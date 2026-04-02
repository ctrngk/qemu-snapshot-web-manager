#!/bin/bash
# Universal installer for qswm (QEMU Snapshot Web Manager)
# Usage: curl -fsSL https://raw.githubusercontent.com/ctrngk/qemu-snapshot-web-manager/main/install.sh | sudo bash
#        curl -fsSL ... | sudo bash -s -- v1.0.0
set -euo pipefail

REPO="ctrngk/qemu-snapshot-web-manager"
VERSION="${1:-}"
ARCH="$(uname -m)"

# Detect latest version if not specified
if [ -z "$VERSION" ]; then
    VERSION=$(curl -fsSL "https://api.github.com/repos/${REPO}/releases/latest" | grep '"tag_name"' | sed 's/.*"v//' | sed 's/".*//')
    if [ -z "$VERSION" ]; then
        echo "Error: Could not detect latest version." >&2
        exit 1
    fi
fi
# Strip leading 'v' if present
VERSION="${VERSION#v}"

echo "Installing qswm v${VERSION} for ${ARCH}..."

BASE_URL="https://github.com/${REPO}/releases/download/v${VERSION}"

# Detect OS
if [ -f /etc/os-release ]; then
    . /etc/os-release
else
    echo "Error: Cannot detect OS (no /etc/os-release)." >&2
    exit 1
fi

install_rpm() {
    local rpm_arch="$ARCH"
    local url="${BASE_URL}/qswm-${VERSION}.${rpm_arch}.rpm"
    echo "Downloading RPM: $url"
    curl -fsSL -o "/tmp/qswm.rpm" "$url"
    dnf install -y "/tmp/qswm.rpm" || rpm -Uvh "/tmp/qswm.rpm"
    rm -f "/tmp/qswm.rpm"
}

install_deb() {
    local deb_arch="$ARCH"
    [ "$deb_arch" = "x86_64" ] && deb_arch="amd64"
    [ "$deb_arch" = "aarch64" ] && deb_arch="arm64"
    local url="${BASE_URL}/qswm_${VERSION}_${deb_arch}.deb"
    echo "Downloading DEB: $url"
    curl -fsSL -o "/tmp/qswm.deb" "$url"
    dpkg -i "/tmp/qswm.deb" || apt-get install -f -y
    rm -f "/tmp/qswm.deb"
}

install_apk() {
    local url="${BASE_URL}/qswm-${VERSION}.${ARCH}.apk"
    echo "Downloading APK: $url"
    curl -fsSL -o "/tmp/qswm.apk" "$url"
    apk add --allow-untrusted "/tmp/qswm.apk"
    rm -f "/tmp/qswm.apk"
}

install_tarball() {
    local url="${BASE_URL}/qswm-${VERSION}-linux-${ARCH}.tar.gz"
    echo "Downloading tarball: $url"
    local tmp="$(mktemp -d)"
    curl -fsSL "$url" | tar xz -C "$tmp"
    "$tmp/qswm-${VERSION}-linux-${ARCH}/install.sh"
    rm -rf "$tmp"
}

case "${ID:-unknown}" in
    fedora|rhel|centos|rocky|alma|ol)
        install_rpm ;;
    opensuse*|sles)
        install_rpm ;;
    ubuntu|debian|pop|linuxmint|elementary)
        install_deb ;;
    alpine)
        install_apk ;;
    arch|manjaro|endeavouros)
        install_tarball ;;
    *)
        echo "Unknown distro '${ID}', falling back to tarball install."
        install_tarball ;;
esac

echo ""
echo "qswm v${VERSION} installed successfully!"
echo "Enable socket activation with:"
echo "  sudo systemctl enable --now qswm.socket qswm-idle.timer"
