# pkgr

pkgr is a C package manager for [Lunarix](https://github.com/lunarixos/lunarix). It builds trusted local source recipes
and installs binary packages from an archive, a local directory, or a signed HTTPS repository. Both
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
depend fields naming runtime dependencies. Use `name`, `name>=version`, or
`name=version`. Install and upgrade resolve requirements from the selected
repository and validate the planned set against installed packages.

Archives can contain regular files and symbolic links under `usr/`, `bin/`,
`sbin/`, `lib/`, and `etc/`. Declare a packaged configuration file with
`config=etc/path`. pkgr replaces the shipped default on upgrade if its
checksum still matches and preserves local edits. Mark packages needed for
the OS with `essential=yes`.

pkgr records file ownership and rejects existing unowned destinations.
`pkgr adopt FILE` registers files that match a trusted archive, with an
exception for its declared configuration files. Package files and the
database share a filesystem. On Lunarix, the boot deployment command
handles the separate EFI partition.

By default, `search`, `install NAME`, and `upgrade NAME` look for archives in
`/usr/share/pkgr/repo` under the selected root. The `--repo` option takes
priority over `PKGR_REPO`. With neither set, pkgr reads the HTTPS URL from
`/etc/pkgr/repository` when configured. An explicit archive path also works.

### Upgrade packages

~~~sh
pkgr upgrade --all
pkgr upgrade fastfetch
pkgr upgrade pkgr
~~~

Named upgrades and `--all` fetch a fresh index when the selected repository
uses HTTPS. `--all` checks installed packages against that repository and
skips any without newer versions there. pkgr stages and validates the complete update before any package
files are changed. One journal covers the batch. `pkgr recover` rolls back
an interrupted batch before the next update, and Lunarix runs recovery
at startup.

By default, `pkgr upgrade pkgr` downloads the prebuilt Lunarix package from
the latest release of the official GitHub repository. It uses curl with
HTTPS certificate verification and works without Git or a compiler in the
guest. Pass `--repo` explicitly to upgrade pkgr from a different package
repository, or pass the path to a local archive for an offline upgrade.

For a tagged release, build the Lunarix ISO from the matching revision and
attach its `build/pkgr-update.pkgr.tar.gz` to the GitHub Release under that
asset name. Lunarix uses that archive for offline updates too.

### Build from Git

A Git repository can supply the same `package.info` and `build.sh` used by
local recipes. Pass an HTTPS, SSH, or file URL. The optional arguments pick a
recipe directory inside the checkout and a Git revision. Use `.` for a recipe
in the repository root. Source projects need a recipe that declares the
package and stages its files. For a named recipe, pkgr looks in `packages/`
and then `/usr/share/pkgr/recipes/`. Set `PKGR_RECIPES` to use a specific
directory for named recipes.

~~~sh
pkgr --root "$root" install --build https://github.com/lunarixos/pkgr.git packages/fastfetch
pkgr build https://github.com/lunarixos/pkgr.git packages/fastfetch COMMIT
~~~

Only build repositories you trust. The recipe runs as your user. For a
system-wide install, run `pkgr build` as a regular user and install the
resulting archive as root.

### Git on Lunarix

The `git` recipe builds command-line Git and its HTTPS helper from the
upstream GitHub mirror. Build it on an x86-64 glibc Linux host with Git's C
build tools and the libcurl and zlib development files.

~~~sh
pkgr build git
~~~

Copy the resulting archive into Lunarix and install it with
`pkgr install ./git-2.56.1-1.x86_64.pkgr.tar.gz`. You can also put the
archive in the local repository and install it by name with `pkgr install git`.

### Lunarix base-devel

`base-devel` packages an optional x86-64 glibc toolchain with GCC, G++, libc
and Linux headers, binutils, CMake, Ninja, Bash, pkg-config, curl, and
common build utilities. It uses the separate `git` and `make` packages.

Build the package on an x86-64 Arch Linux host with those tools, rsync, and
patchelf. The recipe records the host component versions in the package.

~~~sh
pkgr build base-devel
~~~

Put the resulting archive in the same repository as the `git` and `make`
archives. On Lunarix, run `pkgr install base-devel` as root to install all
three. Run `lunarix-devel` as a regular user
for a shell with the toolchain on its path, or run
`lunarix-devel COMMAND` to invoke a specific build command. Compiled
programs use the toolchain's private glibc.

### Use an HTTPS binary repository

Set `PKGR_REPO` to the repository base URL. Provision its trusted public key
at `/etc/pkgr/repo.pem` under the selected installation root, or set
`PKGR_REPO_KEY` to the public key path. The following URL is an example.

~~~sh
export PKGR_REPO=https://packages.example.org/lunarix
export PKGR_REPO_KEY=/path/to/trusted-repo.pem
pkgr sync
pkgr search fastfetch
pkgr install fastfetch
~~~

Sync verifies the signed index before replacing the cached copy. Search works
offline after syncing. Install selects the newest compatible package and
verifies the downloaded archive against the index. Remote operations use
OpenSSL and either curl or HTTPS-enabled wget. Git recipes also use Git and
the tools required by the recipe.

To publish a repository, put the package archives in a directory served over
HTTPS. Generate its index and sign it with an RSA private key kept outside
that directory. Distribute the corresponding public key separately.

~~~sh
pkgr index /srv/lunarix > /srv/lunarix/index.tsv
openssl dgst -sha256 -sign /path/to/private-key.pem \
    -out /srv/lunarix/index.tsv.sig /srv/lunarix/index.tsv
~~~

The staged recipe workflow follows pkgit and makepkg. A separate local package
database and an explicit installation root follow XBPS. Archive handling uses
libarchive.
