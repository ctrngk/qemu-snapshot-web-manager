#!/usr/bin/env bash
# Uninstall QEMU Snapshot Web Manager
set -euo pipefail

if [ "$(id -u)" -ne 0 ]; then
    echo "This script must be run as root (use sudo)"
    exit 1
fi

PREFIX="${1:-/usr/local}"
echo "=== Uninstalling QSWM ==="

# Stop service if running
if systemctl is-active --quiet qemu-snapshot-web-manager 2>/dev/null; then
    echo "Stopping service..."
    systemctl stop qemu-snapshot-web-manager
fi

if systemctl is-enabled --quiet qemu-snapshot-web-manager 2>/dev/null; then
    echo "Disabling service..."
    systemctl disable qemu-snapshot-web-manager
fi

# Remove files
echo "Removing files..."
rm -f "$PREFIX/bin/qswm"
rm -rf "$PREFIX/share/qswm"
rm -f /etc/systemd/system/qemu-snapshot-web-manager.service

if [ -d /etc/systemd/system ]; then
    systemctl daemon-reload
fi

echo ""
echo "=== Uninstall complete ==="
