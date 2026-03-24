#!/usr/bin/env bash
# Development server with auto-rebuild
set -euo pipefail
cd "$(dirname "$0")/.."

PORT="${1:-9091}"
STATIC_DIR="./static"
URI="${2:-qemu:///system}"
PID=""

cleanup() {
    echo ""
    echo "Shutting down..."
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill "$PID"
        wait "$PID" 2>/dev/null
    fi
    exit 0
}
trap cleanup SIGINT SIGTERM

build_and_run() {
    # Stop previous instance
    if [ -n "$PID" ] && kill -0 "$PID" 2>/dev/null; then
        kill "$PID"
        wait "$PID" 2>/dev/null
    fi

    echo ""
    echo "=== Rebuilding... ==="
    if make debug 2>&1; then
        echo "=== Starting server on port $PORT ==="
        sudo ./build/qswm --port "$PORT" --static-dir "$STATIC_DIR" --uri "$URI" &
        PID=$!
        echo "Server PID: $PID"
        echo "Open: http://localhost:$PORT"
    else
        echo "BUILD FAILED — fix errors and save again"
        PID=""
    fi
}

echo "=== QSWM Development Server ==="
echo "Watching for changes in src/ and static/..."
echo "Press Ctrl+C to stop"
echo ""

# Initial build
build_and_run

# Watch for changes
if command -v inotifywait &>/dev/null; then
    while true; do
        inotifywait -q -r -e modify,create,delete src/ static/ 2>/dev/null
        echo "Change detected!"
        build_and_run
    done
else
    echo "(Install inotify-tools for instant rebuild: sudo dnf install inotify-tools)"
    echo "Falling back to polling (every 2 seconds)..."
    LAST_HASH=""
    while true; do
        sleep 2
        HASH=$(find src/ static/ -type f -newer build/qswm 2>/dev/null | head -1)
        if [ -n "$HASH" ] && [ "$HASH" != "$LAST_HASH" ]; then
            LAST_HASH="$HASH"
            echo "Change detected!"
            build_and_run
        fi
    done
fi
