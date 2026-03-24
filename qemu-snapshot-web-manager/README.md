# QEMU Snapshot Web Manager (qswm)

**A lightweight web UI for managing QEMU/KVM virtual machine snapshots with tree visualization**

<!-- Badges: build status, license, etc. -->

---

## Features

- **Visual snapshot tree** — interactive D3.js tree visualization, similar to VirtualBox's snapshot view
- **Internal & external snapshot management** — create, delete, revert, and merge snapshots
- **Auto-merge for external snapshots** — block commit support to flatten snapshot chains
- **Shared folder detection** — discovers virtiofs shared directories
- **VM lifecycle controls** — start, stop, and pause virtual machines
- **Real-time status updates** — keeps the UI in sync with VM state
- **Cross-platform** — Linux (libvirt/KVM) + macOS (libvirt/HVF, UTM planned)
- **Zero JS toolchain** — no npm, no node, no build step — just C and a browser
- **Tiny footprint** — single ~100KB binary, 14KB of HTMX + D3 loaded from CDN

## Screenshots

<!-- Screenshots coming soon -->

## Quick Start

```bash
# Install dependencies (Fedora)
sudo dnf install gcc make pkg-config libmicrohttpd-devel libvirt-devel jansson-devel

# Build
make

# Run (requires root for qemu:///system)
sudo ./build/qswm --port 9091 --static-dir ./static

# Open browser
xdg-open http://localhost:9091
```

## Installation

### Build from source

```bash
git clone https://github.com/user/qemu-snapshot-web-manager.git
cd qemu-snapshot-web-manager
make
```

### System-wide install

```bash
sudo make install    # copies binary to /usr/local/bin, static files to /usr/local/share/qswm, systemd unit
sudo systemctl enable --now qemu-snapshot-web-manager
```

The `make install` target installs:
- `qswm` binary → `/usr/local/bin/qswm`
- Static assets → `/usr/local/share/qswm/static/`
- Systemd service → `/etc/systemd/system/qemu-snapshot-web-manager.service`

### macOS

```bash
brew install libmicrohttpd libvirt jansson pkg-config
make
sudo ./build/qswm --port 9091 --static-dir ./static
```

## Usage

### Command-line options

| Flag | Default | Description |
|------|---------|-------------|
| `--port`, `-p` | `9091` | HTTP listen port |
| `--static-dir`, `-s` | `./static` | Path to static assets directory |
| `--uri`, `-u` | `qemu:///system` | Libvirt connection URI |
| `--help`, `-h` | — | Show usage information |

### Web UI overview

The interface uses a three-panel layout:

1. **Left panel** — VM list with status indicators and lifecycle controls
2. **Center panel** — interactive snapshot tree rendered with D3.js
3. **Right panel** — snapshot details, creation form, and action buttons

### Snapshot operations

- **Create** — choose between internal (single-file, requires VM pause) or external (copy-on-write overlay, live-capable) snapshots
- **Revert** — restore a VM to a previous snapshot state
- **Delete** — remove a snapshot from the tree
- **Merge** — flatten an external snapshot back into its backing file via block commit

### Internal vs external snapshots

**Internal snapshots** store both disk and memory state inside the qcow2 image file itself. They are simple and self-contained but require the VM to be paused during creation.

**External snapshots** create a new overlay qcow2 file that records changes relative to the original image. They support live creation without pausing the VM, but create a chain of files that may need to be merged (block commit) to reclaim space and simplify the tree.

## Architecture

```
┌──────────────┐       ┌──────────────────┐       ┌──────────┐
│   Browser    │──────▶│  C backend       │──────▶│ libvirt  │
│  HTMX + D3  │◀──────│  (libmicrohttpd) │◀──────│  daemon  │
└──────────────┘ HTML  └──────────────────┘       └──────────┘
                 frag.    REST API + static
```

- **C backend** — [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) serves both the REST API and static files
- **HTMX** — returns HTML fragments for partial-page UI updates (no SPA framework needed)
- **D3.js** — renders an interactive snapshot tree from a JSON endpoint
- **Backend abstraction** — a hypervisor abstraction layer allows supporting multiple backends (libvirt, UTM)
- **Codebase size** — ~2000 lines of C, ~300 lines of JavaScript

## API Reference

| Method | Endpoint | Description |
|--------|----------|-------------|
| `GET` | `/api/ping` | Health check |
| `GET` | `/api/vms` | List VMs (HTML) |
| `POST` | `/api/vms/{name}/start` | Start VM |
| `POST` | `/api/vms/{name}/stop` | Stop VM |
| `POST` | `/api/vms/{name}/pause` | Pause VM |
| `GET` | `/api/vms/{name}/snapshots` | Snapshot tree (JSON) |
| `GET` | `/api/vms/{name}/snapshots/{snap}` | Snapshot detail (HTML) |
| `GET` | `/api/vms/{name}/snapshots/form` | Create snapshot form (HTML) |
| `POST` | `/api/vms/{name}/snapshots` | Create snapshot |
| `DELETE` | `/api/vms/{name}/snapshots/{snap}` | Delete snapshot |
| `POST` | `/api/vms/{name}/snapshots/{snap}/revert` | Revert to snapshot |
| `POST` | `/api/vms/{name}/snapshots/{snap}/merge` | Merge external snapshot |
| `GET` | `/api/vms/{name}/shared-folders` | List shared folders (HTML) |

## Known Limitations

- **UTM snapshots not supported** — `utmctl` has no snapshot management commands
- **No authentication** — designed for localhost use; do not expose to untrusted networks
- **No WebSocket** — UI updates are polling-based, not push-based
- **Single-disk assumption** — external snapshot merge (block commit) assumes VMs have a single disk
- **Requires root** — `qemu:///system` connections require root privileges

## Development

```bash
make            # release build (with optimizations)
make debug      # debug build with symbols and sanitizers
make test       # run unit tests
make clean      # remove build artifacts
```

### Project structure

```
qemu-snapshot-web-manager/
├── src/                  # C source files
│   ├── main.c            # entry point, argument parsing, HTTP server setup
│   ├── api_handlers.c    # REST API route handlers
│   ├── snapshot_manager.c # snapshot CRUD operations via libvirt
│   ├── hypervisor.c      # hypervisor abstraction layer
│   └── ...
├── static/               # frontend assets (HTML, CSS, JS)
│   ├── index.html
│   ├── app.js            # HTMX interactions + D3 snapshot tree
│   └── style.css
├── systemd/              # systemd service file
├── tests/                # unit tests
├── Makefile
└── README.md
```

## License

MIT License — see [LICENSE](LICENSE) for details.

## Credits

Built with these excellent libraries:

- [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) — embedded HTTP server
- [libvirt](https://libvirt.org/) — virtualization API
- [jansson](https://github.com/akheron/jansson) — JSON library for C
- [HTMX](https://htmx.org/) — HTML-over-the-wire interactions
- [D3.js](https://d3js.org/) — data-driven DOM visualization
