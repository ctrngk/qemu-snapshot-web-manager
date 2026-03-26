#!/usr/bin/env bash
# Stop the QEMU Snapshot Web Manager if running.
# Usage: sudo ./scripts/dev-stop.sh

set -euo pipefail

if pgrep -f 'qswm.*--port' > /dev/null 2>&1; then
    echo "Stopping qswm..."
    pkill -f 'qswm.*--port'
    sleep 1
    echo "Stopped."
else
    echo "qswm is not running."
fi
