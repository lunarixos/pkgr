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
    shift 2
    for dependency; do printf 'depend=%s\n' "$dependency" >>package.info; done
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
cp "$consumer" repo/
rm stage/usr/bin/consumer stage/usr/bin/example
printf 'suite\n' >stage/usr/bin/suite
suite="$(write_package suite 1.0-1 consumer example)"
cp "$suite" repo/
mkdir resolved
"$pkgr" --root resolved --repo repo install suite
"$pkgr" --root resolved list >installed
printf '%s\n' 'consumer 1.0-1' 'example 1.0-2' 'suite 1.0-1' >expected
cmp expected installed

mv stage/usr/bin/suite stage/usr/bin/broken
broken="$(write_package broken 1.0-1 consumer missing)"
mkdir unresolved
expect_failure "$pkgr" --root unresolved --repo repo install "./$broken"
grep -F 'package missing is unavailable' expected.err
test -z "$("$pkgr" --root unresolved list)"

mv stage/usr/bin/broken stage/usr/bin/cycle
cycle="$(write_package cycle 1.0-1 consumer loop)"
mv stage/usr/bin/cycle stage/usr/bin/loop
loop="$(write_package loop 1.0-1 cycle)"
cp "$loop" repo/
expect_failure "$pkgr" --root unresolved --repo repo install "./$cycle"
grep -F 'dependency cycle involving cycle' expected.err
test -z "$("$pkgr" --root unresolved list)"
mkdir updates
rm stage/usr/bin/loop
printf 'consumer upgraded\n' >stage/usr/bin/consumer
consumer_update="$(write_package consumer 1.0-2 freshdep)"
mv "$consumer_update" updates/
rm stage/usr/bin/consumer
printf 'new dependency\n' >stage/usr/bin/freshdep
freshdep="$(write_package freshdep 1.0-1)"
mv "$freshdep" updates/
rm stage/usr/bin/freshdep
printf 'example upgraded\n' >stage/usr/bin/example
bad_update="$(write_package example 1.0-3 missing)"
mv "$bad_update" updates/
expect_failure "$pkgr" --root resolved --repo updates upgrade --all
grep -F 'package missing is unavailable' expected.err
"$pkgr" --root resolved list >installed
cmp expected installed

example_update="$(write_package example 1.0-3)"
mv "$example_update" updates/
"$pkgr" --root resolved --repo updates upgrade --all
"$pkgr" --root resolved list >installed
printf '%s\n' 'consumer 1.0-2' 'example 1.0-3' 'freshdep 1.0-1' 'suite 1.0-1' >expected
cmp expected installed
"$pkgr" --root resolved --repo updates upgrade --all | grep -Fx 'No package updates available in the selected repository'
"$pkgr" --root resolved --repo updates upgrade example | grep -Fx 'No newer version of example'
mkdir system system-repo
rm -rf stage
mkdir -p stage/bin stage/etc
printf 'old shell\n' >stage/bin/shell
ln -s shell stage/bin/sh
printf 'old config\n' >stage/etc/test.conf
printf 'name=core\nversion=1.0\narch=noarch\nessential=yes\nconfig=etc/test.conf\n' >package.info
oldcore="$("$pkgr" pack package.info stage)"
cp -a stage/. system/
printf 'local config\n' >system/etc/test.conf
"$pkgr" --root system adopt "./$oldcore"
expect_failure "$pkgr" --root system remove core
grep -F 'essential system package' expected.err
mkdir -p modified
cp -a stage/. modified/
printf 'unknown binary\n' >modified/bin/shell
expect_failure "$pkgr" --root modified adopt "./$oldcore"
grep -F 'cannot adopt modified file: bin/shell' expected.err

printf 'new shell\n' >stage/bin/shell
rm stage/bin/sh
ln -s ./shell stage/bin/sh
printf 'new config\n' >stage/etc/test.conf
sed -i 's/version=1.0/version=2.0/' package.info
newcore="$("$pkgr" pack package.info stage)"
mv "$newcore" system-repo/
rm -rf stage
mkdir -p stage/usr/bin
printf 'app\n' >stage/usr/bin/app
app="$(write_package app 1.0 'core>=2.0')"
"$pkgr" --root system --repo system-repo install "./$app"
grep -Fx 'new shell' system/bin/sh
test "$(readlink system/bin/sh)" = ./shell
grep -Fx 'local config' system/etc/test.conf
"$pkgr" --root system list | grep -Fx 'core 2.0'

mv stage/usr/bin/app stage/usr/bin/pin
pin="$(write_package pin 1.0 'core=2.0')"
"$pkgr" --root system install "./$pin"
rm -rf stage
mkdir -p stage/bin stage/etc
printf 'third shell\n' >stage/bin/shell
printf 'third config\n' >stage/etc/test.conf
printf 'name=core\nversion=3.0\narch=noarch\nconfig=etc/test.conf\n' >package.info
thirdcore="$("$pkgr" pack package.info stage)"
mv "$thirdcore" system-repo/
expect_failure "$pkgr" --root system --repo system-repo upgrade --all
grep -F 'pin requires core=2.0' expected.err
grep -Fx 'new shell' system/bin/sh
"$pkgr" --root system remove pin
"$pkgr" --root system --repo system-repo upgrade --all
grep -Fx 'third shell' system/bin/shell
test ! -L system/bin/sh
grep -Fx 'local config' system/etc/test.conf

mkdir system/var/lib/pkgr/.batch
batch=system/var/lib/pkgr/.batch
for fixture in '0 app usr/bin/app' '1 core bin/shell'; do
    set -- $fixture
    pkgdir="$batch/pkg.$1"
    mkdir "$pkgdir"
    cp "system/var/lib/pkgr/$2.record" "$pkgdir/before"
    printf 'name=%s\nversion=9.0\narch=noarch\nfile=%s\n' "$2" "$3" >"$pkgdir/after"
    cp "$pkgdir/after" "system/var/lib/pkgr/$2.record"
    slot=$(awk -v file="file=$3" '$0 == file { print slot+0 } /^file=/ { ++slot }' "$pkgdir/before")
    ln "system/$3" "$pkgdir/old.$slot"
    printf 'interrupted batch\n' >"$pkgdir/new.0"
    rm "system/$3"
    ln "$pkgdir/new.0" "system/$3"
done
"$pkgr" --root system recover
grep -Fx 'app' system/usr/bin/app
grep -Fx 'third shell' system/bin/shell
grep -Fx 'local config' system/etc/test.conf
"$pkgr" --root system list >installed
printf '%s\n' 'app 1.0' 'core 3.0' >expected
cmp expected installed
test ! -e "$batch"
rm -rf stage
mkdir -p stage/usr/bin admin
printf '#!/bin/sh\nexit 0\n' >stage/usr/bin/doas
chmod 4755 stage/usr/bin/doas
printf 'name=opendoas\nversion=1.0\narch=noarch\n' >package.info
admin_package="$("$pkgr" pack package.info stage)"
"$pkgr" --root admin install "./$admin_package"
test -u admin/usr/bin/doas
sed -i 's/name=opendoas/name=other/' package.info
expect_failure "$pkgr" pack package.info stage
sed -i 's/name=other/name=opendoas/' package.info
mv stage/usr/bin/doas stage/usr/bin/other
expect_failure "$pkgr" pack package.info stage
mv stage/usr/bin/other stage/usr/bin/sudo
expect_failure "$pkgr" pack package.info stage
sed -i 's/name=opendoas/name=sudo/' package.info
sudo_package="$("$pkgr" pack package.info stage)"
"$pkgr" --root admin install "./$sudo_package"
test -u admin/usr/bin/sudo
printf 'smoke tests passed\n'
