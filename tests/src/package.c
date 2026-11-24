#include "pkgr.h"

#include <archive.h>
#include <archive_entry.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <fts.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#define META_LIMIT 65536

static char *build_directory;
static char *temporary_archive;

static void clean_build(void)
{
    if (temporary_archive)
        unlink(temporary_archive);
    remove_tree(build_directory);
}

static void register_cleanup(void)
{
    static bool registered;
    if (!registered && atexit(clean_build) != 0)
        fail("cannot register temporary file cleanup");
    registered = true;
}

bool valid_token(const char *text)
{
    if (!text || !isalnum((unsigned char)text[0]) || strlen(text) > 120)
        return false;
    for (const unsigned char *c = (const unsigned char *)text; *c; ++c)
        if (!isalnum(*c) && !strchr("._+-", *c))
            return false;
    return true;
}

static bool valid_path(const char *path)
{
    if (strlen(path) >= PATH_MAX)
        return false;
    if (strncmp(path, "usr/", 4) && strncmp(path, "etc/", 4) &&
        strncmp(path, "bin/", 4) && strncmp(path, "sbin/", 5) && strncmp(path, "lib/", 4))
        return false;
    for (const char *part = path; ; ) {
        size_t length = strcspn(part, "/");
        if (length == 0 || length > NAME_MAX ||
            (length == 1 && !memcmp(part, ".", 1)) ||
            (length == 2 && !memcmp(part, "..", 2)))
            return false;
        for (size_t i = 0; i < length; ++i)
            if ((unsigned char)part[i] < 32 || (unsigned char)part[i] == 127 ||
                part[i] == '\\')
                return false;
        if (!part[length])
            return true;
        part += length + 1;
    }
}

static FILE *checked_stream(int fd, const char *mode)
{
    FILE *stream = fdopen(fd, mode);
    if (!stream)
        syserror("open", "manifest");
    return stream;
}

static void set_field(char **field, const char *value, const char *name)
{
    if (*field)
        fail("duplicate manifest field '%s'", name);
    *field = copy_string(value);
}

static void add_file(struct package *pkg, const char *path)
{
    if (!valid_path(path))
        fail("invalid package path: %s", path);
    size_t count = pkg->file_count;
    if (count >= 65536)
        fail("too many files in package");
    if ((count & (count - 1)) == 0)
        pkg->files = grow_array(pkg->files, count ? count * 2 : 1, sizeof(*pkg->files));
    pkg->files[count] = (struct package_file){copy_string(path), count};
    pkg->file_count++;
}

static int compare_files(const void *left, const void *right)
{
    const struct package_file *a = left, *b = right;
    return strcmp(a->path, b->path);
}

static void sort_files(struct package *pkg)
{
    if (pkg->file_count > 1)
        qsort(pkg->files, pkg->file_count, sizeof(*pkg->files), compare_files);
    for (size_t i = 1; i < pkg->file_count; ++i)
        if (!strcmp(pkg->files[i - 1].path, pkg->files[i].path))
            fail("duplicate package path: %s", pkg->files[i].path);
}

const struct package_file *pkg_file(const struct package *pkg, const char *path)
{
    struct package_file key = {(char *)path, 0};
    return pkg->file_count ?
           bsearch(&key, pkg->files, pkg->file_count, sizeof(*pkg->files), compare_files) : NULL;
}

bool pkg_has_file(const struct package *pkg, const char *path)
{
    return pkg_file(pkg, path) != NULL;
}

struct package_requirement pkg_requirement(const char *expression)
{
    struct package_requirement requirement = {0};
    size_t length = strcspn(expression, "<>=");
    if (length >= sizeof(requirement.name))
        fail("invalid dependency: %s", expression);
    memcpy(requirement.name, expression, length);
    if (!valid_token(requirement.name))
        fail("invalid dependency: %s", expression);
    const char *op = expression + length;
    if (*op) {
        requirement.minimum = !strncmp(op, ">=", 2);
        if (!requirement.minimum && *op != '=')
            fail("use name, name>=version or name=version for dependency: %s", expression);
        requirement.version = op + (requirement.minimum ? 2 : 1);
        if (!valid_token(requirement.version))
            fail("invalid dependency version: %s", expression);
    }
    return requirement;
}

bool pkg_satisfies(struct package_requirement requirement, const char *version)
{
    if (!version)
        return false;
    if (!requirement.version)
        return true;
    int comparison = strverscmp(version, requirement.version);
    return requirement.minimum ? comparison >= 0 : comparison == 0;
}

struct package_config *pkg_config(const struct package *pkg, const char *path)
{
    for (size_t i = 0; i < pkg->config_count; ++i)
        if (!strcmp(pkg->configs[i].path, path))
            return &pkg->configs[i];
    return NULL;
}

static void add_config(struct package *pkg, const char *path, const char *digest)
{
    if (strncmp(path, "etc/", 4) || !valid_path(path) ||
        pkg_config(pkg, path) || pkg->config_count >= 256)
        fail("invalid configuration path: %s", path);
    pkg->configs = grow_array(pkg->configs, pkg->config_count + 1, sizeof(*pkg->configs));
    struct package_config *config = &pkg->configs[pkg->config_count++];
    *config = (struct package_config){ .path = copy_string(path) };
    if (digest)
        memcpy(config->digest, digest, 64);
}

void pkg_parse(FILE *stream, struct package *pkg, bool record)
{
    char *line = NULL;
    size_t capacity = 0;
    ssize_t length;
    while ((length = getline(&line, &capacity, stream)) != -1) {
        if (length > PATH_MAX || memchr(line, 0, (size_t)length))
            fail("invalid manifest line");
        if (length && line[length - 1] == '\n')
            line[--length] = '\0';
        if (!length || line[0] == '#')
            continue;
        char *value = strchr(line, '=');
        if (!value)
            fail("expected key=value in manifest");
        *value++ = '\0';
        if (!strcmp(line, "name"))
            set_field(&pkg->name, value, line);
        else if (!strcmp(line, "version"))
            set_field(&pkg->version, value, line);
        else if (!strcmp(line, "arch"))
            set_field(&pkg->arch, value, line);
        else if (!strcmp(line, "description"))
            set_field(&pkg->description, value, line);
        else if (!strcmp(line, "essential") && !strcmp(value, "yes") && !pkg->essential)
            pkg->essential = true;
        else if (!strcmp(line, "config"))
            add_config(pkg, value, NULL);
        else if (record && !strcmp(line, "config-digest")) {
            if (strspn(value, "0123456789abcdef") != 64 || value[64] != ' ')
                fail("invalid configuration digest");
            add_config(pkg, value + 65, value);
        } else if (!strcmp(line, "depend")) {
            struct package_requirement requirement = pkg_requirement(value);
            if (pkg->dependency_count >= 256)
                fail("too many dependencies");
            for (size_t i = 0; i < pkg->dependency_count; ++i)
                if (!strcmp(pkg_requirement(pkg->depends[i]).name, requirement.name))
                    fail("duplicate dependency: %s", requirement.name);
            pkg->depends = grow_array(pkg->depends, pkg->dependency_count + 1,
                                      sizeof(*pkg->depends));
            pkg->depends[pkg->dependency_count++] = copy_string(value);
        } else if (record && !strcmp(line, "file"))
            add_file(pkg, value);
        else
            fail("unknown manifest field '%s'", line);
    }
    free(line);
    if (ferror(stream))
        syserror("read", "manifest");
    if (!valid_token(pkg->name) || !valid_token(pkg->version) || !valid_token(pkg->arch))
        fail("manifest requires a valid name, version and arch");
    for (size_t i = 0; i < pkg->dependency_count; ++i)
        if (!strcmp(pkg->name, pkg_requirement(pkg->depends[i]).name))
            fail("package depends on itself: %s", pkg->name);
    sort_files(pkg);
}

void pkg_write(FILE *stream, const struct package *pkg, bool record)
{
    fprintf(stream, "name=%s\nversion=%s\narch=%s\n", pkg->name, pkg->version, pkg->arch);
    if (pkg->description)
        fprintf(stream, "description=%s\n", pkg->description);
    for (size_t i = 0; i < pkg->dependency_count; ++i)
        fprintf(stream, "depend=%s\n", pkg->depends[i]);
    if (pkg->essential)
        fputs("essential=yes\n", stream);
    for (size_t i = 0; i < pkg->config_count; ++i) {
        const struct package_config *config = &pkg->configs[i];
        if (record && *config->digest)
            fprintf(stream, "config-digest=%s %s\n", config->digest, config->path);
        else
            fprintf(stream, "config=%s\n", config->path);
    }
    if (record && pkg->file_count) {
        const char **paths = grow_array(NULL, pkg->file_count, sizeof(*paths));
        for (size_t i = 0; i < pkg->file_count; ++i)
            paths[pkg->files[i].slot] = pkg->files[i].path;
        for (size_t i = 0; i < pkg->file_count; ++i)
            fprintf(stream, "file=%s\n", paths[i]);
        free(paths);
    }
    if (fflush(stream) == EOF)
        syserror("write", "manifest");
}

void pkg_print(const struct package *pkg)
{
    printf("Name:         %s\nVersion:      %s\nArchitecture: %s\n", pkg->name, pkg->version,
           pkg->arch);
    if (pkg->description)
        printf("Description:  %s\n", pkg->description);
    if (pkg->dependency_count) {
        printf("Depends on:  ");
        for (size_t i = 0; i < pkg->dependency_count; ++i)
            printf(" %s", pkg->depends[i]);
        putchar('\n');
    }
    if (pkg->file_count)
        printf("Files:        %zu\n", pkg->file_count);
}

void pkg_free(struct package *pkg)
{
    free(pkg->name);
    free(pkg->version);
    free(pkg->arch);
    free(pkg->description);
    for (size_t i = 0; i < pkg->config_count; ++i)
        free(pkg->configs[i].path);
    free(pkg->configs);
    for (size_t i = 0; i < pkg->dependency_count; ++i)
        free(pkg->depends[i]);
    free(pkg->depends);
    for (size_t i = 0; i < pkg->file_count; ++i)
        free(pkg->files[i].path);
    free(pkg->files);
    *pkg = (struct package){0};
}

static void check_archive(struct archive *archive, int status, const char *what)
{
    if (status != ARCHIVE_OK)
        fail("%s: %s", what, archive_error_string(archive) ? archive_error_string(archive) : "invalid archive");
}

void pkg_slot(char name[32], const char *prefix, size_t slot)
{
    snprintf(name, 32, "%s.%zu", prefix, slot);
}

static void read_header(struct archive *archive, struct package *pkg)
{
    struct archive_entry *entry;
    check_archive(archive, archive_read_next_header(archive, &entry), "read package header");
    la_int64_t size = archive_entry_size(entry);
    const char *filename = archive_entry_pathname(entry);
    if (!filename || strcmp(filename, ".PKGRINFO") ||
        archive_entry_filetype(entry) != AE_IFREG || archive_entry_hardlink(entry) ||
        size <= 0 || size > META_LIMIT)
        fail("package must start with a regular .PKGRINFO manifest");
    char buffer[META_LIMIT];
    size_t count = 0;
    while (count < (size_t)size) {
        la_ssize_t got = archive_read_data(archive, buffer + count, (size_t)size - count);
        if (got <= 0)
            fail("truncated package manifest");
        count += (size_t)got;
    }
    FILE *stream = fmemopen(buffer, count, "r");
    if (!stream)
        syserror("open", "package manifest");
    pkg_parse(stream, pkg, false);
    fclose(stream);
}

static bool permitted_mode(const struct package *pkg, const char *path, mode_t mode)
{
    mode &= 07777;
    if (!(mode & 07000))
        return true;
    return mode == 04755 &&
           ((!strcmp(pkg->name, "opendoas") && !strcmp(path, "usr/bin/doas")) ||
            (!strcmp(pkg->name, "sudo") && !strcmp(path, "usr/bin/sudo")));
}

void pkg_read(const char *path, struct package *pkg, int stage, bool metadata_only)
{
    struct archive *archive = archive_read_new();
    if (!archive)
        fail("allocate archive reader");
    check_archive(archive, archive_read_support_filter_gzip(archive), "enable gzip");
    check_archive(archive, archive_read_support_format_tar(archive), "enable tar");
    int input = open(path, O_RDONLY | O_CLOEXEC | O_NONBLOCK);
    struct stat status;
    if (input < 0 || fstat(input, &status) < 0)
        syserror("open", path);
    if (!S_ISREG(status.st_mode))
        fail("not a regular package file: %s", path);
    check_archive(archive, archive_read_open_fd(archive, input, 32768), path);
    read_header(archive, pkg);
    if (!metadata_only) {
        struct archive_entry *entry;
        int status;
        while ((status = archive_read_next_header(archive, &entry)) == ARCHIVE_OK) {
            const char *filename = archive_entry_pathname(entry);
            mode_t mode = archive_entry_perm(entry);
            if (!filename)
                fail("archive entry has no path");
            mode_t type = archive_entry_filetype(entry);
            if ((type != AE_IFREG && type != AE_IFLNK) || archive_entry_hardlink(entry) ||
                (type != AE_IFLNK && archive_entry_symlink(entry)) ||
                !permitted_mode(pkg, filename, mode) || archive_entry_size(entry) < 0 ||
                archive_entry_sparse_count(entry) != 0)
                fail("unsupported archive entry: %s", filename);
            add_file(pkg, filename);
            char name[32];
            pkg_slot(name, "new", pkg->file_count - 1);
            if (type == AE_IFLNK) {
                const char *target = archive_entry_symlink(entry);
                if (!target || !*target || strlen(target) >= PATH_MAX || pkg_config(pkg, filename))
                    fail("invalid symbolic link: %s", filename);
                if (symlinkat(target, stage, name) != 0)
                    syserror("stage symbolic link", filename);
                continue;
            }
            int fd = openat(stage, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (fd < 0)
                syserror("stage", filename);
            check_archive(archive, archive_read_data_into_fd(archive, fd), filename);
            struct stat st;
            if (fstat(fd, &st) < 0)
                syserror("stat", filename);
            if (st.st_size != archive_entry_size(entry))
                fail("truncated archive entry: %s", filename);
            if (fchmod(fd, mode) != 0)
                syserror("set mode", filename);
            sync_fd(fd, filename);
            close(fd);
        }
        if (status != ARCHIVE_EOF)
            check_archive(archive, status, "read package");
        if (!pkg->file_count)
            fail("package has no files");
        sort_files(pkg);
        for (size_t i = 0; i < pkg->config_count; ++i)
            if (!pkg_has_file(pkg, pkg->configs[i].path))
                fail("configuration file missing from archive: %s", pkg->configs[i].path);
    }
    check_archive(archive, archive_read_close(archive), "close package");
    archive_read_free(archive);
    close(input);
}

static void write_header(struct archive *archive, const char *path, mode_t mode, off_t size)
{
    struct archive_entry *entry = archive_entry_new();
    if (!entry)
        fail("allocate archive entry");
    archive_entry_set_pathname(entry, path);
    archive_entry_set_filetype(entry, AE_IFREG);
    archive_entry_set_perm(entry, mode);
    archive_entry_set_size(entry, size);
    check_archive(archive, archive_write_header(archive, entry), path);
    archive_entry_free(entry);
}

static void archive_bytes(struct archive *archive, const void *data, size_t length)
{
    if (archive_write_data(archive, data, length) != (la_ssize_t)length)
        fail("write package payload: %s", archive_error_string(archive));
}

static void archive_file(struct archive *archive, const struct package *pkg,
                         const char *path, const char *relative)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) < 0)
        syserror("open", path);
    if (!S_ISREG(st.st_mode) || !permitted_mode(pkg, relative, st.st_mode))
        fail("unsupported staged file: %s", relative);
    write_header(archive, relative, st.st_mode & 07777, st.st_size);
    char buffer[32768];
    off_t left = st.st_size;
    while (left > 0) {
        size_t size = left < (off_t)sizeof(buffer) ? (size_t)left : sizeof(buffer);
        ssize_t count = read(fd, buffer, size);
        if (count < 0 && errno == EINTR)
            continue;
        if (count < 0)
            syserror("read", path);
        if (!count)
            fail("staged file changed size: %s", path);
        archive_bytes(archive, buffer, (size_t)count);
        left -= count;
    }
    close(fd);
    check_archive(archive, archive_write_finish_entry(archive), relative);
}

static void archive_link(struct archive *archive, const char *path, const char *relative)
{
    char target[PATH_MAX];
    ssize_t length = readlink(path, target, sizeof(target));
    if (length < 0)
        syserror("read symbolic link", relative);
    if ((size_t)length >= sizeof(target))
        fail("symbolic link target too long: %s", relative);
    target[length] = '\0';
    struct archive_entry *entry = archive_entry_new();
    archive_entry_set_pathname(entry, relative);
    archive_entry_set_filetype(entry, AE_IFLNK);
    archive_entry_set_perm(entry, 0777);
    archive_entry_set_size(entry, 0);
    archive_entry_set_symlink(entry, target);
    check_archive(archive, archive_write_header(archive, entry), relative);
    archive_entry_free(entry);
}

char *pkg_pack(const char *manifest, const char *root)
{
    struct package pkg = {0};
    int manifest_fd = open(manifest, O_RDONLY | O_CLOEXEC);
    if (manifest_fd < 0)
        syserror("open", manifest);
    FILE *stream = checked_stream(manifest_fd, "r");
    pkg_parse(stream, &pkg, false);
    fclose(stream);
    char *filename;
    if (asprintf(&filename, "%s-%s.%s%s", pkg.name, pkg.version, pkg.arch, PACKAGE_SUFFIX) < 0)
        fail("out of memory");
    register_cleanup();
    temporary_archive = copy_string(".pkgr-package.XXXXXX");
    int fd = mkstemp(temporary_archive);
    if (fd < 0)
        syserror("create", temporary_archive);
    struct archive *archive = archive_write_new();
    if (!archive)
        fail("allocate archive writer");
    check_archive(archive, archive_write_add_filter_gzip(archive), "enable gzip");
    check_archive(archive, archive_write_set_format_pax_restricted(archive), "enable pax");
    check_archive(archive, archive_write_open_fd(archive, fd), "open output archive");

    char *meta = NULL;
    size_t size = 0;
    stream = open_memstream(&meta, &size);
    if (!stream)
        syserror("open", "manifest");
    pkg_write(stream, &pkg, false);
    if (fclose(stream) != 0)
        syserror("close", "manifest");
    if (size > META_LIMIT)
        fail("manifest is too large");
    write_header(archive, ".PKGRINFO", 0644, (off_t)size);
    archive_bytes(archive, meta, size);
    free(meta);

    char *canonical = realpath(root, NULL);
    if (!canonical)
        syserror("resolve", root);
    char *paths[] = {canonical, NULL};
    FTS *tree = fts_open(paths, FTS_PHYSICAL | FTS_NOCHDIR, NULL);
    if (!tree)
        syserror("open stage", root);
    size_t prefix = strlen(canonical) + 1;
    for (;;) {
        errno = 0;
        FTSENT *entry = fts_read(tree);
        if (!entry) {
            if (errno)
                syserror("read stage", root);
            break;
        }
        if (entry->fts_info == FTS_ERR || entry->fts_info == FTS_DNR ||
            entry->fts_info == FTS_NS) {
            errno = entry->fts_errno;
            syserror("read stage", entry->fts_path);
        }
        if (entry->fts_info == FTS_D) {
            const char *relative = entry->fts_level ? entry->fts_path + prefix : "";
            if (entry->fts_level &&
                strcmp(relative, "usr") && strcmp(relative, "etc") &&
                strcmp(relative, "bin") && strcmp(relative, "sbin") &&
                strcmp(relative, "lib") && !valid_path(relative))
                fail("invalid stage directory: %s", entry->fts_path);
            continue;
        }
        if (entry->fts_info == FTS_DP)
            continue;
        const char *relative = entry->fts_path + prefix;
        add_file(&pkg, relative);
        if (entry->fts_info == FTS_F)
            archive_file(archive, &pkg, entry->fts_accpath, relative);
        else if ((entry->fts_info == FTS_SL || entry->fts_info == FTS_SLNONE) &&
                 !pkg_config(&pkg, relative))
            archive_link(archive, entry->fts_accpath, relative);
        else
            fail("unsupported stage entry: %s", entry->fts_path);
    }
    fts_close(tree);
    free(canonical);
    if (!pkg.file_count)
        fail("stage has no package files");
    sort_files(&pkg);
    for (size_t i = 0; i < pkg.config_count; ++i)
        if (!pkg_has_file(&pkg, pkg.configs[i].path))
            fail("configuration file missing from stage: %s", pkg.configs[i].path);
    check_archive(archive, archive_write_close(archive), "finish package");
    archive_write_free(archive);
    if (fchmod(fd, 0644) != 0)
        syserror("set mode", temporary_archive);
    sync_fd(fd, temporary_archive);
    close(fd);
    if (link(temporary_archive, filename) != 0)
        syserror("publish", filename);
    if (unlink(temporary_archive) != 0)
        syserror("unlink", temporary_archive);
    free(temporary_archive);
    temporary_archive = NULL;
    pkg_free(&pkg);
    return filename;
}

char *pkg_build(const char *recipe)
{
    if (geteuid() == 0)
        fail("run source builds as a regular user, then install the resulting archive");
    char *directory = realpath(recipe, NULL);
    if (!directory && errno == ENOENT && valid_token(recipe)) {
        const char *recipes = getenv("PKGR_RECIPES");
        char *path = path_join(recipes && *recipes ? recipes : "packages", recipe);
        directory = realpath(path, NULL);
        if (!directory && errno == ENOENT && (!recipes || !*recipes)) {
            free(path);
            path = path_join("/usr/share/pkgr/recipes", recipe);
            directory = realpath(path, NULL);
        }
        free(path);
    }
    if (!directory)
        syserror("resolve", recipe);
    char *manifest = path_join(directory, "package.info");
    char *script = path_join(directory, "build.sh");
    if (access(manifest, R_OK) || access(script, R_OK))
        syserror("read recipe", recipe);

    register_cleanup();
    build_directory = copy_string("/tmp/pkgr-build.XXXXXX");
    if (!mkdtemp(build_directory))
        syserror("create", build_directory);
    char *stage = path_join(build_directory, "stage");
    if (mkdir(stage, 0755) != 0)
        syserror("create", stage);
    pid_t child = fork();
    if (child < 0)
        syserror("fork", script);
    if (child == 0) {
        if (dup2(STDERR_FILENO, STDOUT_FILENO) >= 0 && chdir(directory) == 0)
            execl("/bin/sh", "sh", script, stage, build_directory, (char *)NULL);
        dprintf(STDERR_FILENO, "pkgr: run %s: %s\n", script, strerror(errno));
        _exit(127);
    }
    int status;
    while (waitpid(child, &status, 0) < 0)
        if (errno != EINTR)
            syserror("wait for", script);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        fail("build failed: %s", script);
    char *result = pkg_pack(manifest, stage);
    free(stage);
    free(script);
    free(manifest);
    free(directory);
    return result;
}
