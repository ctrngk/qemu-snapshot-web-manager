#!/bin/bash
set -euo pipefail

VERSION="${1:?Usage: build-deb.sh VERSION ARCH}"
ARCH="${2:?Usage: build-deb.sh VERSION ARCH}"

DEB_ARCH="$ARCH"
[ "$ARCH" = "x86_64" ] && DEB_ARCH="amd64"
[ "$ARCH" = "aarch64" ] && DEB_ARCH="arm64"

PKG="qswm_${VERSION}_${DEB_ARCH}"
STAGE="$(mktemp -d)"

# Install tree
install -Dm755 build/qswm "$STAGE/usr/local/bin/qswm"
install -d "$STAGE/usr/local/share/qswm/static"
cp -r static/* "$STAGE/usr/local/share/qswm/static/"
install -Dm755 scripts/idle-check.sh "$STAGE/usr/local/libexec/qswm/idle-check.sh"
install -Dm644 systemd/qswm.socket "$STAGE/etc/systemd/system/qswm.socket"
install -Dm644 systemd/qswm.service "$STAGE/etc/systemd/system/qswm.service"
install -Dm644 systemd/qswm-idle.timer "$STAGE/etc/systemd/system/qswm-idle.timer"
install -Dm644 systemd/qswm-idle-check.service "$STAGE/etc/systemd/system/qswm-idle-check.service"
install -d "$STAGE/etc/qswm/conf.d"

# DEBIAN control
install -d "$STAGE/DEBIAN"
sed "s/VERSION_PLACEHOLDER/$VERSION/;s/ARCH_PLACEHOLDER/$DEB_ARCH/" \
    packaging/deb/control > "$STAGE/DEBIAN/control"
install -m755 packaging/deb/postinst "$STAGE/DEBIAN/postinst"
install -m755 packaging/deb/prerm "$STAGE/DEBIAN/prerm"
cp packaging/deb/conffiles "$STAGE/DEBIAN/conffiles"

dpkg-deb --build "$STAGE" "${PKG}.deb"
rm -rf "$STAGE"
echo "Built: ${PKG}.deb"
