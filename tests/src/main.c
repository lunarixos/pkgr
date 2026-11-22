#include "pkgr.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

_Noreturn void fail(const char *format, ...)
{
    va_list args;
    va_start(args, format);
    fputs("pkgr: ", stderr);
    vfprintf(stderr, format, args);
    fputc('\n', stderr);
    va_end(args);
    exit(1);
}

_Noreturn void syserror(const char *operation, const char *path)
{
    fail("%s %s: %s", operation, path, strerror(errno));
}

char *copy_string(const char *value)
{
    char *result = strdup(value);
    if (!result)
        fail("out of memory");
    return result;
}

void *grow_array(void *array, size_t count, size_t size)
{
    if (count > SIZE_MAX / size)
        fail("array is too large");
    void *result = realloc(array, count * size);
    if (!result)
        fail("out of memory");
    return result;
}

char *path_join(const char *base, const char *name)
{
    char *path;
    size_t length = strlen(base);
    const char *separator = length && base[length - 1] == '/' ? "" : "/";
    if (asprintf(&path, "%s%s%s", base, separator, name) < 0)
        fail("out of memory");
    return path;
}

static int remove_entry(const char *path, const struct stat *status, int type,
                        struct FTW *entry)
{
    (void)status;
    (void)type;
    (void)entry;
    return remove(path);
}

void remove_tree(const char *path)
{
    if (path && nftw(path, remove_entry, 16, FTW_DEPTH | FTW_PHYS) < 0)
        fprintf(stderr, "pkgr: clean %s: %s\n", path, strerror(errno));
}

void sync_fd(int fd, const char *path)
{
    if (fsync(fd) != 0)
        syserror("sync", path);
}

bool has_suffix(const char *str, const char *suffix)
{
    size_t length = strlen(str), ending = strlen(suffix);
    return length >= ending && !strcmp(str + length - ending, suffix);
}

static const char *const usage =
    "Usage: pkgr [--root DIR] [--repo DIR] COMMAND [ARGS]\n"
    "\n"
    "Commands:\n"
    "  pack MANIFEST STAGE       Package a staged usr/ tree into the current directory\n"
    "  build RECIPE              Build a local source recipe and package its output\n"
    "  install NAME|FILE         Install a package from a local repository or file\n"
    "  install --build RECIPE    Build and install a local source recipe\n"
    "  upgrade NAME|FILE         Replace an installed package with a newer archive\n"
    "  search [TEXT]             Search the local package repository\n"
    "  remove NAME               Remove an installed package\n"
    "  info NAME|FILE            Show an installed package or local archive\n"
    "  list                      List installed packages\n"
    "\n"
    "Options:\n"
    "  -r, --root DIR            Installation root (default /)\n"
    "  -R, --repo DIR            Local repository (overrides PKGR_REPO and\n"
    "                            <root>/usr/share/pkgr/repo)\n"
    "  -h, --help                Show help\n"
    "  -v, --version             Show the version\n";

static int usage_error(const char *message)
{
    fprintf(stderr, "pkgr: %s\nTry 'pkgr --help'.\n", message);
    return 2;
}

static char *repository_path(const char *root, const char *configured)
{
    if (configured && *configured)
        return copy_string(configured);
    return path_join(root, DEFAULT_REPO);
}

int main(int argc, char **argv)
{
    static const struct option options[] = {
        {"root", required_argument, NULL, 'r'},
        {"repo", required_argument, NULL, 'R'},
        {"help", no_argument, NULL, 'h'},
        {"version", no_argument, NULL, 'v'},
        {0, 0, NULL, 0},
    };
    const char *root = "/";
    const char *repo = getenv("PKGR_REPO");
    int opt;
    while ((opt = getopt_long(argc, argv, "+r:R:hv", options, NULL)) != -1) {
        switch (opt) {
        case 'r':
            root = optarg;
            break;
        case 'R':
            if (!*optarg)
                return usage_error("repository path cannot be empty");
            repo = optarg;
            break;
        case 'h':
            fputs(usage, stdout);
            return 0;
        case 'v':
            puts("pkgr " PKGR_VERSION);
            return 0;
        default:
            return 2;
        }
    }

    if (optind == argc)
        return usage_error("a command is required");
    const char *command = argv[optind++];
    int count = argc - optind;
    char **args = argv + optind;
    if (!strcmp(command, "help") ||
        (count == 1 && (!strcmp(args[0], "-h") || !strcmp(args[0], "--help")))) {
        fputs(usage, stdout);
        return 0;
    }
    if (!strcmp(command, "pack")) {
        if (count != 2)
            return usage_error("pack requires a manifest and stage directory");
        char *result = pkg_pack(args[0], args[1]);
        puts(result);
        free(result);
        return 0;
    }
    if (!strcmp(command, "build")) {
        if (count != 1)
            return usage_error("build requires a recipe directory");
        char *result = pkg_build(args[0]);
        puts(result);
        free(result);
        return 0;
    }
    if (!strcmp(command, "search")) {
        if (count > 1)
            return usage_error("search takes at most one filter");
        char *directory = repository_path(root, repo);
        repo_search(directory, count ? args[0] : "");
        free(directory);
        return 0;
    }
    if (!strcmp(command, "list")) {
        if (count != 0)
            return usage_error("list takes no arguments");
        struct database db = db_open(root, false);
        db_list(db);
        db_close(db);
        return 0;
    }

    bool install = !strcmp(command, "install");
    bool upgrade = !strcmp(command, "upgrade");
    bool remove = !strcmp(command, "remove");
    bool info = !strcmp(command, "info");
    if (!install && !upgrade && !remove && !info)
        return usage_error("unknown command");

    bool build = install && count == 2 && !strcmp(args[0], "--build");
    if (!build && count != 1)
        return usage_error("expected one package name or file");
    if (info && (strchr(args[0], '/') || has_suffix(args[0], PACKAGE_SUFFIX))) {
        struct package pkg = {0};
        pkg_read(args[0], &pkg, -1, true);
        pkg_print(&pkg);
        pkg_free(&pkg);
        return 0;
    }

    char *artifact = build ? pkg_build(args[1]) : NULL;
    const char *expected = NULL;
    if (!artifact && (install || upgrade) && valid_token(args[0]) &&
        !has_suffix(args[0], PACKAGE_SUFFIX)) {
        expected = args[0];
        char *directory = repository_path(root, repo);
        artifact = repo_find(directory, expected);
        free(directory);
    }
    struct database db = db_open(root, install);
    if (install || upgrade)
        db_install(db, artifact ? artifact : args[0], expected, upgrade);
    else if (remove)
        db_remove(db, args[0]);
    else
        db_info(db, args[0]);
    db_close(db);
    free(artifact);
    return 0;
}
