#!/usr/bin/env bash
# One-command setup: install deps, build, and run QEMU Snapshot Web Manager
#
# Usage:
#   ./scripts/setup.sh          # install deps, build, and start on port 9091
#   ./scripts/setup.sh 8080     # use a custom port
#   ./scripts/setup.sh --no-run # install deps and build only, don't start
#
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${1:-9091}"
NO_RUN=false
if [ "${1:-}" = "--no-run" ]; then
    NO_RUN=true
fi

# ─── Colors ───
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
BOLD='\033[1m'
NC='\033[0m'

info()  { echo -e "${BLUE}▶${NC} $*"; }
ok()    { echo -e "${GREEN}✔${NC} $*"; }
warn()  { echo -e "${YELLOW}⚠${NC} $*"; }
fail()  { echo -e "${RED}✖${NC} $*"; exit 1; }

echo ""
echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║   QEMU Snapshot Web Manager — Setup      ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
echo ""

# ─── Step 1: Check / Install dependencies ───
info "Step 1/3 — Checking dependencies..."

MISSING=()
for pkg in libmicrohttpd libvirt jansson; do
    if pkg-config --exists "$pkg" 2>/dev/null; then
        ver=$(pkg-config --modversion "$pkg")
        ok "$pkg $ver"
    else
        MISSING+=("$pkg")
        warn "$pkg — not found"
    fi
done

if [ ${#MISSING[@]} -gt 0 ]; then
    echo ""
    info "Installing missing dependencies..."

    if [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
        PKGS=()
        for pkg in "${MISSING[@]}"; do
            PKGS+=("${pkg}-devel")
        done
        # Also ensure compiler and tools are present
        sudo dnf install -y gcc make pkg-config "${PKGS[@]}"

    elif [ -f /etc/debian_version ]; then
        PKGS=()
        for pkg in "${MISSING[@]}"; do
            case "$pkg" in
                jansson) PKGS+=("libjansson-dev") ;;
                *)       PKGS+=("lib${pkg}-dev") ;;
            esac
        done
        sudo apt-get update -qq
        sudo apt-get install -y gcc make pkg-config "${PKGS[@]}"

    elif command -v brew &>/dev/null; then
        brew install "${MISSING[@]}" pkg-config

    else
        fail "Cannot auto-install on this OS. Please install: ${MISSING[*]}"
    fi

    # Verify again
    for pkg in "${MISSING[@]}"; do
        if pkg-config --exists "$pkg" 2>/dev/null; then
            ok "$pkg $(pkg-config --modversion "$pkg") — installed"
        else
            fail "Failed to install $pkg"
        fi
    done
fi

# Also check for gcc and make
for tool in gcc make; do
    if command -v "$tool" &>/dev/null; then
        ok "$tool — $(command -v "$tool")"
    else
        fail "$tool not found. Install it with your package manager."
    fi
done

echo ""

# ─── Step 2: Build ───
info "Step 2/3 — Building..."
make clean >/dev/null 2>&1 || true
if make 2>&1; then
    SIZE=$(du -h build/qswm | cut -f1)
    ok "Build successful — build/qswm ($SIZE)"
else
    fail "Build failed. Check the error messages above."
fi

echo ""

# ─── Step 3: Run ───
if [ "$NO_RUN" = true ]; then
    ok "Setup complete (--no-run: skipping server start)"
    echo ""
    echo -e "  To start manually:  ${BOLD}sudo ./build/qswm --port $PORT --static-dir ./static${NC}"
    echo -e "  Then open:          ${BOLD}http://localhost:$PORT${NC}"
    exit 0
fi

info "Step 3/3 — Starting server on port $PORT..."
echo ""

# Check if port is already in use
if ss -tlnp 2>/dev/null | grep -q ":${PORT} "; then
    warn "Port $PORT is already in use!"
    echo "  Either stop the existing process or use a different port:"
    echo "  ./scripts/setup.sh 8080"
    exit 1
fi

# Check if libvirtd is running
if ! systemctl is-active --quiet libvirtd 2>/dev/null; then
    warn "libvirtd is not running. Starting it..."
    sudo systemctl start libvirtd
    ok "libvirtd started"
fi

echo -e "${BOLD}╔══════════════════════════════════════════╗${NC}"
echo -e "${BOLD}║  Starting QSWM...                        ║${NC}"
echo -e "${BOLD}║                                          ║${NC}"
echo -e "${BOLD}║  Open: http://localhost:$PORT             ║${NC}"
echo -e "${BOLD}║  Stop: Ctrl+C                            ║${NC}"
echo -e "${BOLD}╚══════════════════════════════════════════╝${NC}"
echo ""

sudo ./build/qswm --port "$PORT" --static-dir ./static
