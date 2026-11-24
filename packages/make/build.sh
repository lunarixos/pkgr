#!/bin/sh
set -eu

stage=$1
work=$2
version=4.4.1
archive="make-$version.tar.gz"
checksum=dd16fb1d67bfab79a72f5e8390735c49e3e8e70b4945a15ab1f81ddb78658fb3

if [ -n "${PKGR_SOURCE_CACHE:-}" ]; then
    cp -- "$PKGR_SOURCE_CACHE/$archive" "$work/$archive"
else
    curl --fail --location --proto '=https' --retry 2 \
        "https://ftp.gnu.org/gnu/make/$archive" -o "$work/$archive"
fi
printf '%s  %s\n' "$checksum" "$work/$archive" | sha256sum -c -
tar -xzf "$work/$archive" -C "$work"
cd "$work/make-$version"

LDFLAGS=-static ./configure --prefix=/usr --disable-nls --without-guile
make -j"${PKGR_JOBS:-2}"
install -Dm755 -s make "$stage/usr/bin/make"
install -Dm644 COPYING "$stage/usr/share/licenses/make/COPYING"
