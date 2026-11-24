#!/bin/sh
set -eu

stage=$1
work=$2
prefix=/usr/lib/lunarix/base-devel
sdk="$stage$prefix"
sysroot="$sdk/sysroot"
lib="$sysroot/usr/lib"
runtime="$prefix/sysroot/usr/lib"
loader="$runtime/ld-linux-x86-64.so.2"

for tool in pacman gcc g++ patchelf rsync ldd cmake ninja pkgconf; do
    command -v "$tool" >/dev/null || { echo "Install $tool on the build host." >&2; exit 1; }
done
case "$(gcc -dumpmachine)" in
    x86_64-pc-linux-gnu) ;;
    *) echo 'Build this recipe on an x86-64 Arch Linux host.' >&2; exit 1 ;;
esac

install -d "$sdk/bin" "$sdk/share" "$sysroot/usr/include" "$lib"
pacman -Qlq glibc linux-api-headers gcc > "$work/headers.list"
while IFS= read -r source; do
    [ -f "$source" ] || continue
    case "$source" in
        /usr/include/*) install -Dm644 "$source" "$sysroot$source" ;;
        /usr/lib/*)
            name=${source#/usr/lib/}
            case "$name" in
                */*) ;;
                crt*.o|*crt1.o|libc*|libm.*|libm-*|libmvec*|libdl*|libpthread*|libresolv*|librt.*|libutil*|libanl*|libg.a)
                    install -m644 "$source" "$lib/$name"
                    ;;
            esac
            ;;
    esac
done < "$work/headers.list"

stage_tool() {
    binary=$1
    destination=$2
    install -Dm755 "$binary" "$destination"
    dependencies=$(LC_ALL=C ldd "$binary")
    case "$dependencies" in
        *'not found'*) echo "Missing runtime libraries for $binary" >&2; exit 1 ;;
    esac
    printf '%s\n' "$dependencies" | awk '
        $2 == "=>" && $3 ~ /^\// {print $1, $3; next}
        $1 ~ /^\// {print $1, $1}
    ' | while read -r name path; do
        name=${name##*/}
        if [ ! -f "$lib/$name" ]; then
            install -m755 "$path" "$lib/$name"
        fi
    done
    if patchelf --print-interpreter "$destination" >/dev/null 2>&1; then
        patchelf --set-interpreter "$loader" "$destination"
    fi
    patchelf --force-rpath --set-rpath "$runtime" "$destination"
}

gcc_dir=$(dirname "$(gcc -print-libgcc-file-name)")
gcc_relative=${gcc_dir#/usr/}
gcc_version=${gcc_dir##*/}
gcc_target=$(gcc -dumpmachine)
install -d "$sdk/$gcc_relative"
rsync -aL --no-owner --no-group "$gcc_dir/include/" "$sdk/$gcc_relative/include/"
for source in "$gcc_dir/"*.o "$gcc_dir/"*.a "$gcc_dir/"*.so "$gcc_dir/"*.spec; do
    [ -f "$source" ] || continue
    name=${source##*/}
    case "$name" in
        liblto_plugin.so) stage_tool "$source" "$sdk/$gcc_relative/$name" ;;
        *) install -m644 "$source" "$sdk/$gcc_relative/$name" ;;
    esac
done
for program in cc1 cc1plus collect2 lto1 lto-wrapper; do
    stage_tool "$gcc_dir/$program" "$sdk/$gcc_relative/$program"
done

for program in as ld ar ranlib nm objcopy objdump readelf strip strings size \
               cmake ctest cpack ninja bash pkgconf curl patch diff gawk bison flex m4 \
               xz bzip2 unzip; do
    stage_tool "$(command -v "$program")" "$sdk/bin/$program"
done
stage_tool "$(command -v gcc)" "$sdk/bin/gcc.real"
stage_tool "$(command -v g++)" "$sdk/bin/g++.real"
cp "$sdk/bin/pkgconf" "$sdk/bin/pkg-config"
for library in libgcc_s.so.1 libstdc++.so.6 libz.so.1; do
    path=$(gcc -print-file-name="$library")
    [ -f "$path" ] || { echo "Missing compiler library $library" >&2; exit 1; }
    install -m644 "$path" "$lib/$library"
done
cp "$lib/libz.so.1" "$lib/libz.so"
install -Dm644 /usr/include/zlib.h "$sysroot/usr/include/zlib.h"
install -Dm644 /usr/include/zconf.h "$sysroot/usr/include/zconf.h"
rsync -aL --no-owner --no-group /usr/share/cmake/ "$sdk/share/cmake/"
rsync -aL --no-owner --no-group /usr/share/bison/ "$sdk/share/bison/"

for command in gcc g++ cc c++; do
    case "$command" in
        gcc|cc) driver=gcc ;;
        *) driver=g++ ;;
    esac
    cat > "$sdk/bin/$command" <<EOF_WRAPPER
#!/bin/sh
sdk=$prefix
driver=$driver
if [ "\$driver" = g++ ]; then
    set -- -isystem "\$sdk/sysroot/usr/include/c++/$gcc_version" \\
        -isystem "\$sdk/sysroot/usr/include/c++/$gcc_version/$gcc_target" \\
        -isystem "\$sdk/sysroot/usr/include/c++/$gcc_version/backward" "\$@"
fi
exec "\$sdk/bin/\$driver.real" --sysroot="\$sdk/sysroot" -B"\$sdk/bin/" \\
    -Wl,--dynamic-linker=$loader -Wl,-rpath,$runtime "\$@"
EOF_WRAPPER
    chmod 755 "$sdk/bin/$command"
done

install -d "$stage/usr/bin"
cat > "$stage/usr/bin/lunarix-devel" <<'EOF_LAUNCHER'
#!/bin/sh
sdk=/usr/lib/lunarix/base-devel
export PATH="$sdk/bin:$PATH"
export CC="$sdk/bin/gcc" CXX="$sdk/bin/g++"
export PKG_CONFIG_SYSROOT_DIR="$sdk/sysroot"
export PKG_CONFIG_LIBDIR="$sdk/sysroot/usr/lib/pkgconfig:$sdk/sysroot/usr/share/pkgconfig"
export BISON_PKGDATADIR="$sdk/share/bison"
export SSL_CERT_FILE="${SSL_CERT_FILE:-/usr/share/lunarix/ca-certificates.crt}"
export CURL_CA_BUNDLE="${CURL_CA_BUNDLE:-$SSL_CERT_FILE}"
export CMAKE_TLS_CAINFO="${CMAKE_TLS_CAINFO:-$SSL_CERT_FILE}"
export CMAKE_PREFIX_PATH="$sdk/sysroot/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
if [ "$#" -eq 0 ]; then
    exec bash
fi
exec "$@"
EOF_LAUNCHER
chmod 755 "$stage/usr/bin/lunarix-devel"

install -d "$stage/usr/share/licenses/base-devel"
pacman -Q gcc binutils glibc linux-api-headers cmake ninja bash pkgconf curl \
    patch diffutils gawk bison flex m4 xz bzip2 unzip > "$stage/usr/share/licenses/base-devel/components"
for package in gcc binutils glibc linux-api-headers cmake ninja bash pkgconf curl \
               patch diffutils gawk bison flex m4 xz bzip2 unzip; do
    if [ -d "/usr/share/licenses/$package" ]; then
        cp -rL "/usr/share/licenses/$package" "$stage/usr/share/licenses/base-devel/"
    fi
done
