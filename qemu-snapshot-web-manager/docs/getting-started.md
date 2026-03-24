# Getting Started

Get QEMU Snapshot Web Manager running in **one command**.

## The Quick Way

```bash
./scripts/setup.sh
```

That's it. The script will:
1. **Detect your OS** (Fedora, Ubuntu, Debian, macOS)
2. **Install missing dependencies** automatically
3. **Build the project**
4. **Start the server**

Then open your browser to **http://localhost:9091**.

> Need a different port? `./scripts/setup.sh 8080`
>
> Just want to build, not run? `./scripts/setup.sh --no-run`

---

## What You Need

- A Linux machine (Fedora, Ubuntu, Debian) or macOS with Homebrew
- QEMU/KVM virtual machines managed by libvirt
- A web browser
- `sudo` access (for installing packages and accessing VMs)

The setup script handles everything else.

---

## Step-by-Step (If You Prefer Manual)

### 1. Install dependencies

**Fedora / RHEL:**
```bash
sudo dnf install gcc make pkg-config libmicrohttpd-devel libvirt-devel jansson-devel
```

**Ubuntu / Debian:**
```bash
sudo apt install gcc make pkg-config libmicrohttpd-dev libvirt-dev libjansson-dev
```

**macOS:**
```bash
brew install libmicrohttpd libvirt jansson pkg-config
```

Or just run:
```bash
./scripts/deps.sh
```

### 2. Build

```bash
make
```

### 3. Start

```bash
sudo ./build/qswm --port 9091 --static-dir ./static
```

### 4. Open your browser

Go to **http://localhost:9091**

---

## What You'll See

```
┌──────────────┬──────────────────────────┬──────────────────┐
│  Your VMs    │   Snapshot Tree (D3.js)  │  Details Panel   │
│              │                          │                  │
│  vm1 ● on   │       [root]             │  Name: snap1     │
│  vm2 ○ off  │      /     \             │  Type: internal  │
│  vm3 ● on   │  [snap1]  [snap2]        │  Date: ...       │
│              │     |                    │                  │
│              │  [snap3]                 │  [Revert][Delete]│
│              │                          │                  │
│              │                          │  Shared Folders  │
│              │                          │  📁 myshare     │
└──────────────┴──────────────────────────┴──────────────────┘
```

- **Left panel** — Your VMs with status (click to select)
- **Center** — Interactive snapshot tree (click nodes, zoom, pan)
- **Right** — Snapshot details + actions + shared folders

---

## Run as a System Service

Want it to start automatically on boot?

```bash
sudo ./scripts/install.sh
sudo systemctl enable --now qemu-snapshot-web-manager
```

To remove it later:
```bash
sudo ./scripts/uninstall.sh
```

---

## Available Scripts

All scripts are in the `scripts/` directory:

| Script | What it does |
|--------|-------------|
| `./scripts/setup.sh` | **One command to rule them all** — deps + build + run |
| `./scripts/deps.sh` | Install build dependencies only |
| `./scripts/build.sh` | Build only (checks deps first) |
| `./scripts/test.sh` | Run the test suite |
| `./scripts/dev.sh` | Dev server with auto-rebuild on file changes |
| `./scripts/install.sh` | Install system-wide + systemd service |
| `./scripts/uninstall.sh` | Remove everything cleanly |

---

## Troubleshooting

**"Permission denied"**
→ Run with `sudo`: `sudo ./build/qswm`

**"No virtual machines found"**
→ Make sure libvirtd is running: `sudo systemctl start libvirtd`

**"Port already in use"**
→ Use a different port: `./scripts/setup.sh 8080`

**"pkg-config not found" or "library not found"**
→ Run `./scripts/deps.sh` to install everything

**Build errors**
→ Run `./scripts/deps.sh` first, then `make clean && make`

---

## Next Steps

- 📖 **[User Guide](user-guide.md)** — Learn how to use all features
- 🔧 **[Developer Guide](developer-guide.md)** — Architecture, API reference, how to contribute
