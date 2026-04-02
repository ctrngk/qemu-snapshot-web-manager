#!/bin/bash
set -euo pipefail

VERSION="${1:?Usage: build-apk.sh VERSION ARCH}"
ARCH="${2:?Usage: build-apk.sh VERSION ARCH}"

# Update version in APKBUILD
sed -i "s/VERSION_PLACEHOLDER/$VERSION/" packaging/alpine/APKBUILD

echo "APKBUILD prepared for version $VERSION ($ARCH)"
