Name:           landline
Version:        0.3.0
Release:        1%{?dist}
Summary:        iPhone calls, messages and notifications on the GNOME desktop
License:        GPL-2.0-or-later AND MIT
URL:            https://github.com/d101c/landline
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  cmake ninja-build gcc-c++ pkgconf-pkg-config gettext
BuildRequires:  glib2-devel libsecret-devel libnotify-devel openssl-devel avahi-devel
BuildRequires:  json-devel libstdc++-static systemd-rpm-macros
Requires:       gnome-shell >= 51
Requires:       bluez bluez-obexd polkit procps-ng

%description
A quiet dot in the top bar when the iPhone is linked over Bluetooth: read and
answer messages, answer or decline calls, see the notifications of the other
apps. Nothing is installed on the iPhone.

This package holds the GNOME Shell extension and its engine.

%prep
%autosetup

%build
%cmake -S engine -G Ninja -DTETHER_VERSION=%{version} -DTETHER_BUILD_EXTENSIONS=OFF \
    -DTETHER_CAMERA=OFF -DBUILD_TESTING=OFF -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DLANDLINE_LIBEXECDIR=%{_libexecdir}/landline
%cmake_build

%install
packaging/install-tree.sh %{buildroot} %{_libexecdir} %{__cmake_builddir} %{version}
%find_lang %{name}

%preun
if [ "$1" -eq 0 ]; then
    # Put the Bluetooth settings back the way they were.
    %{_libexecdir}/landline/landline-setup revert >/dev/null 2>&1 || :
fi

%files -f %{name}.lang
%license %{_datadir}/licenses/landline/
%dir %{_libexecdir}/landline
%{_libexecdir}/landline/landlined
%{_libexecdir}/landline/landlinectl
%{_libexecdir}/landline/landline-setup
%{_libexecdir}/landline/VERSION
%caps(cap_net_raw=ep) %{_libexecdir}/landline/landline-le-params
%{_userunitdir}/landline-engine.service
%{_userunitdir}/obex.service.d/
%{_datadir}/polkit-1/actions/io.github.d101c.landline.policy
%{_datadir}/gnome-shell/extensions/landline@d101c.github.io/

%changelog
* Fri Nov 27 2026 Landline <landline@d101c.github.io> - 0.3.0-1
- Engine 0.3.0: a classic link BlueZ keeps "In Progress" is freed, the charging state is told.

* Tue Nov 24 2026 Landline <landline@d101c.github.io> - 0.2.1-1
- First package: the extension and its engine together.
