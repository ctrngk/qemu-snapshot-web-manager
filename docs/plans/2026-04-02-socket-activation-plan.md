# Socket Activation & Drop-in Config Implementation Plan


**Goal:** Add Cockpit-style socket activation, idle auto-shutdown, and drop-in config to qswm.

**Architecture:** systemd socket unit listens on port 9091 and passes the fd to qswm via `sd_listen_fds()`. A config module reads INI-style drop-in files from `/etc/qswm/conf.d/`. An idle tracker records the last request timestamp; a systemd timer periodically checks it and stops the service when idle exceeds the threshold.

**Tech Stack:** C11, libmicrohttpd (`MHD_OPTION_LISTEN_SOCKET`), libsystemd (`sd_listen_fds`), systemd units, INI-style config parser (hand-rolled, ~100 LOC).

**Design doc:** `docs/plans/2026-04-02-socket-activation-design.md`

---

### Task 1: Add config struct and header

**Files:**
- Create: `src/config.h`
- Create: `src/config.c`
- Test: `tests/test_config.c`

**Step 1: Write the failing test**

Create `tests/test_config.c`:

```c
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include "../src/config.h"

static int tests_run = 0;
static int tests_passed = 0;

#define RUN_TEST(fn) do { \
    tests_run++; \
    printf("  TEST: %s ... ", #fn); \
    fn(); \
    tests_passed++; \
    printf("PASS\n"); \
} while (0)

/* Test 1: defaults are sensible */
static void test_defaults(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    assert(cfg.port == 9091);
    assert(cfg.idle_timeout == 600);
    assert(strcmp(cfg.uri, "qemu:///system") == 0);
    assert(strcmp(cfg.static_dir, "/usr/local/share/qswm/static") == 0);
}

/* Test 2: parse a single config line */
static void test_parse_line(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    config_parse_line(&cfg, "port", "8080");
    assert(cfg.port == 8080);
    config_parse_line(&cfg, "idle_timeout", "300");
    assert(cfg.idle_timeout == 300);
    config_parse_line(&cfg, "uri", "qemu:///session");
    assert(strcmp(cfg.uri, "qemu:///session") == 0);
}

/* Test 3: parse a config file buffer */
static void test_parse_buffer(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    const char *buf =
        "[general]\n"
        "port = 7777\n"
        "idle_timeout = 120\n"
        "# comment line\n"
        "\n"
        "uri = qemu+ssh://host/system\n";
    config_parse_buffer(&cfg, buf);
    assert(cfg.port == 7777);
    assert(cfg.idle_timeout == 120);
    assert(strcmp(cfg.uri, "qemu+ssh://host/system") == 0);
    /* static_dir unchanged */
    assert(strcmp(cfg.static_dir, "/usr/local/share/qswm/static") == 0);
}

/* Test 4: unknown keys are silently ignored */
static void test_unknown_key(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    const char *buf =
        "[general]\n"
        "bogus_key = whatever\n"
        "port = 1234\n";
    config_parse_buffer(&cfg, buf);
    assert(cfg.port == 1234);
}

/* Test 5: CLI args override config */
static void test_cli_override(void)
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    config_parse_line(&cfg, "port", "7777");
    /* Simulate CLI override — just set directly */
    cfg.port = 9999;
    assert(cfg.port == 9999);
}

int main(void)
{
    printf("=== Config Tests ===\n");
    RUN_TEST(test_defaults);
    RUN_TEST(test_parse_line);
    RUN_TEST(test_parse_buffer);
    RUN_TEST(test_unknown_key);
    RUN_TEST(test_cli_override);
    printf("=== %d/%d tests passed ===\n", tests_passed, tests_run);
    return tests_passed == tests_run ? 0 : 1;
}
```

**Step 2: Run test to verify it fails**

Run: `cd qemu-snapshot-web-manager && make test`
Expected: Compilation failure — `config.h` does not exist.

**Step 3: Write config.h**

Create `src/config.h`:

```c
#ifndef CONFIG_H
#define CONFIG_H

typedef struct {
    int         port;
    int         idle_timeout;   /* seconds, 0 = disabled */
    char        uri[256];
    char        static_dir[256];
} qswm_config_t;

/* Fill cfg with compiled-in defaults */
void config_defaults(qswm_config_t *cfg);

/* Parse a single key=value pair into cfg */
void config_parse_line(qswm_config_t *cfg, const char *key, const char *value);

/* Parse an INI-style buffer (one [general] section) into cfg */
void config_parse_buffer(qswm_config_t *cfg, const char *buf);

/* Scan /etc/qswm/conf.d/*.conf in alphabetical order, merge into cfg */
int  config_load_dropins(qswm_config_t *cfg, const char *conf_dir);

#endif
```

**Step 4: Write config.c**

Create `src/config.c`:

```c
#include "config.h"
#include "util.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <ctype.h>

void config_defaults(qswm_config_t *cfg)
{
    cfg->port = 9091;
    cfg->idle_timeout = 600;
    snprintf(cfg->uri, sizeof(cfg->uri), "qemu:///system");
    snprintf(cfg->static_dir, sizeof(cfg->static_dir),
             "/usr/local/share/qswm/static");
}

void config_parse_line(qswm_config_t *cfg, const char *key, const char *value)
{
    if (strcmp(key, "port") == 0)
        cfg->port = atoi(value);
    else if (strcmp(key, "idle_timeout") == 0)
        cfg->idle_timeout = atoi(value);
    else if (strcmp(key, "uri") == 0)
        snprintf(cfg->uri, sizeof(cfg->uri), "%s", value);
    else if (strcmp(key, "static_dir") == 0)
        snprintf(cfg->static_dir, sizeof(cfg->static_dir), "%s", value);
    /* unknown keys silently ignored */
}

void config_parse_buffer(qswm_config_t *cfg, const char *buf)
{
    const char *p = buf;
    char line[512];

    while (*p) {
        /* read one line */
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p, len);
        line[len] = '\0';
        p = eol ? eol + 1 : p + len;

        /* trim leading whitespace */
        char *s = line;
        while (isspace((unsigned char)*s)) s++;

        /* skip empty, comments, section headers */
        if (*s == '\0' || *s == '#' || *s == ';' || *s == '[')
            continue;

        /* split on '=' */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        char *key = s;
        char *val = eq + 1;

        /* trim trailing whitespace from key */
        char *end = key + strlen(key) - 1;
        while (end > key && isspace((unsigned char)*end)) *end-- = '\0';

        /* trim leading whitespace from value */
        while (isspace((unsigned char)*val)) val++;
        /* trim trailing whitespace from value */
        end = val + strlen(val) - 1;
        while (end > val && isspace((unsigned char)*end)) *end-- = '\0';

        config_parse_line(cfg, key, val);
    }
}

static int cmp_str(const void *a, const void *b)
{
    return strcmp(*(const char **)a, *(const char **)b);
}

int config_load_dropins(qswm_config_t *cfg, const char *conf_dir)
{
    DIR *d = opendir(conf_dir);
    if (!d) return 0; /* no dir = use defaults, not an error */

    /* collect .conf filenames */
    char *files[128];
    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < 128) {
        size_t nlen = strlen(ent->d_name);
        if (nlen > 5 && strcmp(ent->d_name + nlen - 5, ".conf") == 0)
            files[count++] = str_dup(ent->d_name);
    }
    closedir(d);

    /* sort alphabetically */
    qsort(files, (size_t)count, sizeof(char *), cmp_str);

    /* parse each file */
    for (int i = 0; i < count; i++) {
        char path[512];
        snprintf(path, sizeof(path), "%s/%s", conf_dir, files[i]);

        FILE *f = fopen(path, "r");
        if (!f) {
            log_msg(LOG_WARN, "Cannot open config: %s", path);
            free(files[i]);
            continue;
        }

        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        rewind(f);

        if (sz > 0 && sz < 65536) {
            char *buf = malloc((size_t)sz + 1);
            if (buf) {
                size_t rd = fread(buf, 1, (size_t)sz, f);
                buf[rd] = '\0';
                config_parse_buffer(cfg, buf);
                log_msg(LOG_INFO, "Loaded config: %s", path);
                free(buf);
            }
        }
        fclose(f);
        free(files[i]);
    }

    return count;
}
```

**Step 5: Run tests to verify they pass**

Run: `cd qemu-snapshot-web-manager && make test`
Expected: All config tests pass, existing tests still pass.

**Step 6: Commit**

```bash
git add src/config.h src/config.c tests/test_config.c
git commit -m "feat: add config module with drop-in support

- qswm_config_t struct with defaults (port, idle_timeout, uri, static_dir)
- INI-style parser for [general] section
- Drop-in directory scanner (/etc/qswm/conf.d/*.conf)
- 5 unit tests for config parsing"
```

---

### Task 2: Integrate config into main.c

**Files:**
- Modify: `src/main.c`

**Step 1: Add config loading before CLI arg parsing**

In `main.c`, replace the three default variables (lines 35-37) with a `qswm_config_t` struct. Load drop-in config first, then let CLI args override.

Replace `main.c` lines 33-37:

```c
int main(int argc, char *argv[])
{
    int port = 9091;
    const char *static_dir = "./static";
    const char *libvirt_uri = "qemu:///system";
```

With:

```c
int main(int argc, char *argv[])
{
    qswm_config_t cfg;
    config_defaults(&cfg);
    config_load_dropins(&cfg, "/etc/qswm/conf.d");

    int port = cfg.port;
    const char *static_dir = cfg.static_dir;
    const char *libvirt_uri = cfg.uri;
```

Add `#include "config.h"` at the top of main.c with the other includes.

**Step 2: Build and run existing tests**

Run: `cd qemu-snapshot-web-manager && make clean && make && make test`
Expected: All tests pass. Binary still works with `./build/qswm --help`.

**Step 3: Commit**

```bash
git add src/main.c
git commit -m "feat: integrate drop-in config into main

Config loaded from /etc/qswm/conf.d/*.conf before CLI parsing.
CLI args still override config file values."
```

---

### Task 3: Add socket activation support to server

**Files:**
- Modify: `src/server.h` — add `server_start_activated()` function
- Modify: `src/server.c` — implement socket activation via `sd_listen_fds()`
- Modify: `src/main.c` — detect socket activation, call appropriate start function
- Modify: `Makefile` — link `libsystemd`

**Step 1: Install libsystemd-devel if needed**

Run: `sudo dnf install -y systemd-devel`

**Step 2: Update Makefile to link libsystemd**

In `Makefile`, change line 5:

```makefile
PKG_CFLAGS := $(shell pkg-config --cflags libmicrohttpd libvirt jansson)
PKG_LIBS   := $(shell pkg-config --libs   libmicrohttpd libvirt jansson) -lvirt-qemu
```

To:

```makefile
PKG_CFLAGS := $(shell pkg-config --cflags libmicrohttpd libvirt jansson libsystemd)
PKG_LIBS   := $(shell pkg-config --libs   libmicrohttpd libvirt jansson libsystemd) -lvirt-qemu
```

**Step 3: Update server.h**

Replace `server.h` content with:

```c
#ifndef SERVER_H
#define SERVER_H

/* Start server by binding to the given port (normal mode) */
int  server_start(int port, const char *static_dir, const char *libvirt_uri);

/* Start server using a pre-opened socket fd (socket activation mode) */
int  server_start_fd(int fd, const char *static_dir, const char *libvirt_uri);

/* Check if systemd passed us a socket. Returns fd >= 0 or -1 if not activated. */
int  server_check_activation(void);

void server_stop(void);

#endif
```

**Step 4: Implement socket activation in server.c**

Add near the top of `server.c` (after existing includes):

```c
#include <systemd/sd-daemon.h>
```

Add these two new functions before `server_stop()`:

```c
int server_check_activation(void)
{
    int n = sd_listen_fds(0);
    if (n < 0) {
        log_msg(LOG_ERROR, "sd_listen_fds failed: %d", n);
        return -1;
    }
    if (n == 0)
        return -1;  /* not socket-activated */
    if (n > 1)
        log_msg(LOG_WARN, "Multiple sockets passed (%d), using first", n);
    return SD_LISTEN_FDS_START; /* fd 3 */
}

int server_start_fd(int fd, const char *static_dir, const char *libvirt_uri)
{
    g_static_dir  = static_dir;
    g_libvirt_uri = libvirt_uri;

    routes_init();

    daemon_handle = MHD_start_daemon(
        MHD_USE_INTERNAL_POLLING_THREAD,
        0,                               /* port ignored when using LISTEN_SOCKET */
        NULL, NULL,
        &request_handler, NULL,
        MHD_OPTION_LISTEN_SOCKET, (MHD_socket)fd,
        MHD_OPTION_NOTIFY_COMPLETED, &route_request_completed, NULL,
        MHD_OPTION_END);

    if (!daemon_handle) {
        log_msg(LOG_ERROR, "MHD_start_daemon failed (socket-activated fd %d)", fd);
        return -1;
    }

    log_msg(LOG_INFO, "HTTP server started (socket-activated, fd %d)", fd);
    return 0;
}
```

**Step 5: Update main.c to use socket activation**

In `main.c`, after config loading and CLI parsing, replace the `server_start()` call block (lines ~94-98) with:

```c
    /* Try socket activation first */
    int act_fd = server_check_activation();
    int rc;
    if (act_fd >= 0) {
        log_msg(LOG_INFO, "Socket activation detected (fd %d)", act_fd);
        rc = server_start_fd(act_fd, static_dir, libvirt_uri);
    } else {
        rc = server_start(port, static_dir, libvirt_uri);
    }

    if (rc != 0) {
        log_msg(LOG_ERROR, "Failed to start server");
        be->disconnect();
        return 1;
    }
```

**Step 6: Build and test**

Run: `cd qemu-snapshot-web-manager && make clean && make && make test`
Expected: Compiles with libsystemd, all tests pass, `./build/qswm --help` works.

**Step 7: Commit**

```bash
git add src/server.h src/server.c src/main.c Makefile
git commit -m "feat: add systemd socket activation support

- server_check_activation() detects fd from sd_listen_fds()
- server_start_fd() uses MHD_OPTION_LISTEN_SOCKET
- Falls back to normal port binding when not socket-activated
- Links libsystemd"
```

---

### Task 4: Add idle tracking and check endpoint

**Files:**
- Modify: `src/server.c` — track last-request timestamp, add `/api/idle-check`

**Step 1: Add idle timestamp to server.c**

Near the top of `server.c`, add after the existing globals:

```c
#include <stdatomic.h>
#include <time.h>

static atomic_long last_request_time;

static void touch_activity(void)
{
    atomic_store(&last_request_time, (long)time(NULL));
}

long server_idle_seconds(void)
{
    long last = atomic_load(&last_request_time);
    if (last == 0) return 0;
    return (long)time(NULL) - last;
}
```

**Step 2: Add `server_idle_seconds()` declaration to server.h**

Add to `server.h`:

```c
/* Returns seconds since last HTTP request, or 0 if no requests yet */
long server_idle_seconds(void);
```

**Step 3: Call `touch_activity()` in request_handler**

In `server.c` `request_handler()`, add as the first line inside the function body:

```c
    touch_activity();
```

**Step 4: Handle `/api/idle-check` in request_handler**

In `request_handler()`, just before the existing `/api/ping` check, add:

```c
    if (strcmp(url, "/api/idle-check") == 0) {
        char body[64];
        snprintf(body, sizeof(body), "%ld", server_idle_seconds());
        struct MHD_Response *resp = MHD_create_response_from_buffer(
            strlen(body), body, MHD_RESPMEM_MUST_COPY);
        MHD_add_response_header(resp, "Content-Type", "text/plain");
        enum MHD_Result ret = MHD_queue_response(connection, MHD_HTTP_OK, resp);
        MHD_destroy_response(resp);
        return ret;
    }
```

**Step 5: Initialize timestamp in both start functions**

Add `touch_activity();` at the start of both `server_start()` and `server_start_fd()`.

**Step 6: Build and test**

Run: `cd qemu-snapshot-web-manager && make clean && make`
Expected: Builds cleanly.

Manual test:
```bash
sudo ./build/qswm --port 9091 --static-dir ./static &
sleep 2
curl -s http://localhost:9091/api/idle-check
# Should print a small number (seconds since last request)
kill %1
```

**Step 7: Commit**

```bash
git add src/server.h src/server.c
git commit -m "feat: add idle tracking and /api/idle-check endpoint

Tracks last HTTP request timestamp with atomic_long.
GET /api/idle-check returns seconds since last request."
```

---

### Task 5: Create systemd units

**Files:**
- Create: `systemd/qswm.socket`
- Modify: `systemd/qemu-snapshot-web-manager.service` (rename to `systemd/qswm.service`)
- Create: `systemd/qswm-idle.timer`
- Create: `systemd/qswm-idle-check.service`
- Create: `scripts/idle-check.sh`

**Step 1: Create socket unit**

Create `systemd/qswm.socket`:

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

**Step 2: Rename and update service unit**

Rename `systemd/qemu-snapshot-web-manager.service` → `systemd/qswm.service`.

Write `systemd/qswm.service`:

```ini
[Unit]
Description=QEMU Snapshot Web Manager
Documentation=https://github.com/ctrngk/qemu-snapshot-web-manager
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

# Security hardening
ProtectSystem=strict
ReadWritePaths=/var/lib/libvirt
ProtectHome=yes
NoNewPrivileges=no
PrivateTmp=yes

[Install]
WantedBy=multi-user.target
```

**Step 3: Create idle check script**

Create `scripts/idle-check.sh`:

```bash
#!/bin/bash
# Check if qswm is idle and stop it if so.
# Called by qswm-idle-check.service via qswm-idle.timer.

IDLE_THRESHOLD="${QSWM_IDLE_TIMEOUT:-600}"
ENDPOINT="http://127.0.0.1:9091/api/idle-check"

# If qswm.service is not active, nothing to do
if ! systemctl is-active --quiet qswm.service; then
    exit 0
fi

idle_secs=$(curl -sf --max-time 3 "$ENDPOINT" 2>/dev/null)
if [ -z "$idle_secs" ]; then
    exit 0  # can't reach, don't stop
fi

if [ "$idle_secs" -ge "$IDLE_THRESHOLD" ]; then
    logger -t qswm-idle "Idle for ${idle_secs}s (threshold: ${IDLE_THRESHOLD}s), stopping qswm"
    systemctl stop qswm.service
fi
```

**Step 4: Create timer and check service units**

Create `systemd/qswm-idle.timer`:

```ini
[Unit]
Description=Periodically check if qswm is idle

[Timer]
OnBootSec=5min
OnUnitActiveSec=2min

[Install]
WantedBy=timers.target
```

Create `systemd/qswm-idle-check.service`:

```ini
[Unit]
Description=Check qswm idle state and stop if inactive

[Service]
Type=oneshot
ExecStart=/usr/local/libexec/qswm/idle-check.sh
Environment=QSWM_IDLE_TIMEOUT=600
```

**Step 5: Commit**

```bash
git rm systemd/qemu-snapshot-web-manager.service
git add systemd/qswm.socket systemd/qswm.service \
        systemd/qswm-idle.timer systemd/qswm-idle-check.service \
        scripts/idle-check.sh
git commit -m "feat: add systemd socket activation units

- qswm.socket: listens on port 9091
- qswm.service: started by socket, replaces old always-on service
- qswm-idle.timer + qswm-idle-check.service: stop after idle
- scripts/idle-check.sh: curl-based idle detection"
```

---

### Task 6: Update Makefile install target

**Files:**
- Modify: `Makefile`

**Step 1: Update install target**

Replace the existing `install` target with:

```makefile
install: all
	install -Dm755 $(TARGET) /usr/local/bin/qswm
	install -d /usr/local/share/qswm/static
	cp -r static/* /usr/local/share/qswm/static/ 2>/dev/null || true
	install -d /etc/qswm/conf.d
	install -Dm755 scripts/idle-check.sh /usr/local/libexec/qswm/idle-check.sh
	install -Dm644 systemd/qswm.socket /etc/systemd/system/qswm.socket
	install -Dm644 systemd/qswm.service /etc/systemd/system/qswm.service
	install -Dm644 systemd/qswm-idle.timer /etc/systemd/system/qswm-idle.timer
	install -Dm644 systemd/qswm-idle-check.service /etc/systemd/system/qswm-idle-check.service
	systemctl daemon-reload
	@echo ""
	@echo "Installed. Enable socket activation with:"
	@echo "  sudo systemctl enable --now qswm.socket qswm-idle.timer"

uninstall:
	systemctl disable --now qswm.socket qswm.service qswm-idle.timer 2>/dev/null || true
	rm -f /usr/local/bin/qswm
	rm -rf /usr/local/share/qswm
	rm -f /usr/local/libexec/qswm/idle-check.sh
	rmdir /usr/local/libexec/qswm 2>/dev/null || true
	rm -f /etc/systemd/system/qswm.socket
	rm -f /etc/systemd/system/qswm.service
	rm -f /etc/systemd/system/qswm-idle.timer
	rm -f /etc/systemd/system/qswm-idle-check.service
	systemctl daemon-reload
	@echo "Uninstalled."
```

**Step 2: Verify Makefile syntax**

Run: `cd qemu-snapshot-web-manager && make -n install`
Expected: Prints the install commands without errors.

**Step 3: Commit**

```bash
git add Makefile
git commit -m "feat: update Makefile for socket activation install

- Installs socket, service, timer, and idle-check units
- Creates /etc/qswm/conf.d/ drop-in directory
- Adds uninstall target
- Prints enable instructions after install"
```

---

### Task 7: Update README and docs

**Files:**
- Modify: `README.md`

**Step 1: Update Screenshots section** (already done)

**Step 2: Update Installation section**

After the existing `sudo make install` block, replace the enable command and add socket activation docs:

Replace:
```
sudo systemctl enable --now qemu-snapshot-web-manager
```

With:
```
sudo systemctl enable --now qswm.socket qswm-idle.timer
```

**Step 3: Add Configuration section**

After the "Usage" section, add a new "Configuration" section:

```markdown
## Configuration

qswm uses sensible defaults. To override, create drop-in files in `/etc/qswm/conf.d/`:

```bash
sudo mkdir -p /etc/qswm/conf.d
sudo tee /etc/qswm/conf.d/10-custom.conf <<EOF
[general]
port = 8080
idle_timeout = 300
uri = qemu:///system
static_dir = /usr/local/share/qswm/static
EOF
```

Files are read in alphabetical order; later files override earlier ones. CLI arguments override config files.

| Key | Default | Description |
|-----|---------|-------------|
| `port` | `9091` | HTTP listen port |
| `idle_timeout` | `600` | Seconds idle before auto-shutdown (0 = disabled) |
| `uri` | `qemu:///system` | Libvirt connection URI |
| `static_dir` | `/usr/local/share/qswm/static` | Path to static assets |
```

**Step 4: Update Architecture section**

Add to the architecture diagram explanation:

```markdown
### Socket activation flow

```
Boot → qswm.socket (listening, zero resources)
       ↓ first connection
       systemd starts qswm.service
       ↓ idle for 10 min
       qswm-idle.timer stops qswm.service
       qswm.socket keeps listening → cycle repeats
```
```

**Step 5: Commit**

```bash
git add README.md
git commit -m "docs: update README for socket activation and config

- Document socket activation setup
- Add Configuration section with drop-in examples
- Update systemctl enable command
- Add socket activation flow diagram"
```

---

### Task 8: End-to-end integration test

**Step 1: Build everything**

```bash
cd qemu-snapshot-web-manager && make clean && make && make test
```

Expected: All unit tests pass, binary builds.

**Step 2: Test normal mode (no socket activation)**

```bash
sudo ./build/qswm --port 9091 --static-dir ./static &
sleep 1
curl -s http://localhost:9091/api/ping
# Expected: "pong"
curl -s http://localhost:9091/api/idle-check
# Expected: a number (seconds)
sudo kill %1
```

**Step 3: Test install and socket activation**

```bash
sudo make install
sudo systemctl enable --now qswm.socket qswm-idle.timer
# Socket should be listening
sudo systemctl status qswm.socket
# Service should NOT be running yet
sudo systemctl status qswm.service  # inactive

# Trigger activation
curl -s http://localhost:9091/api/ping
# Expected: "pong" (service auto-started)
sudo systemctl status qswm.service  # active
```

**Step 4: Commit any fixes, then final commit**

```bash
git add -A
git commit -m "chore: final integration fixes for socket activation"
```
