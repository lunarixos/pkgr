#include "pkgr.h"

#include <errno.h>
#include <fcntl.h>
#include <ftw.h>
#include <getopt.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
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

static char *command_temp;

static void cleanup_command(void)
{
    remove_tree(command_temp);
}

const char *temp_directory(void)
{
    if (!command_temp) {
        command_temp = copy_string("/tmp/pkgr-command.XXXXXX");
        if (!mkdtemp(command_temp))
            syserror("create", command_temp);
        if (atexit(cleanup_command) != 0)
            fail("cannot register temporary directory cleanup");
    }
    return command_temp;
}

void run_tool(char *const args[], char *const fallback[], int output)
{
    pid_t child = fork();
    if (child < 0)
        syserror("fork", args[0]);
    if (child == 0) {
        if (dup2(output, STDOUT_FILENO) >= 0) {
            execvp(args[0], args);
            if (errno == ENOENT && fallback)
                execvp(fallback[0], fallback);
        }
        dprintf(STDERR_FILENO, "pkgr: run %s: %s\n", args[0], strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            syserror("wait for", args[0]);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("%s failed", args[0]);
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
    "Usage: pkgr [--root DIR] [--repo LOCATION] COMMAND [ARGS]\n"
    "\n"
    "Commands:\n"
    "  pack MANIFEST STAGE       Package a staged filesystem into the current directory\n"
    "  build RECIPE              Build and package a local source recipe\n"
    "  build URL [DIR [REF]]     Build a recipe from a Git checkout\n"
    "  install NAME|FILE         Install a package from a repository or file\n"
    "  install --build SOURCE    Build and install; Git sources accept [DIR [REF]]\n"
    "  index DIRECTORY           Write a binary repository index to stdout\n"
    "  sync                      Refresh the signed remote repository index\n"
    "  upgrade NAME|FILE         Upgrade one installed package\n"
    "  upgrade --all             Upgrade installed packages from the selected repository\n"
    "  upgrade pkgr              Upgrade pkgr from its official GitHub release\n"
    "  search [TEXT]             Search the selected package repository\n"
    "  remove NAME               Remove an installed package\n"
    "  info NAME|FILE            Show an installed package or local archive\n"
    "  list                      List installed packages\n"
    "  adopt FILE                Register matching files from a trusted archive\n"
    "  recover                   Recover an interrupted package transaction\n"
    "\n"
    "Options:\n"
    "  -r, --root DIR            Installation root (default /)\n"
    "  -R, --repo LOCATION       Local directory or HTTPS repository (overrides PKGR_REPO and\n"
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

static char *configured_repository(const char *root)
{
    char *path = path_join(root, "etc/pkgr/repository");
    FILE *stream = fopen(path, "r");
    if (!stream && errno != ENOENT)
        syserror("open repository configuration", path);
    free(path);
    if (!stream)
        return NULL;
    char *url = NULL;
    size_t capacity = 0;
    ssize_t length = getline(&url, &capacity, stream);
    if (ferror(stream))
        syserror("read", "repository configuration");
    fclose(stream);
    if (length > 0 && url[length - 1] == '\n')
        url[--length] = '\0';
    if (length <= 0 || memchr(url, 0, (size_t)length) || strpbrk(url, "\r\t "))
        fail("invalid repository configuration");
    return url;
}

static void deploy_boot(const char *root)
{
    if (strcmp(root, "/") || access("/etc/lunarix/boot.conf", F_OK) != 0)
        return;
    run_tool((char *[]) { "/usr/sbin/lunarix-boot-update", NULL }, NULL, STDERR_FILENO);
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
    bool explicit_repo = false;
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
            explicit_repo = true;
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
        if (count < 1 || count > 3)
            return usage_error("build requires RECIPE or URL [DIR [REF]]");
        char *result = build_source(count, args);
        puts(result);
        free(result);
        return 0;
    }
    if (!strcmp(command, "index")) {
        if (count != 1)
            return usage_error("index requires a repository directory");
        repo_index(args[0]);
        return 0;
    }
    if (!strcmp(command, "recover")) {
        if (count)
            return usage_error("recover takes no arguments");
        struct database db = db_open(root, false);
        db_close(db);
        return 0;
    }
    if (!strcmp(command, "adopt")) {
        if (count != 1)
            return usage_error("adopt takes one archive");
        struct database db = db_open(root, true);
        db_adopt(db, args[0]);
        db_close(db);
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


    char *configured = repo && *repo ? NULL : configured_repository(root);
    if (configured)
        repo = configured;
    if (!strcmp(command, "sync")) {
        if (count || !repo || !repo_is_remote(repo))
            return usage_error("sync requires an HTTPS repository set by --repo or PKGR_REPO");
        repo_sync(root, repo);
        free(configured);
        return 0;
    }
    if (!strcmp(command, "search")) {
        if (count > 1)
            return usage_error("search takes at most one filter");
        char *directory = repository_path(root, repo);
        if (repo_is_remote(directory))
            remote_search(root, directory, count ? args[0] : "");
        else
            repo_search(directory, count ? args[0] : "");
        free(directory);
        free(configured);
        return 0;
    }

    bool install = !strcmp(command, "install");
    bool upgrade = !strcmp(command, "upgrade");
    bool remove = !strcmp(command, "remove");
    bool info = !strcmp(command, "info");
    if (!install && !upgrade && !remove && !info)
        return usage_error("unknown command");

    bool build = install && count && !strcmp(args[0], "--build");
    if (build && (count < 2 || count > 4))
        return usage_error("install --build requires RECIPE or URL [DIR [REF]]");
    if (!build && count != 1)
        return usage_error("expected one package name or file");
    if (info && (strchr(args[0], '/') || has_suffix(args[0], PACKAGE_SUFFIX))) {
        struct package pkg = {0};
        pkg_read(args[0], &pkg, -1, true);
        pkg_print(&pkg);
        pkg_free(&pkg);
        free(configured);
        return 0;
    }

    char *artifact = build ? build_source(count - 1, args + 1) : NULL;
    if (install || upgrade) {
        const char *target = artifact ? artifact : args[0];
        bool all = upgrade && !strcmp(target, "--all");
        bool by_name = !artifact && valid_token(target) && !has_suffix(target, PACKAGE_SUFFIX);
        bool from_release = upgrade && !strcmp(target, "pkgr") && !explicit_repo;
        char *directory = repository_path(root, repo);
        if (upgrade && (all || by_name) && !from_release && repo_is_remote(directory))
            repo_sync(root, directory);
        struct database db = db_open(root, install);
        if (upgrade && (all || by_name))
            upgrade_packages(db, root, directory, all ? NULL : target, from_release);
        else
            install_package(db, root, directory, by_name ? target : NULL,
                            by_name ? NULL : target, upgrade);
        if (upgrade)
            deploy_boot(root);
        db_close(db);
        free(directory);
    } else {
        struct database db = db_open(root, false);
        if (remove)
            db_remove(db, args[0]);
        else
            db_info(db, args[0]);
        db_close(db);
    }
    free(artifact);
    free(configured);
    return 0;
}
