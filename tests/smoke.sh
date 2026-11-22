#!/bin/sh
set -eu
pkgr="$(pwd)/pkgr"
test_dir="$(mktemp -d)"
trap 'rm -rf "$test_dir"' EXIT
cd "$test_dir"

expect_failure() {
    if "$@" >expected.out 2>expected.err; then
        echo "unexpected success: $*" >&2
        exit 1
    fi
}

write_package() {
    printf 'name=%s\nversion=%s\narch=noarch\n' "$1" "$2" >package.info
    if [ "$#" = 3 ]; then printf 'depend=%s\n' "$3" >>package.info; fi
    "$pkgr" pack package.info stage
}

"$pkgr" -h >short-help
"$pkgr" --help >long-help
cmp short-help long-help
"$pkgr" -v >short-version
"$pkgr" --version >long-version
cmp short-version long-version
expect_failure "$pkgr" unknown-command
mkdir -p root/usr/bin stage/usr/bin stage/usr/share/pkgr-test
printf 'original\n' >stage/usr/bin/example
printf 'old documentation\n' >stage/usr/share/pkgr-test/old
one="$(write_package example 1.0-1)"
"$pkgr" info "./$one" | grep -F 'Name:         example'
printf 'unmanaged\n' >root/usr/bin/example
expect_failure "$pkgr" --root root install "./$one"
grep -Fx unmanaged root/usr/bin/example
rm root/usr/bin/example
"$pkgr" --root root install "./$one"
cmp stage/usr/bin/example root/usr/bin/example
"$pkgr" --root root list | grep -Fx 'example 1.0-1'
expect_failure "$pkgr" --root root install "./$one"
expect_failure "$pkgr" --root root upgrade "./$one"

printf 'replacement\n' >stage/usr/bin/example
mv stage/usr/share/pkgr-test/old stage/usr/share/pkgr-test/new
two="$(write_package example 1.0-2)"
mkdir -p repo
cp "$one" "$two" repo/
"$pkgr" --repo repo search example | grep -Fx 'example 1.0-2'
"$pkgr" --root root --repo repo upgrade example
cmp stage/usr/bin/example root/usr/bin/example
test ! -e root/usr/share/pkgr-test/old
test -f root/usr/share/pkgr-test/new
"$pkgr" --root root info example | grep -F 'Version:      1.0-2'

mkdir -p root/var/lib/pkgr/.pending
pending=root/var/lib/pkgr/.pending
cp root/var/lib/pkgr/example.record "$pending/before"
cp "$pending/before" "$pending/after"
path_number=0
while IFS= read -r field; do
    case "$field" in
        file=*)
            path=${field#file=}
            ln "root/$path" "$pending/old.$path_number"
            printf 'interrupted write\n' >"$pending/new.$path_number"
            rm "root/$path"
            ln "$pending/new.$path_number" "root/$path"
            path_number=$((path_number + 1))
            ;;
    esac
done <"$pending/after"
"$pkgr" --root root list | grep -Fx 'example 1.0-2'
cmp stage/usr/bin/example root/usr/bin/example
test ! -d "$pending"

rm stage/usr/bin/example stage/usr/share/pkgr-test/new
printf 'consumer\n' >stage/usr/bin/consumer
consumer="$(write_package consumer 1.0-1 example)"
mkdir empty outside
ln -s "$test_dir/outside" empty/usr
expect_failure "$pkgr" --root empty install "./$one"
test ! -e outside/bin/example
rm empty/usr
expect_failure "$pkgr" --root empty install "./$consumer"
"$pkgr" --root root install "./$consumer"
expect_failure "$pkgr" --root root remove example
"$pkgr" --root root remove consumer
rm root/usr/bin/example
printf 'conflicting package\n' >stage/usr/bin/example
conflict="$(write_package conflict 1.0-1)"
expect_failure "$pkgr" --root root install "./$conflict"
grep -F 'example already owns usr/bin/example' expected.err
test ! -e root/usr/bin/example
"$pkgr" --root root remove example
test ! -e root/usr/bin/example
test ! -e root/usr/share/pkgr-test/new
test -z "$("$pkgr" --root root list)"
mkdir -p default-root/usr/share/pkgr/repo
cp "$one" "$two" default-root/usr/share/pkgr/repo/
"$pkgr" --root default-root search example | grep -Fx 'example 1.0-2'
"$pkgr" --root default-root install example
"$pkgr" --root default-root list | grep -Fx 'example 1.0-2'
"$pkgr" --root default-root remove example

mkdir first-only
cp "$one" first-only/
PKGR_REPO=first-only "$pkgr" --root default-root install example
"$pkgr" --root default-root list | grep -Fx 'example 1.0-1'
PKGR_REPO=first-only "$pkgr" --root default-root --repo repo upgrade example
"$pkgr" --root default-root list | grep -Fx 'example 1.0-2'
expect_failure "$pkgr" --root empty install example
grep -F 'repository empty/usr/share/pkgr/repo does not exist' expected.err
expect_failure "$pkgr" --root default-root install missing
grep -F 'package missing is unavailable' expected.err
printf 'smoke tests passed\n'
