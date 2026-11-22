#!/bin/sh
set -eu
stage=$1
work=$2
version=2.71.0
filename="fastfetch-$version.tar.gz"
checksum=e71d93de41e8e4b2554c36d2ae51758112a21ecf3a8c98605a2c689251e62987
if [ -n "${PKGR_SOURCE_CACHE:-}" ]; then
    cp -- "$PKGR_SOURCE_CACHE/$filename" "$work/$filename"
else
    curl --fail --location --proto '=https' --retry 2 \
        "https://codeload.github.com/fastfetch-cli/fastfetch/tar.gz/refs/tags/$version" \
        -o "$work/$filename"
fi
printf '%s  %s\n' "$checksum" "$work/$filename" | sha256sum -c -
tar -xzf "$work/$filename" -C "$work"
source="$work/fastfetch-$version"

cmake -S "$source" -B "$work/build" -DCMAKE_BUILD_TYPE=MinSizeRel \
    -DCMAKE_EXE_LINKER_FLAGS=-static -DBINARY_LINK_TYPE=dlopen \
    -DBUILD_FLASHFETCH=OFF -DENABLE_LTO=OFF -DSET_TWEAK=OFF \
    -DENABLE_IMAGE_LOGO=OFF -DENABLE_EMBEDDED_PCIIDS=OFF -DENABLE_EMBEDDED_AMDGPUIDS=OFF \
    -DENABLE_VULKAN=OFF -DENABLE_WAYLAND=OFF -DENABLE_X11=OFF -DENABLE_XCB=OFF -DENABLE_XCB_RANDR=OFF -DENABLE_XRANDR=OFF \
    -DENABLE_DRM=OFF -DENABLE_DRM_AMDGPU=OFF -DENABLE_VADRM=OFF -DENABLE_VAX11=OFF -DENABLE_VDPAU=OFF \
    -DENABLE_GIO=OFF -DENABLE_DCONF=OFF -DENABLE_EET=OFF -DENABLE_DBUS=OFF \
    -DENABLE_SQLITE3=OFF -DENABLE_RPM=OFF -DENABLE_EGL=OFF -DENABLE_GLX=OFF \
    -DENABLE_OPENCL=OFF -DENABLE_ELF=OFF -DENABLE_THREADS=OFF -DENABLE_ZLIB=OFF \
    -DENABLE_LIBZFS=OFF -DENABLE_LUA=OFF
cmake --build "$work/build" --target fastfetch --parallel "${PKGR_JOBS:-2}"
install -Dm755 "$work/build/fastfetch" "$stage/usr/bin/fastfetch"
install -Dm644 "$source/LICENSE" "$stage/usr/share/licenses/fastfetch/LICENSE"
