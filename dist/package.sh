#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis, N0GQ
# package.sh: build Signum once (prefix /usr, no encryption) and make the release files for this
# computer's architecture in out/:
#   signum_VERSION_ARCH.deb            Debian, Ubuntu, Mint, Raspberry Pi OS (64-bit)
#   signum-VERSION-linux-ARCH.tar.gz   any other distribution: unpack, sudo ./install.sh
#   signum-VERSION-linux-ARCH.debug    the program's debug symbols (gdb, crash reports)
# Run from the source tree: dist/package.sh [CXX=g++-12]. Needs dpkg-dev (dpkg-deb, dpkg-shlibdeps) and binutils.
# The binary finds its GUI next to itself (<bin>/../share/signum/www), so one build serves both.
set -eu
cd "$(dirname "$0")/.."
version=$(cat VERSION)
arch=$(dpkg --print-architecture)
out=$PWD/out
stage=$PWD/out/stage
rm -rf "$out" && mkdir -p "$stage"

./configure --prefix=/usr "$@"
make -j"$(nproc)"
make install DESTDIR="$stage" prefix=/usr bindir=/usr/bin datadir=/usr/share sysconfdir=/etc mandir=/usr/share/man
# debug symbols in their own file (for crash reports), a stripped program in the packages
objcopy --only-keep-debug --compress-debug-sections=zlib "$stage/usr/bin/signum" "$out/signum-$version-linux-$arch.debug"
objcopy --strip-debug --add-gnu-debuglink="$out/signum-$version-linux-$arch.debug" "$stage/usr/bin/signum"
# the example units start the installed program
sed -i 's|/usr/local/bin/signum|/usr/bin/signum|' "$stage"/usr/share/signum/examples/*.service
gzip -9n "$stage"/usr/share/man/man1/signum.1

# ---- .deb
deb=$out/deb
mkdir -p "$deb/DEBIAN"
cp -a "$stage"/. "$deb"/
mkdir -p "$deb/usr/share/doc/signum"
printf 'Signum, a P1/P2/P3 HF data modem in software.\nCopyright (C) 2026 Jeff Francis, N0GQ\nLicense: GPL-3.0-or-later (/usr/share/common-licenses/GPL-3)\n' > "$deb/usr/share/doc/signum/copyright"
# library dependencies from the binary (dpkg-shlibdeps wants a debian/control to read)
t=$(mktemp -d)
mkdir -p "$t/debian"
printf 'Source: signum\n\nPackage: signum\nArchitecture: any\n' > "$t/debian/control"
shlibs=$(cd "$t" && dpkg-shlibdeps -O "$deb/usr/bin/signum" | sed -n 's/^shlibs:Depends=//p')
rm -rf "$t"
size=$(du -sk "$deb" --exclude=DEBIAN | cut -f1)
cat > "$deb/DEBIAN/control" <<EOF
Package: signum
Version: $version
Architecture: $arch
Maintainer: Jeff Francis, N0GQ <https://github.com/jfrancis42/signum>
Installed-Size: $size
Depends: $shlibs, libcap2-bin
Recommends: python3, alsa-utils
Suggests: pat, libhamlib-utils
Section: hamradio
Priority: optional
Homepage: https://github.com/jfrancis42/signum
Description: P1/P2/P3 HF data modem in software
 Signum turns a Linux computer with a sound card into an HF data modem for
 P1, P2 and P3 links. Host programs such as Pat (Winlink) use it through a
 virtual serial port as they would a hardware modem. Browser GUI, terminal
 front panel, telnet and a WebSocket API. Built without encryption.
EOF
echo /etc/signum/calibration.conf > "$deb/DEBIAN/conffiles"
cat > "$deb/DEBIAN/postinst" <<'EOF'
#!/bin/sh
# real-time priority: dpkg does not keep file capabilities, so set them here
set -e
if [ "$1" = configure ]; then
	setcap cap_sys_nice,cap_ipc_lock+ep /usr/bin/signum 2>/dev/null ||
		echo "signum: setcap failed; Signum runs without real-time priority (docs: /usr/share/doc/signum/docs/md/performance.md)" >&2
fi
exit 0
EOF
chmod 755 "$deb/DEBIAN/postinst"
dpkg-deb --root-owner-group --build "$deb" "$out/signum_${version}_${arch}.deb"

# ---- tarball
name=signum-$version-linux-$arch
tb=$out/$name
mkdir -p "$tb"
cp -a "$stage"/. "$tb"/
cp dist/install.sh "$tb"/install.sh
chmod 755 "$tb"/install.sh
tar -C "$out" --owner=0 --group=0 -czf "$out/$name.tar.gz" "$name"

rm -rf "$stage" "$deb" "$tb"
ls -l "$out"
