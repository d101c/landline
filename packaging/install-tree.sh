#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Lays out a distribution package of Landline (deb, rpm, Arch): the engine,
# its setup helper and the GNOME Shell extension, in one tree.
#
#   packaging/install-tree.sh <destdir> <libexecdir> <build dir> <version>
#
#   <libexecdir>  /usr/libexec (Debian, Ubuntu, Fedora) or /usr/lib (Arch)
#   <build dir>   where cmake built engine/ (landlined, landlinectl,
#                 landline-le-params, locale/)
#
# Nothing here needs root or the network. The one thing left to the package
# scripts: "setcap cap_net_raw+ep <libexecdir>/landline/landline-le-params".
set -euo pipefail

[ "$#" = 4 ] || { sed -n '2,13p' "$0" >&2; exit 64; }
dest="$1"; libexec="$2"; build="$3"; version="$4"
repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
uuid="landline@d101c.github.io"
dir="$libexec/landline"

install -d "$dest$dir"
for bin in landlined landlinectl landline-le-params; do
    install -m 0755 "$build/$bin" "$dest$dir/$bin"
done
printf '%s\n' "$version" > "$dest$dir/VERSION"
chmod 0644 "$dest$dir/VERSION"

# The setup helper, told that it belongs to a package.
sed -e 's|^PACKAGED=0$|PACKAGED=1|' \
    -e "s|^LIBEXEC_PARENT=/usr/local/libexec\$|LIBEXEC_PARENT=$libexec|" \
    "$repo/system/landline-setup" > "$dest$dir/landline-setup"
chmod 0755 "$dest$dir/landline-setup"
grep -q '^PACKAGED=1$' "$dest$dir/landline-setup"
grep -q "^LIBEXEC_PARENT=$libexec\$" "$dest$dir/landline-setup"

install -d "$dest/usr/share/polkit-1/actions"
sed -e "s|/usr/local/libexec/landline/landline-setup|$dir/landline-setup|" \
    -e '/Managed by landline-setup/d' \
    "$repo/system/io.github.d101c.landline.policy" > "$dest/usr/share/polkit-1/actions/io.github.d101c.landline.policy"
grep -q "$dir/landline-setup" "$dest/usr/share/polkit-1/actions/io.github.d101c.landline.policy"

# Translations of the engine.
if [ -d "$build/locale" ]; then
    install -d "$dest/usr/share/locale"
    cp -r "$build/locale/." "$dest/usr/share/locale/"
fi

# User units: the engine (started by the extension, enabled for nobody) and
# the complement that restarts BlueZ's obexd.
install -d "$dest/usr/lib/systemd/user/obex.service.d"
cat > "$dest/usr/lib/systemd/user/landline-engine.service" <<UNIT
[Unit]
Description=Landline engine (Bluetooth link to an iPhone)
Documentation=https://github.com/d101c/landline
PartOf=graphical-session.target dbus.service
After=graphical-session.target dbus.socket dbus.service
StartLimitIntervalSec=60
StartLimitBurst=5

[Service]
Type=simple
Environment=PATH=$dir:/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
ExecStartPre=-/usr/bin/pkill -u %U -f ^landlined
ExecStart=$dir/landlined
Restart=on-failure
RestartSec=3

[Install]
WantedBy=graphical-session.target
UNIT
grep -v '^#' "$repo/system/obex-landline.conf" > "$dest/usr/lib/systemd/user/obex.service.d/landline.conf"

# The extension, for every user of the computer (each one turns it on).
ext="$dest/usr/share/gnome-shell/extensions/$uuid"
install -d "$ext"
cp -r "$repo/extension/." "$ext/"
rm -f "$ext/schemas/gschemas.compiled" "$ext/.gitignore"
glib-compile-schemas --strict "$ext/schemas"
find "$ext" -type d -exec chmod 0755 {} + ; find "$ext" -type f -exec chmod 0644 {} +

install -Dm 0644 "$repo/engine/LICENSE" "$dest/usr/share/licenses/landline/LICENSE.engine"
install -Dm 0644 "$repo/extension/LICENSE" "$dest/usr/share/licenses/landline/LICENSE.extension"
