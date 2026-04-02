# Cross-Platform Packaging & Release Pipeline

## Problem

qswm is currently install-from-source only. Users on Fedora, Ubuntu, Debian, Alpine, and other Linux distros should be able to install via their native package manager or a universal install script.

## Approach

GitHub Actions CI/CD pipeline triggered on `git tag v*`, building native packages for major Linux distros plus a generic tarball. A universal install script auto-detects the OS and installs the correct package.

## Release Artifacts

Per architecture (x86_64 + aarch64):

| Artifact | Target |
|----------|--------|
| `qswm-{ver}.x86_64.rpm` | Fedora, RHEL, openSUSE |
| `qswm-{ver}.aarch64.rpm` | Fedora, RHEL, openSUSE (ARM) |
| `qswm_{ver}_amd64.deb` | Ubuntu, Debian |
| `qswm_{ver}_arm64.deb` | Ubuntu, Debian (ARM) |
| `qswm-{ver}.x86_64.apk` | Alpine |
| `qswm-{ver}.aarch64.apk` | Alpine (ARM) |
| `qswm-{ver}-linux-x86_64.tar.gz` | Generic Linux / Arch / FreeBSD |
| `qswm-{ver}-linux-aarch64.tar.gz` | Generic Linux (ARM) |
| `install.sh` | Universal installer script |

## GitHub Actions Workflow

### Trigger

```yaml
on:
  push:
    tags: ['v*']
```

### Build Matrix

```yaml
strategy:
  matrix:
    include:
      - os: ubuntu-latest
        arch: x86_64
        platform: linux/amd64
      - os: ubuntu-latest
        arch: aarch64
        platform: linux/arm64
```

### Build Steps

1. **Checkout** source at tag
2. **Set up QEMU** (for aarch64 cross-builds via `docker/setup-qemu-action`)
3. **Build RPM** in Fedora container
4. **Build DEB** in Ubuntu container
5. **Build APK** in Alpine container
6. **Build tarball** — compiled binary + static files + systemd units + install script
7. **Create GitHub Release** — attach all artifacts

### aarch64 Strategy

Use Docker with QEMU emulation:
```yaml
- uses: docker/setup-qemu-action@v3
  with:
    platforms: arm64
```

Then run builds in `--platform linux/arm64` containers.

## Package Spec Details

### RPM (.spec file)

```
Name:     qswm
Version:  %{version}
Release:  1%{?dist}
Summary:  QEMU Snapshot Web Manager
License:  MIT
Requires: libmicrohttpd libvirt-libs jansson systemd-libs
```

Post-install scriptlet: `systemctl daemon-reload`
Config: `/etc/qswm/conf.d/` marked as `%dir` (preserved on upgrade)

### DEB (debian/ directory)

```
Package: qswm
Architecture: amd64
Depends: libmicrohttpd12, libvirt0, libjansson4, libsystemd0
```

Maintainer scripts: postinst runs `systemctl daemon-reload`
conffiles: `/etc/qswm/conf.d/` preserved on upgrade

### APK (APKBUILD)

Alpine package with OpenRC init script alternative (Alpine uses OpenRC, not systemd).

### Tarball

Generic `.tar.gz` containing:
```
qswm-{ver}-linux-{arch}/
├── bin/qswm
├── share/qswm/static/
├── systemd/qswm.socket
├── systemd/qswm.service
├── systemd/qswm-idle.timer
├── systemd/qswm-idle-check.service
├── libexec/idle-check.sh
└── install.sh        # local installer for the tarball
```

## Install Script (`install.sh`)

Universal installer that:
1. Detects OS via `/etc/os-release`
2. Detects architecture via `uname -m`
3. Fetches the latest release from GitHub API (or uses a specified version)
4. Downloads the correct package (.rpm, .deb, .apk, or .tar.gz)
5. Installs using the native package manager
6. Runs `systemctl daemon-reload` and prints enable instructions

Usage:
```bash
# Install latest
curl -fsSL https://raw.githubusercontent.com/ctrngk/qemu-snapshot-web-manager/main/install.sh | sudo bash

# Install specific version
curl -fsSL https://raw.githubusercontent.com/ctrngk/qemu-snapshot-web-manager/main/install.sh | sudo bash -s -- v1.0.0
```

## Package Contents (all formats)

| Path | Content |
|------|---------|
| `/usr/local/bin/qswm` | Binary |
| `/usr/local/share/qswm/static/` | HTML, CSS, JS assets |
| `/etc/systemd/system/qswm.socket` | Socket activation unit |
| `/etc/systemd/system/qswm.service` | Service unit |
| `/etc/systemd/system/qswm-idle.timer` | Idle check timer |
| `/etc/systemd/system/qswm-idle-check.service` | Idle check oneshot |
| `/usr/local/libexec/qswm/idle-check.sh` | Idle check script |
| `/etc/qswm/conf.d/` | Drop-in config directory (empty, preserved) |

## macOS

Skipped for now (experimental). macOS instructions remain in README for building from source.
