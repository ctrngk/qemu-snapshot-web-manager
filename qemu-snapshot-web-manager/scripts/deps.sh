#!/usr/bin/env bash
# Install build dependencies
set -euo pipefail

echo "=== Installing QSWM Dependencies ==="

# Detect distro
if [ -f /etc/fedora-release ] || [ -f /etc/redhat-release ]; then
    echo "Detected: Fedora/RHEL"
    sudo dnf install -y gcc make pkg-config \
        libmicrohttpd-devel libvirt-devel jansson-devel \
        inotify-tools
elif [ -f /etc/debian_version ]; then
    echo "Detected: Debian/Ubuntu"
    sudo apt update
    sudo apt install -y gcc make pkg-config \
        libmicrohttpd-dev libvirt-dev libjansson-dev \
        inotify-tools
elif command -v brew &>/dev/null; then
    echo "Detected: macOS (Homebrew)"
    brew install libmicrohttpd libvirt jansson pkg-config
else
    echo "Unknown distro. Please install manually:"
    echo "  - gcc, make, pkg-config"
    echo "  - libmicrohttpd (development headers)"
    echo "  - libvirt (development headers)"
    echo "  - jansson (development headers)"
    exit 1
fi

echo ""
echo "=== Verifying ==="
for pkg in libmicrohttpd libvirt jansson; do
    version=$(pkg-config --modversion "$pkg" 2>/dev/null || echo "NOT FOUND")
    echo "  $pkg: $version"
done

echo ""
echo "=== Dependencies installed ✅ ==="
echo "Now run: ./scripts/build.sh"
