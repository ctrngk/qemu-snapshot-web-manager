# Developer Guide — QEMU Snapshot Web Manager

> For developers who want to understand, build, modify, or contribute to the project.

---

## 1. Project Overview

QEMU Snapshot Web Manager is a lightweight web UI for managing QEMU/KVM virtual machine snapshots. It is a single C binary that serves both a REST API and static frontend assets.

### Architecture

```
Browser (HTMX + D3.js)
  ↔ C backend (libmicrohttpd REST server + static file serving)
  ↔ libvirt daemon (qemu:///system)
```

- **C backend** — libmicrohttpd HTTP server exposing REST endpoints. Returns HTML fragments for HTMX and JSON for the D3.js snapshot tree.
- **HTMX frontend** — Server-rendered HTML fragments swapped into the DOM. No client-side templating, no SPA framework.
- **D3.js visualization** — Interactive snapshot tree rendered from a JSON endpoint using `d3.hierarchy()` + `d3.tree()` + `d3.zoom()`.

### Design Philosophy

- **No npm, no Node.js, no JS build toolchain.** The frontend is plain HTML, CSS, and vanilla JS. HTMX and D3.js are loaded from CDN.
- **Single C binary.** One compilation unit, one artifact. Server-rendered HTML fragments eliminate the need for a separate frontend build.
- **Minimal dependencies.** Three C libraries (libmicrohttpd, libvirt, jansson), all available via system package managers.

### Key Libraries

| Library | Version | Purpose |
|---------|---------|---------|
| [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/) | 1.0.x | HTTP server (note: 1.0.x API, not the older 0.9.x — different types and constants) |
| [libvirt](https://libvirt.org/) | any | VM and snapshot management via the libvirt C API |
| [jansson](https://github.com/akheron/jansson) | any | JSON serialization for snapshot tree data |
| [HTMX](https://htmx.org/) | 1.9.12 | HTML-over-the-wire frontend interactivity (CDN) |
| [D3.js](https://d3js.org/) | v7 | Snapshot tree visualization (CDN) |

---

## 2. Project Structure

```
qemu-snapshot-web-manager/
├── Makefile                    # Build system (gcc, pkg-config, auto-dependency)
├── README.md                   # Project overview
├── docs/
│   ├── getting-started.md     # Quick start guide
│   ├── user-guide.md          # End-user documentation
│   └── developer-guide.md     # This file
├── scripts/                    # Helper scripts
│   ├── setup.sh               # One-command setup: deps + build + run
│   ├── build.sh               # Build the project
│   ├── dev.sh                 # Development server with auto-rebuild
│   ├── deps.sh                # Check/install build dependencies
│   ├── install.sh             # Install system-wide
│   ├── uninstall.sh           # Remove system-wide install
│   └── test.sh                # Run tests
├── src/
│   ├── main.c                 # Entry point, CLI argument parsing, signal handlers
│   ├── server.h/.c            # HTTP server (libmicrohttpd), static file serving, MIME detection
│   ├── routes.h/.c            # API route dispatcher, request/response helpers
│   ├── vm_backend.h/.c        # Hypervisor abstraction layer (vtable pattern)
│   ├── libvirt_backend.h/.c   # libvirt implementation of vm_backend_t
│   ├── utm_backend.h/.c       # UTM implementation (macOS only, Linux stub)
│   ├── snapshot.h/.c          # Snapshot tree data structure + JSON serialization
│   ├── html_render.h/.c       # HTML fragment generators for HTMX responses
│   └── util.h/.c              # Logging, string helpers, URL decoding
├── static/
│   ├── index.html             # App shell (HTMX attributes, 3-column layout)
│   ├── styles.css             # Full CSS (grid layout, custom properties, responsive)
│   └── tree.js                # D3.js snapshot tree visualization
├── systemd/
│   └── qemu-snapshot-web-manager.service
└── tests/
    ├── test_snapshot_tree.c   # 7 snapshot tree tests
    └── test_html_render.c     # 6 HTML renderer tests
```

---

## 3. Prerequisites

### Fedora / RHEL

```bash
sudo dnf install gcc make pkg-config \
    libmicrohttpd-devel libvirt-devel jansson-devel
```

### Ubuntu / Debian

```bash
sudo apt install gcc make pkg-config \
    libmicrohttpd-dev libvirt-dev libjansson-dev
```

### macOS

```bash
brew install libmicrohttpd libvirt jansson pkg-config
```

### Verify

```bash
pkg-config --modversion libmicrohttpd libvirt jansson
```

All three libraries must be found by `pkg-config` before the build will succeed.

> **Note:** The build also links against `-lvirt-qemu` (from the same `libvirt-devel` / `libvirt-dev` package) for `virDomainQemuAgentCommand()`, which is used to send commands to the QEMU Guest Agent inside VMs.

---

## 4. Building

### Make Targets

```bash
make            # Release build (-O2 optimization)
make debug      # Debug build (-g -O0 -DDEBUG)
make clean      # Remove build/ directory
make test       # Build and run all tests
make run        # Build and run on port 9091
make install    # Install to /usr/local (requires sudo)
```

### Makefile Details

**Compiler flags:**
- Base: `-std=c11 -Wall -Wextra -D_GNU_SOURCE`
- Release: adds `-O2`
- Debug: adds `-g -O0 -DDEBUG`
- Library flags via `pkg-config --cflags` and `pkg-config --libs` for `libmicrohttpd`, `libvirt`, and `jansson`

**Source file discovery:**
- `$(wildcard src/*.c)` auto-includes any new `.c` file added to `src/`. No need to edit the Makefile when adding source files.

**Header dependency tracking:**
- `-MMD -MP` flags generate `.d` dependency files alongside each `.o`
- `-include $(DEPS)` (soft include) pulls these in on subsequent builds
- Effect: changing a header automatically recompiles all files that include it

**Test compilation:**
- Each `tests/*.c` is compiled as a standalone executable
- Links against all compiled `src/*.o` objects **except** `main.o` (avoids duplicate `main()` symbols)
- Tests are auto-discovered — any new `tests/test_*.c` file is picked up

---

## 5. Architecture Deep Dive

### Request Flow

```
Browser
  → HTTP request
  → libmicrohttpd (server.c)
      ├── Static file? → serve_static_file() → MIME detection → response
      └── /api/* path? → route_dispatch() (routes.c)
                             ├── vm_backend vtable → libvirt_backend / utm_backend
                             ├── html_render.c → HTML fragment → HTMX swap in browser
                             └── snapshot.c → JSON → D3.js render in browser
```

### Backend Vtable (`vm_backend.h`)

The hypervisor abstraction uses a vtable (struct of function pointers) to decouple the HTTP layer from any specific hypervisor API.

```c
typedef struct vm_backend {
    const char *name;  // "libvirt" or "utm"

    // Connection lifecycle
    int  (*connect)(const char *uri);
    void (*disconnect)(void);

    // VM listing
    int  (*list_vms)(vm_info_t ***vms, int *count);
    void (*free_vm_list)(vm_info_t **vms, int count);

    // VM lifecycle
    int (*vm_start)(const char *vm_name);
    int (*vm_stop)(const char *vm_name);
    int (*vm_pause)(const char *vm_name);

    // Snapshot operations
    int (*list_snapshots)(const char *vm_name, snapshot_node_t **tree);
    int (*create_snapshot)(const char *vm_name, const char *snap_name,
                           const char *description, snap_type_t type);
    int (*delete_snapshot)(const char *vm_name, const char *snap_name, int auto_merge);
    int (*revert_snapshot)(const char *vm_name, const char *snap_name);
    int (*merge_snapshot)(const char *vm_name, const char *snap_name);

    // Shared folders
    int  (*list_shared_folders)(const char *vm_name, shared_folder_t **folders, int *count);
    void (*free_shared_folders)(shared_folder_t *folders, int count);
    int  (*add_shared_folder)(const char *vm_name, const char *source_dir, const char *mount_tag, const char *fs_type, int read_only);
    int  (*remove_shared_folder)(const char *vm_name, const char *mount_tag);
    int  (*mount_shared_folder)(const char *vm_name, const char *mount_tag, char **error_html);
    int  (*unmount_shared_folder)(const char *vm_name, const char *mount_tag, char **error_html);
    int  (*check_mount_status)(const char *vm_name, shared_folder_t *folders, int count);
} vm_backend_t;
```

**Global singleton pattern:**
- `backend_set(vm_backend_t *be)` — set the active backend (called once at startup)
- `backend_get()` — retrieve the active backend (used by route handlers)

**Key types:**
- `vm_state_t` — `VM_RUNNING`, `VM_PAUSED`, `VM_SHUTOFF`, `VM_OTHER`
- `snap_type_t` — `SNAP_INTERNAL`, `SNAP_EXTERNAL`
- `vm_info_t` — `name`, `uuid`, `state`, `vcpus`, `memory_kb`
- `shared_folder_t` — `source_dir`, `mount_tag`, `fs_type` (`"virtiofs"` or `"9p"`), `read_only`, `mounted` (`1` = mounted, `0` = not mounted, `-1` = unknown/agent unavailable)

**Backend implementations:**
- **libvirt** (`libvirt_backend.c`): Full implementation via the libvirt C API. Accessed via `libvirt_backend_get()`.
- **UTM** (`utm_backend.c`): macOS-only behind `#ifdef __APPLE__`. On Linux, returns NULL. Accessed via `utm_backend_get()`.

### HTTP Server (`server.c`)

Uses the **libmicrohttpd 1.0.x** API (not the older 0.9.x — different types and constants).

**Daemon startup:**

```c
daemon_handle = MHD_start_daemon(
    MHD_USE_THREAD_PER_CONNECTION,
    (uint16_t)port,
    NULL, NULL,                   // no accept policy callback
    &request_handler, NULL,       // request handler + user data
    MHD_OPTION_NOTIFY_COMPLETED, &route_request_completed, NULL,
    MHD_OPTION_END);
```

**Static file serving:**
1. Path traversal protection — rejects URLs containing `..`
2. Index file handling — `/` maps to `/index.html`
3. File stat validation — ensures the path is a regular file
4. Reads entire file into memory, sends via `MHD_create_response_from_buffer()`

**MIME detection** (`guess_mime()`):

| Extension | MIME Type |
|-----------|-----------|
| `.html` | `text/html` |
| `.css` | `text/css` |
| `.js` | `application/javascript` |
| `.json` | `application/json` |
| `.png` | `image/png` |
| `.svg` | `image/svg+xml` |
| `.ico` | `image/x-icon` |
| (default) | `application/octet-stream` |

**Routing logic:** If the URL starts with `/api/`, delegate to `route_dispatch()`. Otherwise, serve static files (GET only; other methods get 405).

### Route Dispatcher (`routes.c`)

**URL parsing:** `path_segment(url, N)` extracts the Nth segment from a URL path. For example, given `/api/vms/ubuntu/snapshots/snap1`:

| Index | Segment |
|-------|---------|
| 0 | `api` |
| 1 | `vms` |
| 2 | `ubuntu` |
| 3 | `snapshots` |
| 4 | `snap1` |

**POST/DELETE body accumulation:** Uses the `con_cls` (connection context) pattern required by libmicrohttpd's callback-based design. The request body is accumulated across multiple invocations of the handler callback, stored in a `request_context_t`.

**Form body parsing:** `form_value(body, key)` parses URL-encoded form bodies (e.g., `name=snap1&description=test`). Values are URL-decoded via `url_decode()`.

**Mutation responses:** Handlers that modify state (start/stop/pause, create/delete/revert/merge snapshot) add the `HX-Trigger: vmStateChanged` response header, allowing the HTMX frontend to react to mutations.

**Response helpers:**
- `send_html(connection, status, body)` — HTML with `text/html; charset=utf-8`
- `send_json(connection, status, body)` — JSON with `application/json`
- `send_html_trigger(connection, status, body, trigger)` — HTML + `HX-Trigger` header
- `send_404(connection)`, `send_405(connection)` — standard error responses

### HTML Renderers (`html_render.c`)

**String builder pattern** — `strbuf_t` for efficient HTML assembly:

```c
typedef struct {
    char  *buf;   // allocated buffer
    size_t len;   // current length
    size_t cap;   // capacity
} strbuf_t;
```

- `sb_init(sb, initial_capacity)` — initialize
- `sb_append(sb, str)` — append string, auto-grows
- `sb_appendf(sb, fmt, ...)` — append formatted (printf-style)
- `sb_finish(sb)` — extract the buffer, invalidate the builder

**XSS prevention:** `html_escape()` sanitizes all user input before rendering:

| Character | Escape |
|-----------|--------|
| `<` | `&lt;` |
| `>` | `&gt;` |
| `&` | `&amp;` |
| `"` | `&quot;` |
| `'` | `&#39;` |

**Rendering functions** (each returns a `malloc()`'d string — caller frees):

| Function | Output |
|----------|--------|
| `render_vm_list()` | VM cards with state badges and HTMX click handlers |
| `render_snapshot_detail()` | Snapshot metadata panel with revert/delete/merge buttons |
| `render_create_snapshot_form()` | Modal form with name/description/type fields |
| `render_shared_folders()` | Shared folder list with mount tags and paths |
| `render_add_shared_folder_form()` | Add shared folder form with directory browser |
| `render_directory_listing()` | Server-side directory browser for modal |
| `render_success()` | `<div class="alert alert-success">` message |
| `render_error()` | `<div class="alert alert-error">` message |
| `render_error_html()` | Rich HTML error message with structured details, SELinux fix instructions, and distro-specific install commands |

HTMX attributes (`hx-get`, `hx-post`, `hx-delete`, `hx-target`, `hx-swap`, `hx-confirm`) are embedded directly in the rendered HTML.

### Snapshot Tree (`snapshot.c`)

**N-ary tree structure:**

```c
typedef struct snapshot_node {
    char *id;                        // unique snapshot name
    char *description;               // user-provided text
    char *creation_time;             // ISO-8601 timestamp
    snap_type_t type;                // SNAP_INTERNAL or SNAP_EXTERNAL
    int is_current;                  // boolean: active snapshot?

    struct snapshot_node *parent;    // NULL for root
    struct snapshot_node **children; // dynamic array
    int child_count;
    int child_capacity;              // grows by doubling (initial: 4)
} snapshot_node_t;
```

**Core operations:**
- `snapshot_node_new()` — allocate node, duplicate all strings
- `snapshot_node_add_child(parent, child)` — append child, double array capacity when full, set `child->parent`
- `snapshot_tree_find(root, id)` — DFS search by ID, returns node or NULL
- `snapshot_tree_count(root)` — recursive count of all nodes
- `snapshot_tree_free(root)` — recursive DFS free of all nodes and their strings

**JSON serialization** (`snapshot_tree_to_json()`):

Produces D3.js-compatible JSON via jansson:

```json
{
  "id": "snap1",
  "name": "snap1",
  "description": "First snapshot",
  "date": "2025-01-15T10:30:00",
  "type": "internal",
  "isCurrent": true,
  "children": [
    { "id": "snap2", "name": "snap2", ... , "children": [] }
  ]
}
```

The `name` field duplicates `id` for D3.js label rendering. Serialization uses `json_dumps(obj, JSON_INDENT(2))` with `json_decref()` cleanup.

### Frontend (`static/`)

**`index.html`** — HTMX app shell with a 3-column CSS Grid layout:

```
┌──────────────┬─────────────────────┬──────────────────┐
│  VM List     │  Snapshot Tree      │  Detail Panel    │
│  (aside)     │  (section)          │  (aside)         │
│              │                     │                  │
│  #vm-list    │  #snapshot-tree     │  #snapshot-detail│
│  hx-get=     │  D3.js canvas       │  #shared-folders │
│  /api/vms    │                     │                  │
│  hx-trigger= │                     │                  │
│  load        │                     │                  │
└──────────────┴─────────────────────┴──────────────────┘
```

Key HTMX attributes: `hx-get`, `hx-post`, `hx-delete`, `hx-trigger`, `hx-target`, `hx-swap`, `hx-confirm`, `hx-on::after-request`.

Key JS functions: `selectVm(vmName)`, `loadSnapshotTree(vmName)`, `showToast(message, type)`.

**`styles.css`** — CSS Grid layout, custom properties for theming, responsive breakpoints.

**`tree.js`** — D3.js v7 snapshot tree: fetches JSON from `/api/vms/{name}/snapshots`, builds `d3.hierarchy()`, lays out with `d3.tree()`, renders SVG with `d3.zoom()` for pan/zoom.

---

## 6. API Reference

All endpoints are under `/api/`. HTML responses are fragments intended for HTMX `innerHTML` swaps. Mutation endpoints include the `HX-Trigger: vmStateChanged` response header.

| Method | Path | Request Body | Response | Notes |
|--------|------|-------------|----------|-------|
| `GET` | `/api/ping` | — | `text/plain` `"pong"` | Health check |
| `GET` | `/api/vms` | — | HTML fragment (VM list) | HTMX target: `#vm-list` |
| `POST` | `/api/vms/{name}/start` | — | HTML success/error | HX-Trigger: `vmStateChanged` |
| `POST` | `/api/vms/{name}/stop` | — | HTML success/error | HX-Trigger: `vmStateChanged` |
| `POST` | `/api/vms/{name}/pause` | — | HTML success/error | HX-Trigger: `vmStateChanged` |
| `GET` | `/api/vms/{name}/snapshots` | — | JSON (snapshot tree) | Consumed by D3.js in `tree.js` |
| `GET` | `/api/vms/{name}/snapshots/{snap}` | — | HTML fragment | Snapshot detail panel |
| `GET` | `/api/vms/{name}/snapshots/form` | — | HTML fragment | Create snapshot form modal |
| `POST` | `/api/vms/{name}/snapshots` | URL-encoded: `name`, `description`, `type` | HTML success/error | HX-Trigger: `vmStateChanged` |
| `DELETE` | `/api/vms/{name}/snapshots/{snap}` | — | HTML success/error | Passes `auto_merge=1` to backend |
| `POST` | `/api/vms/{name}/snapshots/{snap}/revert` | — | HTML success/error | HX-Trigger: `vmStateChanged` |
| `POST` | `/api/vms/{name}/snapshots/{snap}/merge` | — | HTML success/error | Block commit for external snapshots |
| `GET` | `/api/vms/{name}/shared-folders` | — | HTML fragment | Shared folders list for VM; includes real-time mount status via guest agent `check_mount_status()` |
| `GET` | `/api/vms/{name}/shared-folders/form` | — | HTML fragment | Add shared folder form modal |
| `GET` | `/api/browse?path=...` | — | HTML fragment | Server-side directory browser; returns folder listing for the given path |
| `POST` | `/api/vms/{name}/shared-folders` | URL-encoded: `source_dir`, `mount_tag`, `fs_type`, `read_only` | HTML success/error | Add virtiofs/9p shared folder; auto-enables shared memory for virtiofs; HX-Trigger: `vmStateChanged` |
| `DELETE` | `/api/vms/{name}/shared-folders/{tag}` | — | HTML success/error | Detach shared folder; HX-Trigger: `vmStateChanged` |
| `POST` | `/api/vms/{name}/shared-folders/{tag}/mount` | — | HTML success/error | Mount via QEMU Guest Agent (`guest-exec`); HX-Trigger: `vmStateChanged` |
| `POST` | `/api/vms/{name}/shared-folders/{tag}/unmount` | — | HTML success/error | Unmount via QEMU Guest Agent (`guest-exec`); HX-Trigger: `vmStateChanged` |

`{name}` is the VM name (URL-decoded). `{snap}` is the snapshot ID (URL-decoded).

---

## 7. Testing

### Overview

- Test source files live in `tests/`
- Each file is a standalone executable with its own `main()`
- Tests link against all `src/*.o` objects except `main.o`
- `make test` builds and runs all tests

### Current Test Suite

| File | Tests | What It Covers |
|------|-------|----------------|
| `test_snapshot_tree.c` | 7 | Node creation, tree building, DFS find, recursive count, JSON serialization, null field handling, dynamic array growth (20 children) |
| `test_html_render.c` | 6 | VM list rendering, snapshot detail HTML, create form modal, shared folders, success/error messages, XSS escaping |
| **Total** | **13** | |

### Adding a New Test

1. Create `tests/test_xxx.c` with a `main()` function
2. `#include` the headers you need from `src/`
3. Return 0 on success, non-zero on failure
4. Run `make test` — new test files are auto-discovered by the Makefile

No test framework is used. Tests use plain `assert()` or manual checks with `fprintf(stderr, ...)` + `return 1` on failure.

---

## 8. Adding a New Backend

To add support for a new hypervisor (e.g., Proxmox, Xen, Incus):

1. **Create header and source files:**
   ```
   src/mybackend.h
   src/mybackend.c
   ```

2. **Implement all `vm_backend_t` function pointers.** Every field in the struct must be set. If an operation is unsupported, return an error code or set the pointer to a stub that returns `-1`.

3. **Expose a getter function:**
   ```c
   vm_backend_t *mybackend_get(void);
   ```
   This returns a pointer to a static `vm_backend_t` struct populated with your function pointers.

4. **Register in `main.c`.** Add initialization logic, possibly behind an `#ifdef` or a CLI flag:
   ```c
   #ifdef USE_MYBACKEND
   vm_backend_t *be = mybackend_get();
   #else
   vm_backend_t *be = libvirt_backend_get();
   #endif
   backend_set(be);
   ```

5. **Rebuild.** The Makefile's `$(wildcard src/*.c)` auto-discovers the new source file. No Makefile edits needed.

---

## 9. Adding a New API Endpoint

1. **Add a handler function in `routes.c`:**
   ```c
   static enum MHD_Result handle_my_endpoint(
       struct MHD_Connection *connection,
       const char *vm_name)
   {
       // ... build response ...
       return send_html(connection, MHD_HTTP_OK, html);
   }
   ```

2. **Add route matching in `route_dispatch()`.** Use `path_segment()` to extract URL segments and `strcmp()` to match. Follow the existing nested-conditional pattern.

3. **If the endpoint returns HTML**, add a rendering function in `html_render.c`:
   - Use the `strbuf_t` builder for efficient assembly
   - Pass all user-derived strings through `html_escape()`
   - Return a `malloc()`'d string (caller frees)
   - Embed HTMX attributes in the HTML for interactivity

4. **If the endpoint mutates state**, add the `HX-Trigger: vmStateChanged` header via `send_html_trigger()`.

5. **Rebuild.** No Makefile changes needed.

---

## 10. Guest Agent Integration

The mount/unmount feature communicates with the QEMU Guest Agent running inside the VM via `virDomainQemuAgentCommand()` from `libvirt-qemu.h`.

### How It Works

1. **Mount command flow:**
   - The handler builds a JSON command for `guest-exec`:
     ```json
     {"execute": "guest-exec", "arguments": {"path": "/usr/bin/mkdir", "arg": ["-p", "/media/<tag>"], "capture-output": true}}
     ```
   - Then issues the mount:
     ```json
     {"execute": "guest-exec", "arguments": {"path": "/usr/bin/mount", "arg": ["-t", "virtiofs", "<tag>", "/media/<tag>"], "capture-output": true}}
     ```
   - Output is retrieved via `guest-exec-status` with the PID returned by `guest-exec`.
   - Command output is base64-encoded by the agent and decoded on the host.

2. **Unmount command flow:**
   - Issues `guest-exec` with `/usr/bin/umount /media/<tag>`.

3. **Error handling:**
   - If the guest agent is not running or not installed, `virDomainQemuAgentCommand()` returns an error.
   - Mount failures return rich HTML errors via `render_error_html()` that include:
     - The actual error message from the guest
     - SELinux fix instructions (`semanage permissive -a virt_qemu_ga_t`)
     - Distro-specific commands to install `qemu-guest-agent` and `policycoreutils-python-utils`

3. **Mount status detection:**
   - Before returning shared folder list, `check_mount_status()` is called
   - Executes `findmnt --json` inside the VM via `guest-exec`
   - Parses JSON output to match mounted filesystems against shared folder tags
   - Sets `folders[i].mounted` to 1 (mounted), 0 (not mounted), or -1 (agent unavailable)
   - Status is reflected in UI as "✓ Mounted" / "Not Mounted" badges

4. **Virtiofs shared memory auto-configuration:**
   - When adding a virtiofs shared folder, the backend checks if the VM has `<memoryBacking>` with `<access mode='shared'/>`
   - If missing, it automatically injects the required XML block:
     ```xml
     <memoryBacking>
       <source type='memfd'/>
       <access mode='shared'/>
     </memoryBacking>
     ```
   - This is required for virtiofs to work. Without shared memory, virtiofs mounts will fail.
   - The check is skipped for 9p shared folders (they don't require shared memory).

### SELinux Considerations

On Fedora/RHEL guests, SELinux confines the guest agent under the `virt_qemu_ga_t` domain, which by default does not allow arbitrary command execution (including `mount`). The recommended fix is:

```bash
sudo semanage permissive -a virt_qemu_ga_t
```

---

## 11. Code Style & Conventions

| Convention | Example |
|------------|---------|
| C standard | C11 (`-std=c11`) |
| Warning flags | `-Wall -Wextra`, code must compile clean |
| Naming | `snake_case` for functions and variables |
| Type names | `_t` suffix: `vm_backend_t`, `snapshot_node_t`, `strbuf_t` |
| String ownership | Functions returning allocated strings → caller frees |
| XSS prevention | All user input through `html_escape()` before rendering |
| Logging | `log_msg(LOG_INFO, "fmt", ...)` — levels: `LOG_DEBUG`, `LOG_INFO`, `LOG_WARN`, `LOG_ERROR` |
| Includes | System headers first, then project headers |
| Error returns | 0 = success, -1 = failure (matches libvirt convention) |

### Key Utility Functions (`util.h`)

| Function | Purpose |
|----------|---------|
| `log_msg(level, fmt, ...)` | Formatted logging to stderr |
| `str_dup(s)` | Allocate and copy string (NULL-safe) |
| `str_fmt(fmt, ...)` | Allocate and format (like `asprintf`) |
| `str_starts_with(str, prefix)` | Boolean prefix check |
| `str_eq(a, b)` | String equality (NULL-safe) |
| `url_decode(src)` | URL-decode a string (allocates) |
| `path_segment(url, index)` | Extract Nth URL path segment |

---

## 12. Debugging

### Debug Build

```bash
make debug
```

Produces a binary with debug symbols (`-g`), no optimization (`-O0`), and the `DEBUG` preprocessor macro defined. The server logs to stderr.

### Testing Endpoints with curl

```bash
# Health check
curl http://localhost:9091/api/ping

# List VMs (HTML fragment)
curl http://localhost:9091/api/vms

# Start a VM
curl -X POST http://localhost:9091/api/vms/myvm/start

# Get snapshot tree (JSON)
curl http://localhost:9091/api/vms/myvm/snapshots

# Get snapshot detail (HTML fragment)
curl http://localhost:9091/api/vms/myvm/snapshots/snap1

# Create a snapshot
curl -X POST -d 'name=snap1&description=test&type=internal' \
    http://localhost:9091/api/vms/myvm/snapshots

# Delete a snapshot
curl -X DELETE http://localhost:9091/api/vms/myvm/snapshots/snap1

# Revert to a snapshot
curl -X POST http://localhost:9091/api/vms/myvm/snapshots/snap1/revert

# Merge an external snapshot
curl -X POST http://localhost:9091/api/vms/myvm/snapshots/snap1/merge

# List shared folders
curl http://localhost:9091/api/vms/myvm/shared-folders

# Get add shared folder form
curl http://localhost:9091/api/vms/myvm/shared-folders/form

# Browse directories on the host
curl "http://localhost:9091/api/browse?path=/home"

# Add a shared folder (virtiofs)
curl -X POST -d 'source_dir=/home/user/shared&mount_tag=myshare&fs_type=virtiofs&read_only=0' \
    http://localhost:9091/api/vms/myvm/shared-folders

# Add a shared folder (9p)
curl -X POST -d 'source_dir=/home/user/data&mount_tag=data&fs_type=9p&read_only=1' \
    http://localhost:9091/api/vms/myvm/shared-folders

# Remove (detach) a shared folder
curl -X DELETE http://localhost:9091/api/vms/myvm/shared-folders/myshare

# Mount a shared folder inside the guest
curl -X POST http://localhost:9091/api/vms/myvm/shared-folders/myshare/mount

# Unmount a shared folder inside the guest
curl -X POST http://localhost:9091/api/vms/myvm/shared-folders/myshare/unmount
```

### Verbose Libvirt Logging

```bash
LIBVIRT_DEBUG=1 ./build/qswm --port 9091 --static-dir ./static
```

The `LIBVIRT_DEBUG` environment variable enables verbose output from the libvirt client library, useful for diagnosing connection or API issues.

### Common Issues

| Symptom | Cause | Fix |
|---------|-------|-----|
| `Failed to connect to libvirt` | libvirtd not running or wrong URI | `sudo systemctl start libvirtd` or use `--uri qemu:///session` |
| `pkg-config: package not found` | Missing `-devel` package | Install the prerequisite packages (see §3) |
| Port already in use | Another instance running | Kill the other process or use `--port <other>` |
| 403 on static files | Wrong `--static-dir` path | Pass the correct path to the `static/` directory |

---

## 13. Known Technical Debt / TODOs

- **No authentication or authorization.** The server binds to all interfaces. Use a reverse proxy or firewall to restrict access.
- **No HTTPS.** TLS termination should be handled by a reverse proxy (e.g., nginx, Caddy).
- **Single-disk assumption for merge.** External snapshot merge (`block-commit`) assumes the VM has a single disk. Multi-disk VMs may not merge correctly.
- **No WebSocket support.** The UI polls for updates. A WebSocket connection would enable live state updates without polling.
- **UTM snapshot support blocked.** `utmctl` does not expose snapshot operations, so UTM backend snapshot functions are stubs.
- **No unit tests for `routes.c` or `libvirt_backend.c`.** Testing these properly would require mocking libmicrohttpd connections and the libvirt API.
- **No rate limiting or request size limits.** Acceptable for local use; add limits if exposing to a network.
