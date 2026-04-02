#!/usr/bin/env bash
# Install QEMU Snapshot Web Manager system-wide
set -euo pipefail
cd "$(dirname "$0")/.."

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root (use sudo)"
    exit 1
fi

PREFIX="${1:-/usr/local}"
echo "=== Installing QSWM to $PREFIX ==="

# Build first
echo "Building release..."
make clean
make all

# Install binary
echo "Installing binary..."
install -Dm755 build/qswm "$PREFIX/bin/qswm"

# Install static files
echo "Installing static assets..."
install -d "$PREFIX/share/qswm/static"
cp -r static/* "$PREFIX/share/qswm/static/"

# Install systemd service
if [ -d /etc/systemd/system ]; then
    echo "Installing systemd service..."
    install -Dm644 systemd/qemu-snapshot-web-manager.service \
        /etc/systemd/system/qemu-snapshot-web-manager.service
    
    # Update ExecStart path if custom prefix
    if [ "$PREFIX" != "/usr/local" ]; then
        sed -i "s|/usr/local/bin/qswm|$PREFIX/bin/qswm|g" \
            /etc/systemd/system/qemu-snapshot-web-manager.service
        sed -i "s|/usr/local/share/qswm|$PREFIX/share/qswm|g" \
            /etc/systemd/system/qemu-snapshot-web-manager.service
    fi
    
    systemctl daemon-reload
    echo ""
    echo "To start the service:"
    echo "  sudo systemctl start qemu-snapshot-web-manager"
    echo "  sudo systemctl enable qemu-snapshot-web-manager  # auto-start on boot"
fi

echo ""
echo "=== Installation complete ==="
echo "Binary:  $PREFIX/bin/qswm"
echo "Static:  $PREFIX/share/qswm/static/"
echo "Service: /etc/systemd/system/qemu-snapshot-web-manager.service"
echo ""
echo "Run manually:  sudo qswm --port 9091"
echo "Open browser:  http://localhost:9091"
