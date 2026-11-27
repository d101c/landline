Name:           landline
Version:        0.3.0
Release:        2%{?dist}
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

%post
# The Bluetooth settings iPhones need, when this system does not have them.
# Removing the package puts them back (%%preun).
%{_libexecdir}/landline/landline-setup apply >/dev/null 2>&1 || :
# Installed from a downloaded file: add the repository the package comes from.
if ! grep -rqs 'd101c:landline' /etc/yum.repos.d 2>/dev/null; then
    cat > /etc/yum.repos.d/landline.repo <<'REPO'
# Written by the landline package. Removed with it.
[copr:copr.fedorainfracloud.org:d101c:landline]
name=Landline (Copr d101c/landline)
baseurl=https://download.copr.fedorainfracloud.org/results/d101c/landline/fedora-$releasever-$basearch/
type=rpm-md
skip_if_unavailable=True
gpgcheck=1
gpgkey=https://download.copr.fedorainfracloud.org/results/d101c/landline/pubkey.gpg
repo_gpgcheck=0
enabled=1
REPO
fi

%postun
if [ "$1" -eq 0 ] && grep -qs 'Written by the landline package' /etc/yum.repos.d/landline.repo; then
    rm -f /etc/yum.repos.d/landline.repo
fi

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
%{_libexecdir}/landline/landline-first-run
%{_sysconfdir}/xdg/autostart/landline-first-run.desktop
%exclude %{_datadir}/keyrings/landline-ppa.asc
%{_libexecdir}/landline/VERSION
%caps(cap_net_raw=ep) %{_libexecdir}/landline/landline-le-params
%{_userunitdir}/landline-engine.service
%{_userunitdir}/obex.service.d/
%{_datadir}/polkit-1/actions/io.github.d101c.landline.policy
%{_datadir}/gnome-shell/extensions/landline@d101c.github.io/

%changelog
* Fri Nov 27 2026 Landline <landline@d101c.github.io> - 0.3.0-2
- The extension is turned on at the first login after the install.
- The Bluetooth settings iPhones need are set at install time when missing.
- Installed from a downloaded file, the package adds its own repository.

* Fri Nov 27 2026 Landline <landline@d101c.github.io> - 0.3.0-1
- Engine 0.3.0: a classic link BlueZ keeps "In Progress" is freed, the charging state is told.

* Tue Nov 24 2026 Landline <landline@d101c.github.io> - 0.2.1-1
- First package: the extension and its engine together.
