#include "pkgr.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define INDEX_NAME "index.tsv"
#define INDEX_SIGNATURE "index.tsv.sig"
#define CACHE_NAME "repository.index"
#define CACHE_STAGE ".repository.index.new"
#define INDEX_LIMIT (8 * 1024 * 1024)

bool repo_is_remote(const char *url)
{
    return strstr(url, "://") != NULL;
}

static void check_url(const char *url)
{
    if (strncmp(url, "https://", 8) || !url[8] || strlen(url) >= PATH_MAX - 2 || strpbrk(url, "?#\r\n\t "))
        fail("remote repository requires an HTTPS base URL");
}

static void fetch(const char *url, const char *name, const char *path)
{
    char *address = path_join(url, name);
    run_tool((char *[]){"curl", "-fsSL", "--proto", "=https", "--max-time", "120",
                        "-o", (char *)path, "--", address, NULL},
             (char *[]){"wget", "-q", "-T", "30", "-O", (char *)path, "--", address, NULL},
             STDERR_FILENO);
    free(address);
}

static bool valid_digest(const char *value)
{
    return strlen(value) == 64 && strspn(value, "0123456789abcdef") == 64;
}

void file_digest(const char *path, char digest[65])
{
    FILE *output = tmpfile();
    if (!output)
        syserror("create", "digest output");
    run_tool((char *[]){"openssl", "dgst", "-sha256", "-r", "--", (char *)path, NULL},
             NULL, fileno(output));
    rewind(output);
    if (fread(digest, 1, 64, output) != 64 || fgetc(output) != ' ')
        fail("could not read SHA-256 digest for %s", path);
    digest[64] = '\0';
    fclose(output);
    if (!valid_digest(digest))
        fail("invalid SHA-256 digest for %s", path);
}

bool repo_filename(const char *name)
{
    return isalnum((unsigned char)*name) && strlen(name) <= NAME_MAX &&
           strspn(name, "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._+-") == strlen(name) &&
           has_suffix(name, PACKAGE_SUFFIX);
}

static char *key_path(const char *root)
{
    const char *key = getenv("PKGR_REPO_KEY");
    return key && *key ? copy_string(key) : path_join(root, "etc/pkgr/repo.pem");
}

struct index_entry {
    char *name;
    char *version;
    char *arch;
    char *digest;
    char *filename;
    char *description;
};

static bool read_entry(FILE *stream, char line[PATH_MAX], struct index_entry *entry)
{
    char *cursor = line;
    if (!fgets(line, PATH_MAX, stream)) {
        if (ferror(stream))
            syserror("read", "repository index");
        return false;
    }
    char *end = strchr(line, '\n');
    if (!end)
        fail("invalid repository index line");
    *end = '\0';
    char **fields[] = {&entry->name, &entry->version, &entry->arch,
                       &entry->digest, &entry->filename};
    for (size_t i = 0; i < sizeof(fields) / sizeof(*fields); ++i) {
        *fields[i] = strsep(&cursor, "\t");
        if (!cursor)
            fail("missing repository index field");
    }
    entry->description = cursor;
    if (!valid_token(entry->name) || !valid_token(entry->version) ||
        !valid_token(entry->arch) || !valid_digest(entry->digest) ||
        !repo_filename(entry->filename) || strpbrk(cursor, "\t\r\033"))
        fail("invalid repository index entry");
    return true;
}

static void check_header(FILE *stream)
{
    char line[sizeof(INDEX_HEADER)];
    if (!fgets(line, sizeof(line), stream) || strcmp(line, INDEX_HEADER))
        fail("unsupported repository index format");
}

void repo_sync(const char *root, const char *url)
{
    check_url(url);
    const char *work = temp_directory();
    char *index = path_join(work, INDEX_NAME);
    char *signature = path_join(work, INDEX_SIGNATURE);
    char *key = key_path(root);
    char key_digest[65];
    if (access(key, R_OK) != 0)
        syserror("read repository public key", key);
    fetch(url, INDEX_NAME, index);
    fetch(url, INDEX_SIGNATURE, signature);
    struct stat status;
    if (stat(index, &status) != 0)
        syserror("stat", index);
    if (status.st_size > INDEX_LIMIT)
        fail("repository index exceeds %d bytes", INDEX_LIMIT);
    file_digest(key, key_digest);
    run_tool((char *[]){"openssl", "dgst", "-sha256", "-verify", key, "-signature",
                        signature, index, NULL}, NULL, STDERR_FILENO);
    FILE *source = fopen(index, "r");
    if (!source)
        syserror("open", index);
    check_header(source);
    char line[PATH_MAX];
    struct index_entry entry;
    while (read_entry(source, line, &entry))
        ;
    rewind(source);

    struct database db = db_open(root, true);
    if (unlinkat(db.dir, CACHE_STAGE, 0) != 0 && errno != ENOENT)
        syserror("remove", CACHE_STAGE);
    int fd = openat(db.dir, CACHE_STAGE, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        syserror("create", CACHE_STAGE);
    FILE *destination = fdopen(fd, "w");
    if (!destination)
        syserror("open", CACHE_STAGE);
    if (fprintf(destination, "%s\n%s\n", url, key_digest) < 0)
        syserror("write", CACHE_STAGE);
    size_t count;
    while ((count = fread(line, 1, sizeof(line), source)) != 0)
        if (fwrite(line, 1, count, destination) != count)
            syserror("write", CACHE_STAGE);
    if (ferror(source))
        syserror("read", index);
    if (fflush(destination) != 0)
        syserror("write", CACHE_STAGE);
    sync_fd(fd, CACHE_STAGE);
    if (fclose(destination) != 0 || renameat(db.dir, CACHE_STAGE, db.dir, CACHE_NAME) != 0)
        syserror("publish", CACHE_NAME);
    sync_fd(db.dir, CACHE_NAME);
    db_close(db);
    fclose(source);
    free(key);
    free(signature);
    free(index);
    printf("Synced %s\n", url);
}

static FILE *open_index(struct database db, const char *root, const char *url)
{
    check_url(url);
    int fd = db.dir < 0 ? -1 : openat(db.dir, CACHE_NAME, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (db.dir < 0 || (fd < 0 && errno == ENOENT))
        fail("no cached index for %s; run pkgr sync", url);
    if (fd < 0)
        syserror("open", CACHE_NAME);

    FILE *index = fdopen(fd, "r");
    if (!index)
        syserror("open", CACHE_NAME);
    char *key = key_path(root);
    char actual_key[65], cached_key[66];
    file_digest(key, actual_key);
    free(key);
    char line[PATH_MAX];
    if (!fgets(line, sizeof(line), index) || strlen(line) != strlen(url) + 1 ||
        strncmp(line, url, strlen(url)) || line[strlen(url)] != '\n' ||
        !fgets(cached_key, sizeof(cached_key), index) || strlen(cached_key) != 65 || cached_key[64] != '\n' ||
        strncmp(cached_key, actual_key, 64))
        fail("cached index has a different URL or signing key; run pkgr sync");
    check_header(index);
    return index;
}

static bool matching_arch(const struct index_entry *entry, const char *arch)
{
    return !strcmp(entry->arch, arch) || !strcmp(entry->arch, "noarch");
}

void remote_search(const char *root, const char *url, const char *filter)
{
    struct database db = db_open(root, false);
    FILE *index = open_index(db, root, url);
    db_close(db);
    struct utsname system;
    if (uname(&system) != 0)
        syserror("read", "machine architecture");
    char line[PATH_MAX];
    struct index_entry entry;
    while (read_entry(index, line, &entry))
        if (matching_arch(&entry, system.machine) &&
            (strstr(entry.name, filter) || strstr(entry.description, filter))) {
            printf("%s %s", entry.name, entry.version);
            if (*entry.description)
                printf("  %s", entry.description);
            putchar('\n');
        }
    fclose(index);
}

char *remote_find(struct database db, const char *root, const char *url,
                  struct package_requirement requirement, const char *newer_than)
{
    const char *name = requirement.name;
    FILE *index = open_index(db, root, url);
    struct utsname system;
    if (uname(&system) != 0)
        syserror("read", "machine architecture");
    char line[PATH_MAX];
    struct index_entry entry;
    char *filename = NULL, *version = NULL, *arch = NULL;
    char expected_digest[65];
    bool ambiguous = false;
    while (read_entry(index, line, &entry)) {
        if (strcmp(entry.name, name) || !matching_arch(&entry, system.machine) ||
            !pkg_satisfies(requirement, entry.version) ||
            (newer_than && strverscmp(entry.version, newer_than) <= 0))
            continue;
        int order = version ? strverscmp(entry.version, version) : 1;
        if (!order)
            ambiguous = true;
        if (order <= 0)
            continue;
        ambiguous = false;
        free(filename);
        free(version);
        free(arch);
        filename = copy_string(entry.filename);
        version = copy_string(entry.version);
        arch = copy_string(entry.arch);
        strcpy(expected_digest, entry.digest);
    }
    fclose(index);
    if (ambiguous)
        fail("ambiguous repository version: %s %s", name, version);
    if (!filename) {
        if (newer_than)
            return NULL;
        fail("package %s is unavailable for %s in %s", name, system.machine, url);
    }

    char *artifact = path_join(temp_directory(), filename);
    fetch(url, filename, artifact);
    char digest[65];
    file_digest(artifact, digest);
    if (strcmp(digest, expected_digest))
        fail("SHA-256 mismatch for %s", filename);
    struct package pkg = {0};
    pkg_read(artifact, &pkg, -1, true);
    if (strcmp(pkg.name, name) || strcmp(pkg.version, version) || strcmp(pkg.arch, arch))
        fail("package metadata differs from the signed index: %s", filename);
    pkg_free(&pkg);
    free(arch);
    free(version);
    free(filename);
    return artifact;
}

char *pkgr_release(const char *newer_than)
{
    const char *url = getenv("PKGR_RELEASE_URL");
    if (!url || !*url)
        url = "https://github.com/lunarixos/pkgr/releases/latest/download";
    check_url(url);

    const char *filename = "pkgr-update.pkgr.tar.gz";
    char *address = path_join(url, filename);
    char *artifact = path_join(temp_directory(), filename);
    run_tool((char *[]){"curl", "-q", "-fsSL", "--proto", "=https",
                        "--proto-redir", "=https", "--max-time", "120",
                        "-o", artifact, "--", address, NULL}, NULL, STDERR_FILENO);
    free(address);

    struct package pkg = {0};
    pkg_read(artifact, &pkg, -1, true);
    if (strcmp(pkg.name, "pkgr"))
        fail("the pkgr release contains %s", pkg.name);
    struct utsname system;
    if (uname(&system) != 0)
        syserror("read", "machine architecture");
    if (strcmp(pkg.arch, system.machine))
        fail("the pkgr release is for %s, this system is %s", pkg.arch, system.machine);
    bool newer = strverscmp(pkg.version, newer_than) > 0;
    pkg_free(&pkg);
    if (newer)
        return artifact;
    free(artifact);
    return NULL;
}
