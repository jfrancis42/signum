#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Jeff Francis, N0GQ
# install.sh: install Signum from this release tarball (for a distribution without the .deb).
#   sudo ./install.sh [PREFIX]        install under PREFIX (default /usr/local)
#   sudo ./install.sh --uninstall [PREFIX]
# Needs ALSA's library (alsa-lib / libasound2), and setcap (libcap) for real-time priority.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
un=no
[ "${1:-}" = --uninstall ] && { un=yes; shift; }
prefix=${1:-/usr/local}
if [ "$prefix" = /usr ]; then etc=/etc; else etc=$prefix/etc; fi

if [ $un = yes ]; then
	rm -f "$prefix/bin/signum" "$prefix/bin/signum-start" "$prefix/bin/signum-loopback" "$prefix/share/man/man1/signum.1.gz"
	rm -rf "$prefix/share/signum" "$prefix/share/doc/signum"
	rm -f "$etc/signum/calibration.conf"; rmdir "$etc/signum" 2>/dev/null || true
	echo "Signum removed from $prefix"
	exit 0
fi

mkdir -p "$prefix/bin" "$prefix/share/man/man1" "$prefix/share/doc" "$etc/signum"
install -m 755 "$here/usr/bin/signum" "$prefix/bin/signum"
for s in signum-start signum-loopback; do
	sed "s|^datadir=/usr/share/signum|datadir=$prefix/share/signum|" "$here/usr/bin/$s" > "$prefix/bin/$s"
	chmod 755 "$prefix/bin/$s"
done
rm -rf "$prefix/share/signum" "$prefix/share/doc/signum"
cp -R "$here/usr/share/signum" "$prefix/share/signum"
cp -R "$here/usr/share/doc/signum" "$prefix/share/doc/signum"
sed -i "s|/usr/bin/signum|$prefix/bin/signum|" "$prefix/share/signum/examples/"*.service
install -m 644 "$here/usr/share/man/man1/signum.1.gz" "$prefix/share/man/man1/signum.1.gz"
[ -e "$etc/signum/calibration.conf" ] || install -m 644 "$here/etc/signum/calibration.conf" "$etc/signum/calibration.conf"

if PATH="$PATH:/usr/sbin:/sbin" setcap cap_sys_nice,cap_ipc_lock+ep "$prefix/bin/signum" 2>/dev/null; then
	echo "Signum installed in $prefix, with real-time priority"
else
	echo "Signum installed in $prefix, WITHOUT real-time priority: install setcap (libcap) and run"
	echo "  sudo setcap cap_sys_nice,cap_ipc_lock+ep $prefix/bin/signum"
fi
"$prefix/bin/signum" --version | head -1
