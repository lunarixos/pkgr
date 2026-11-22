#include "pkgr.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>

static DIR *open_repo(const char *directory)
{
    DIR *dir = opendir(directory);
    if (!dir && errno == ENOENT)
        fail("repository %s does not exist; add packages there or use --repo DIR", directory);
    if (!dir)
        syserror("open repository", directory);
    return dir;
}

static struct dirent *next_package(DIR *dir)
{
    for (;;) {
        errno = 0;
        struct dirent *entry = readdir(dir);
        if (!entry && errno)
            syserror("scan", "repository");
        if (!entry || has_suffix(entry->d_name, PACKAGE_SUFFIX))
            return entry;
    }
}

char *repo_find(const char *directory, const char *name)
{
    DIR *dir = open_repo(directory);
    struct utsname system;
    if (uname(&system) != 0)
        syserror("read", "machine architecture");
    char *result = NULL, *version = NULL;
    bool ambiguous = false;
    for (struct dirent *entry; (entry = next_package(dir)); ) {
        char *path = path_join(directory, entry->d_name);
        struct package pkg = {0};
        pkg_read(path, &pkg, -1, true);
        if (!strcmp(pkg.name, name) &&
            (!strcmp(pkg.arch, "noarch") || !strcmp(pkg.arch, system.machine))) {
            int order = version ? strverscmp(pkg.version, version) : 1;
            if (!order)
                ambiguous = true;
            if (order > 0) {
                ambiguous = false;
                free(version);
                free(result);
                version = copy_string(pkg.version);
                result = path;
                path = NULL;
            }
        }
        pkg_free(&pkg);
        free(path);
    }
    closedir(dir);
    if (ambiguous)
        fail("ambiguous repository version: %s %s", name, version);
    free(version);
    if (!result)
        fail("package %s is unavailable for %s in %s", name, system.machine, directory);
    return result;
}

void repo_search(const char *directory, const char *filter)
{
    DIR *dir = open_repo(directory);
    for (struct dirent *entry; (entry = next_package(dir)); ) {
        char *path = path_join(directory, entry->d_name);
        struct package pkg = {0};
        pkg_read(path, &pkg, -1, true);
        if (strstr(pkg.name, filter) || (pkg.description && strstr(pkg.description, filter))) {
            printf("%s %s", pkg.name, pkg.version);
            if (pkg.description)
                printf("  %s", pkg.description);
            putchar('\n');
        }
        pkg_free(&pkg);
        free(path);
    }
    closedir(dir);
}
