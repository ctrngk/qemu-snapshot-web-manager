#!/usr/bin/env bash
# Run all tests
set -euo pipefail
cd "$(dirname "$0")/.."

echo "=== QSWM Test Suite ==="
echo ""

make test

echo ""
echo "=== All tests passed ✅ ==="
