# Socket-Activated Launch with Drop-in Config

## Problem

qswm currently runs as an always-on systemd service. For daily use on a workstation, it should only run when needed — start on first browser visit, stop when idle — like Cockpit.

## Approach

Adopt systemd socket activation with idle auto-shutdown and a drop-in config directory.

## Architecture

### Systemd units

| Unit | Purpose |
|------|---------|
| `qswm.socket` | Listens on port 9091, starts the service on first connection |
| `qswm.service` | The web server, started by socket activation |
| `qswm-idle.timer` | Periodic timer (every 2 min) that checks idle state and stops service |

### Startup flow

1. At boot, only `qswm.socket` is active (near-zero resources)
2. Browser hits `localhost:9091` → systemd starts `qswm.service`
3. `qswm` receives the socket fd from systemd via `sd_listen_fds()` instead of binding its own
4. `qswm-idle.timer` runs a check; if no connections for 10 min, it stops the service
5. Socket remains listening — next browser visit restarts everything

### Config system

- Defaults compiled into the binary (port 9091, timeout 600s, URI `qemu:///system`)
- `/etc/qswm/conf.d/*.conf` — INI-style drop-in overrides, read in alphabetical order
- Later files override earlier ones; keys not specified keep defaults

Example: `/etc/qswm/conf.d/10-custom.conf`:

```ini
[general]
port = 8080
idle_timeout = 300
uri = qemu:///system
static_dir = /usr/local/share/qswm/static
```

## Code Changes

### 1. Socket activation support (`server.c`)

Detect if launched via socket activation using `sd_listen_fds()` from `libsystemd`. If an fd is passed, use it directly instead of creating and binding a new socket. Fall back to normal bind if not socket-activated (for development use).

### 2. Config parser (`config.c` / `config.h`)

New module that:
- Defines a config struct with all options and their defaults
- Scans `/etc/qswm/conf.d/*.conf` in alphabetical order
- Parses simple INI format (`key = value` under `[general]`)
- Merges with defaults; CLI args override config file values

### 3. Idle tracking

Track timestamp of last HTTP request in the server. Expose idle duration via:
- A lightweight endpoint `/api/idle-check` returning seconds since last request, OR
- An exit-based approach: a helper script calls the endpoint and runs `systemctl stop qswm.service` if idle exceeds threshold

### 4. New systemd units

**`qswm.socket`:**
```ini
[Unit]
Description=QEMU Snapshot Web Manager Socket

[Socket]
ListenStream=9091
Accept=no
NoDelay=yes

[Install]
WantedBy=sockets.target
```

**`qswm.service` (updated):**
```ini
[Unit]
Description=QEMU Snapshot Web Manager
Documentation=https://github.com/user/qemu-snapshot-web-manager
After=libvirtd.service network.target
Wants=libvirtd.service
Requires=qswm.socket

[Service]
Type=simple
ExecStart=/usr/local/bin/qswm --static-dir /usr/local/share/qswm/static
Restart=on-failure
RestartSec=5
User=root
Environment=HOME=/root

ProtectSystem=strict
ReadWritePaths=/var/lib/libvirt
ProtectHome=yes
NoNewPrivileges=no
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

**`qswm-idle.timer`:**
```ini
[Unit]
Description=Check qswm idle state

[Timer]
OnBootSec=5min
OnUnitActiveSec=2min

[Install]
WantedBy=timers.target
```

**`qswm-idle-check.service`:**
```ini
[Unit]
Description=Stop qswm if idle

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/qswm/idle-check.sh
```

### 5. Makefile updates

- Install `qswm.socket`, `qswm-idle.timer`, `qswm-idle-check.service`
- Install `idle-check.sh` to `/usr/local/libexec/qswm/`
- Create `/etc/qswm/conf.d/` directory
- Add `enable` / `disable` targets for socket activation

### 6. Documentation updates

- Update README installation and usage sections
- Document config options and drop-in directory
- Add example config snippet

## Config Options Reference

| Key | Default | Description |
|-----|---------|-------------|
| `port` | `9091` | HTTP listen port |
| `idle_timeout` | `600` | Seconds of inactivity before auto-shutdown (0 = disabled) |
| `uri` | `qemu:///system` | Libvirt connection URI |
| `static_dir` | `/usr/local/share/qswm/static` | Path to static assets |
