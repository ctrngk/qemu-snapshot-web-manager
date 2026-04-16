Name:           qswm
Version:        %{_version}
Release:        1%{?dist}
Summary:        QEMU Snapshot Web Manager — lightweight web UI for KVM snapshot management

License:        MIT
URL:            https://github.com/ctrngk/qemu-snapshot-web-manager
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc make pkg-config
BuildRequires:  libmicrohttpd-devel libvirt-devel jansson-devel systemd-devel

Requires:       libmicrohttpd libvirt-libs jansson systemd-libs

%description
A lightweight web UI for managing QEMU/KVM virtual machine snapshots with
tree visualization. Features socket activation, idle auto-shutdown, and
drop-in configuration.

%prep
%setup -q -n qemu-snapshot-web-manager-%{version}

%build
make %{?_smp_mflags}

%install
install -Dm755 build/qswm %{buildroot}/usr/local/bin/qswm
install -d %{buildroot}/usr/local/share/qswm/static
cp -r static/* %{buildroot}/usr/local/share/qswm/static/
install -Dm755 scripts/idle-check.sh %{buildroot}/usr/local/libexec/qswm/idle-check.sh
install -Dm644 systemd/qswm.socket %{buildroot}/etc/systemd/system/qswm.socket
install -Dm644 systemd/qswm.service %{buildroot}/etc/systemd/system/qswm.service
install -Dm644 systemd/qswm-idle.timer %{buildroot}/etc/systemd/system/qswm-idle.timer
install -Dm644 systemd/qswm-idle.service %{buildroot}/etc/systemd/system/qswm-idle.service
install -d %{buildroot}/etc/qswm/conf.d

%post
systemctl daemon-reload

%preun
if [ $1 -eq 0 ]; then
    systemctl disable --now qswm.socket qswm.service qswm-idle.timer 2>/dev/null || true
fi

%postun
systemctl daemon-reload

%files
/usr/local/bin/qswm
/usr/local/share/qswm/
/usr/local/libexec/qswm/
/etc/systemd/system/qswm.socket
/etc/systemd/system/qswm.service
/etc/systemd/system/qswm-idle.timer
/etc/systemd/system/qswm-idle.service
%dir /etc/qswm
%dir /etc/qswm/conf.d

%changelog
