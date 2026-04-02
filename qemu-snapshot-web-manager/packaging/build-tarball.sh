#!/bin/bash
set -euo pipefail

VERSION="${1:?Usage: build-tarball.sh VERSION ARCH}"
ARCH="${2:?Usage: build-tarball.sh VERSION ARCH}"

PKG="qswm-${VERSION}-linux-${ARCH}"
STAGE="$(mktemp -d)/${PKG}"
mkdir -p "$STAGE"

install -Dm755 build/qswm "$STAGE/bin/qswm"
install -d "$STAGE/share/qswm/static"
cp -r static/* "$STAGE/share/qswm/static/"
install -Dm755 scripts/idle-check.sh "$STAGE/libexec/idle-check.sh"
install -Dm644 systemd/qswm.socket "$STAGE/systemd/qswm.socket"
install -Dm644 systemd/qswm.service "$STAGE/systemd/qswm.service"
install -Dm644 systemd/qswm-idle.timer "$STAGE/systemd/qswm-idle.timer"
install -Dm644 systemd/qswm-idle-check.service "$STAGE/systemd/qswm-idle-check.service"

cat > "$STAGE/install.sh" <<'INSTALLER'
#!/bin/bash
set -euo pipefail
DIR="$(cd "$(dirname "$0")" && pwd)"
echo "Installing qswm from tarball..."
sudo install -Dm755 "$DIR/bin/qswm" /usr/local/bin/qswm
sudo install -d /usr/local/share/qswm/static
sudo cp -r "$DIR/share/qswm/static/"* /usr/local/share/qswm/static/
sudo install -Dm755 "$DIR/libexec/idle-check.sh" /usr/local/libexec/qswm/idle-check.sh
sudo install -Dm644 "$DIR/systemd/qswm.socket" /etc/systemd/system/qswm.socket
sudo install -Dm644 "$DIR/systemd/qswm.service" /etc/systemd/system/qswm.service
sudo install -Dm644 "$DIR/systemd/qswm-idle.timer" /etc/systemd/system/qswm-idle.timer
sudo install -Dm644 "$DIR/systemd/qswm-idle-check.service" /etc/systemd/system/qswm-idle-check.service
sudo install -d /etc/qswm/conf.d
sudo systemctl daemon-reload
echo "Installed. Enable with: sudo systemctl enable --now qswm.socket qswm-idle.timer"
INSTALLER
chmod +x "$STAGE/install.sh"

tar czf "${PKG}.tar.gz" -C "$(dirname "$STAGE")" "$PKG"
rm -rf "$(dirname "$STAGE")"
echo "Built: ${PKG}.tar.gz"
