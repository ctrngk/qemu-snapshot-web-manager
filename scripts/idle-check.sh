#!/bin/bash
# Check if qswm is idle and stop it if so.
# Called by qswm-idle.service via qswm-idle.timer.

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
