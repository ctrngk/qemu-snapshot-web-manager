#!/usr/bin/env bash
# Build QEMU Snapshot Web Manager
set -euo pipefail
cd "$(dirname "$0")/.."

echo "=== Building qswm ==="

# Check dependencies
for pkg in libmicrohttpd libvirt jansson; do
    if ! pkg-config --exists "$pkg" 2>/dev/null; then
        echo "ERROR: Missing dependency: $pkg"
        echo "Install with:"
        echo "  Fedora: sudo dnf install ${pkg}-devel"
        echo "  Ubuntu: sudo apt install lib${pkg}-dev"
        exit 1
    fi
done

BUILD_TYPE="${1:-release}"

if [ "$BUILD_TYPE" = "debug" ]; then
    echo "Building debug..."
    make debug
else
    echo "Building release..."
    make all
fi

echo ""
echo "=== Build complete ==="
echo "Binary: build/qswm ($(du -h build/qswm | cut -f1))"
echo "Run: sudo ./build/qswm --port 9091 --static-dir ./static"
