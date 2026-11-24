#!/bin/sh
set -eu

stage=$1
work=$2
version=2.56.1
archive="git-$version.tar.gz"
checksum=7352791502f1b9745e7414e15e4ae9b7bb4773428b9a1b4f7beb3fdd1073aba4
if [ -n "${PKGR_SOURCE_CACHE:-}" ]; then
    cp -- "$PKGR_SOURCE_CACHE/$archive" "$work/$archive"
else
    curl --fail --location --proto '=https' --retry 2 \
        "https://codeload.github.com/git/git/tar.gz/refs/tags/v$version" \
        -o "$work/$archive"
fi
printf '%s  %s\n' "$checksum" "$work/$archive" | sha256sum -c -
tar -xzf "$work/$archive" -C "$work"
source="$work/git-$version"

make -C "$source" -j"${PKGR_JOBS:-2}" prefix=/usr gitexecdir=/usr/lib/git-core \
    GIT_VERSION="$version" SHELL_PATH=/bin/sh NO_GETTEXT=YesPlease NO_PERL=YesPlease \
    NO_PYTHON=YesPlease NO_TCLTK=YesPlease NO_EXPAT=YesPlease NO_OPENSSL=YesPlease \
    git git-remote-http
make -C "$source/templates" DESTDIR="$stage" template_instdir=/usr/share/git-core/templates install

runtime="$stage/usr/lib/lunarix/git"
helpers="$stage/usr/lib/git-core"
install -d "$runtime" "$helpers" "$stage/usr/bin"
for program in git git-remote-http; do
    install -m755 -s "$source/$program" "$runtime/$program"
    dependencies=$(LC_ALL=C ldd "$source/$program")
    case "$dependencies" in
        *'not found'*) echo "Missing runtime libraries for $program" >&2; exit 1 ;;
    esac
    printf '%s\n' "$dependencies" | awk '
        $2 == "=>" && $3 ~ /^\// {print $1, $3; next}
        $1 ~ /^\// {n = split($1, parts, "/"); print parts[n], $1}
    ' | while read -r name path; do
        install -m755 "$path" "$runtime/${name##*/}"
    done
done
test -x "$runtime/ld-linux-x86-64.so.2"

cat > "$stage/usr/bin/git" <<'LAUNCHER'
#!/bin/sh
runtime=/usr/lib/lunarix/git
if [ -r /usr/share/lunarix/ca-certificates.crt ]; then
    export GIT_SSL_CAINFO="${GIT_SSL_CAINFO:-/usr/share/lunarix/ca-certificates.crt}"
fi
exec "$runtime/ld-linux-x86-64.so.2" --library-path "$runtime" "$runtime/git" "$@"
LAUNCHER
cat > "$helpers/git-remote-http" <<'LAUNCHER'
#!/bin/sh
runtime=/usr/lib/lunarix/git
exec "$runtime/ld-linux-x86-64.so.2" --argv0 "$0" --library-path "$runtime" "$runtime/git-remote-http" "$@"
LAUNCHER
cp "$helpers/git-remote-http" "$helpers/git-remote-https"
for command in upload-pack receive-pack upload-archive; do
    printf '#!/bin/sh\nexec /usr/bin/git %s "$@"\n' "$command" > "$helpers/git-$command"
done
chmod 0755 "$stage/usr/bin/git" "$helpers/"*
install -Dm644 "$source/COPYING" "$stage/usr/share/licenses/git/COPYING"
