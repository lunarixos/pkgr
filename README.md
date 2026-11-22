# pkgr

pkgr is a C package manager for [Lunarix](https://github.com/lunarixos/lunarix). It builds trusted local source recipes
and installs binary packages from a file or a local directory repository. Both
paths use the same compressed tar format and installed-package database.

## Build and try it

Install a C compiler, Make, pkg-config, and the libarchive development files.
Build and run the smoke tests from this directory.

~~~sh
make
make check
./pkgr --help
~~~

Run against a disposable root to try the package here.

~~~sh
root=$(mktemp -d)
make install-repo DESTDIR="$root"
./pkgr --root "$root" search fastfetch
./pkgr --root "$root" install fastfetch
./pkgr --root "$root" list
./pkgr --root "$root" remove fastfetch
~~~

The tested Lunarix binary is build/pkgr. To build a static binary yourself,
supply static libarchive and its compression libraries to make static. The
Lunarix test used libarchive with gzip support linked against static zlib.

~~~sh
make static ARCHIVE_CFLAGS="-I/path/to/include" \
  ARCHIVE_LIBS="/path/to/libarchive.a /path/to/libz.a"
~~~

To try it inside an installed Lunarix VM, copy build/pkgr and the Fastfetch
archive from build/repo/ into the guest. From their directory, run:

~~~sh
install -Dm755 ./pkgr /usr/bin/pkgr
install -Dm644 ./fastfetch-2.71.0-1.x86_64.pkgr.tar.gz \
  /usr/share/pkgr/repo/fastfetch-2.71.0-1.x86_64.pkgr.tar.gz
pkgr search fastfetch
pkgr install fastfetch
fastfetch --structure OS:Kernel:Shell:Memory --logo none
pkgr remove fastfetch
~~~

Installing to the real guest root requires write access to the system and its
package database. Omit --root when running as root in the installed guest.

## Recipes and repositories

A recipe directory contains package.info metadata and build.sh. The script takes
a stage directory and a scratch directory. It writes its installable files to
the stage. Run trusted recipes as a regular user in a build environment. The
binary installer reads the archive and never runs the recipe or package hooks.

~~~sh
./pkgr build fastfetch
~~~

The Fastfetch recipe uses CMake, a C compiler with static libc support, curl,
tar and sha256sum. It pins and checks the upstream source archive. Set
PKGR_SOURCE_CACHE to an absolute directory with the source tarball to build
offline, and PKGR_JOBS to control the build parallelism.

Metadata uses name, version, arch and description fields with optional repeated
depend fields naming packages that must already be installed. Source recipes
turn into .pkgr.tar.gz archives with a .PKGRINFO manifest and regular files below
usr/. pkgr records file ownership, rejects existing unowned destinations and
blocks removal of required packages. It uses a filesystem journal to recover an
interrupted change on the next invocation. The package database and destination
files must be on the same filesystem, as on Lunarix's default ext4 install.

By default, search, install NAME and upgrade NAME look for archives in
/usr/share/pkgr/repo under the selected root. The --repo option takes priority
over PKGR_REPO, and PKGR_REPO takes priority over that default. An explicit
archive path also works. For name lookup, pkgr selects the newest compatible
version using GNU version sort. This release consumes trusted local archives.
Remote signed indexes and automatic dependency fetching are future work.

The staged recipe workflow follows pkgit and makepkg. A separate local package
database and an explicit installation root follow XBPS. Archive handling uses
libarchive.
