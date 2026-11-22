#include "pkgr.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define RECORD_SUFFIX ".record"
#define PREPARE ".prepare"
#define PENDING ".pending"
#define DONE ".done"

static int pkg_parent(int root, const char *path, bool create, char name[NAME_MAX + 1])
{
    int fd = fcntl(root, F_DUPFD_CLOEXEC, 3);
    if (fd < 0)
        syserror("duplicate root for", path);
    const char *part = path;
    const char *slash;
    while ((slash = strchr(part, '/'))) {
        size_t length = (size_t)(slash - part);
        if (!length || length > NAME_MAX)
            fail("invalid directory in path: %s", path);
        memcpy(name, part, length);
        name[length] = '\0';
        int next = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        if (next < 0 && errno == ENOENT && create) {
            if (mkdirat(fd, name, 0755) == 0)
                sync_fd(fd, path);
            else if (errno != EEXIST)
                syserror("create directory for", path);
            next = openat(fd, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        }
        int saved = errno;
        close(fd);
        if (next < 0) {
            errno = saved;
            if (errno == ENOENT && !create)
                return -1;
            syserror("open parent of", path);
        }
        fd = next;
        part = slash + 1;
    }
    if (!*part || strlen(part) > NAME_MAX)
        fail("invalid file name: %s", path);
    strcpy(name, part);
    return fd;
}

static DIR *entries_at(int parent)
{
    int fd = openat(parent, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0)
        syserror("open", "package database directory");
    DIR *dir = fdopendir(fd);
    if (!dir)
        syserror("scan", "package database directory");
    return dir;
}

static struct dirent *next_entry(DIR *directory)
{
    errno = 0;
    struct dirent *entry = readdir(directory);
    if (!entry && errno)
        syserror("scan", "package database directory");
    return entry;
}

static int open_work(int db, const char *name)
{
    int fd = openat(db, name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0 && errno != ENOENT)
        syserror("open transaction directory", name);
    return fd;
}

static void clear_work(int db, const char *name)
{
    int fd = open_work(db, name);
    if (fd < 0)
        return;
    DIR *dir = entries_at(fd);
    for (struct dirent *entry; (entry = next_entry(dir)); ) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        if (unlinkat(fd, entry->d_name, 0) != 0)
            syserror("clean transaction file", entry->d_name);
    }
    closedir(dir);
    close(fd);
    if (unlinkat(db, name, AT_REMOVEDIR) != 0)
        syserror("clean transaction directory", name);
    sync_fd(db, name);
}

static void record_name(const char *name, char filename[NAME_MAX + 1])
{
    if (!valid_token(name))
        fail("invalid package name: %s", name);
    snprintf(filename, NAME_MAX + 1, "%s%s", name, RECORD_SUFFIX);
}

static bool read_record(int dir, const char *name, struct package *pkg)
{
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        if (errno == ENOENT)
            return false;
        syserror("open package record", name);
    }
    struct stat st;
    if (fstat(fd, &st) < 0)
        syserror("stat package record", name);
    if (!S_ISREG(st.st_mode))
        fail("package record is not a regular file: %s", name);
    FILE *stream = fdopen(fd, "r");
    if (!stream)
        syserror("read package record", name);
    pkg_parse(stream, pkg, true);
    fclose(stream);
    if (!pkg->file_count)
        fail("empty package record: %s", name);
    return true;
}

static void write_record(int dir, const char *name, const struct package *pkg)
{
    int fd = openat(dir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);
    if (fd < 0)
        syserror("create package record", name);
    FILE *stream = fdopen(fd, "w");
    if (!stream)
        syserror("write package record", name);
    pkg_write(stream, pkg, true);
    sync_fd(fd, name);
    if (fclose(stream) != 0)
        syserror("close package record", name);
}

static bool path_status(int dir, const char *name, struct stat *st)
{
    if (fstatat(dir, name, st, AT_SYMLINK_NOFOLLOW) == 0)
        return true;
    if (errno == ENOENT)
        return false;
    syserror("inspect", name);
}

static bool same_file(const struct stat *left, const struct stat *right)
{
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static bool installed(int db, const char *name, struct package *pkg)
{
    char file[NAME_MAX + 1];
    record_name(name, file);
    if (!read_record(db, file, pkg))
        return false;
    if (strcmp(pkg->name, name))
        fail("package name does not match record: %s", file);
    return true;
}

static void replace_record(int db, int work, const char *name, const struct package *pkg)
{
    char file[NAME_MAX + 1];
    record_name(name, file);
    if (pkg->name) {
        if (unlinkat(work, "record", 0) < 0 && errno != ENOENT)
            syserror("clear staged record", name);
        write_record(work, "record", pkg);
        sync_fd(work, "staged record");
        if (renameat(work, "record", db, file) != 0)
            syserror("publish package record", name);
        sync_fd(work, "staged record");
    } else if (unlinkat(db, file, 0) != 0 && errno != ENOENT)
        syserror("remove package record", name);
    sync_fd(db, name);
}

static void finish_work(int db)
{
    if (renameat(db, PENDING, db, DONE) != 0)
        syserror("finish transaction", PENDING);
    sync_fd(db, DONE);
    clear_work(db, DONE);
}

static void rollback(struct database db, int work)
{
    struct package before = {0}, after = {0};
    read_record(work, "before", &before);
    read_record(work, "after", &after);
    const char *package = before.name ? before.name : after.name;
    if (!package || (before.name && after.name && strcmp(before.name, after.name)))
        fail("invalid pending transaction");
    fprintf(stderr, "pkgr: recovering interrupted transaction for %s\n", package);

    for (size_t i = 0; i < after.file_count; ++i) {
        const struct package_file *file = &after.files[i];
        char name[NAME_MAX + 1], staged[32];
        int parent = pkg_parent(db.root, file->path, false, name);
        if (parent < 0)
            continue;
        pkg_slot(staged, "new", file->slot);
        struct stat current, candidate;
        if (!path_status(work, staged, &candidate))
            fail("missing recovery payload: %s", file->path);
        if (path_status(parent, name, &current) && same_file(&current, &candidate)) {
            if (unlinkat(parent, name, 0) != 0)
                syserror("undo install of", file->path);
            sync_fd(parent, file->path);
        }
        close(parent);
    }
    for (size_t i = 0; i < before.file_count; ++i) {
        const struct package_file *file = &before.files[i];
        char name[NAME_MAX + 1], backup[32];
        pkg_slot(backup, "old", file->slot);
        struct stat current, saved;
        if (!path_status(work, backup, &saved))
            continue;
        int parent = pkg_parent(db.root, file->path, true, name);
        if (path_status(parent, name, &current)) {
            if (!same_file(&current, &saved))
                fail("cannot restore %s because it was changed outside pkgr", file->path);
        } else if (linkat(work, backup, parent, name, 0) != 0) {
            syserror("restore", file->path);
        }
        sync_fd(parent, file->path);
        close(parent);
    }
    replace_record(db.dir, work, package, &before);
    pkg_free(&before);
    pkg_free(&after);
}

struct database db_open(const char *path, bool create)
{
    struct database db = {open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC), -1};
    if (db.root < 0)
        syserror("open installation root", path);
    char name[NAME_MAX + 1];
    db.dir = pkg_parent(db.root, "var/lib/pkgr/unused", create, name);
    if (db.dir < 0)
        return db;
    if (flock(db.dir, LOCK_EX | LOCK_NB) != 0)
        syserror("lock", "package database");
    clear_work(db.dir, DONE);
    clear_work(db.dir, PREPARE);
    int work = open_work(db.dir, PENDING);
    if (work >= 0) {
        rollback(db, work);
        close(work);
        finish_work(db.dir);
    }
    return db;
}

void db_close(struct database db)
{
    if (db.dir >= 0)
        close(db.dir);
    close(db.root);
}

static void check_dependencies(int dir, const struct package *pkg)
{
    for (size_t i = 0; i < pkg->dependency_count; ++i) {
        struct package dependency = {0};
        if (!installed(dir, pkg->depends[i], &dependency))
            fail("%s requires installed package %s", pkg->name, pkg->depends[i]);
        pkg_free(&dependency);
    }
}

static void check_dependents(int db, const char *name)
{
    DIR *dir = entries_at(db);
    for (struct dirent *entry; (entry = next_entry(dir)); ) {
        if (!has_suffix(entry->d_name, RECORD_SUFFIX))
            continue;
        struct package pkg = {0};
        if (!read_record(db, entry->d_name, &pkg))
            fail("package record disappeared: %s", entry->d_name);
        for (size_t i = 0; i < pkg.dependency_count; ++i)
            if (!strcmp(pkg.depends[i], name))
                fail("%s is required by %s", name, pkg.name);
        pkg_free(&pkg);
    }
    closedir(dir);
}

static void check_owners(int db, const struct package *candidate)
{
    DIR *dir = entries_at(db);
    for (struct dirent *entry; (entry = next_entry(dir)); ) {
        if (!has_suffix(entry->d_name, RECORD_SUFFIX))
            continue;
        struct package installed_pkg = {0};
        if (!read_record(db, entry->d_name, &installed_pkg))
            fail("package record disappeared: %s", entry->d_name);
        if (strcmp(candidate->name, installed_pkg.name) != 0)
            for (size_t i = 0; i < candidate->file_count; ++i)
                if (pkg_has_file(&installed_pkg, candidate->files[i].path))
                    fail("%s already owns %s", installed_pkg.name, candidate->files[i].path);
        pkg_free(&installed_pkg);
    }
    closedir(dir);
}

static int prepare_work(struct database db)
{
    if (mkdirat(db.dir, PREPARE, 0700) != 0)
        syserror("create transaction directory", PREPARE);
    int fd = open_work(db.dir, PREPARE);
    if (fd < 0)
        fail("transaction directory disappeared");
    return fd;
}

static bool probe_file(int root, const char *path, struct stat *st)
{
    char name[NAME_MAX + 1];
    int parent = pkg_parent(root, path, false, name);
    if (parent < 0)
        return false;
    bool exists = path_status(parent, name, st);
    close(parent);
    return exists;
}

static void validate_parents(const struct package *pkg, const char *path)
{
    char buffer[PATH_MAX];
    strcpy(buffer, path);
    for (char *slash = strrchr(buffer, '/'); slash; slash = strrchr(buffer, '/')) {
        *slash = '\0';
        if (pkg_has_file(pkg, buffer))
            fail("archive uses a file as a directory: %s", buffer);
    }
}

static void backup_files(int root, int work, const struct package *pkg)
{
    for (size_t i = 0; i < pkg->file_count; ++i) {
        const struct package_file *file = &pkg->files[i];
        char name[NAME_MAX + 1], backup[32];
        int parent = pkg_parent(root, file->path, false, name);
        if (parent < 0)
            continue;
        struct stat st;
        if (path_status(parent, name, &st)) {
            if (!S_ISREG(st.st_mode))
                fail("managed path is no longer a regular file: %s", file->path);
            pkg_slot(backup, "old", file->slot);
            if (linkat(parent, name, work, backup, 0) != 0)
                syserror("back up", file->path);
        }
        close(parent);
    }
}

static void unlink_old(int root, int work, const struct package_file *file)
{
    char name[NAME_MAX + 1], backup[32];
    pkg_slot(backup, "old", file->slot);
    struct stat saved, current;
    if (!path_status(work, backup, &saved))
        return;
    int parent = pkg_parent(root, file->path, false, name);
    if (parent < 0 || !path_status(parent, name, &current) || !same_file(&current, &saved))
        fail("managed path changed during transaction: %s", file->path);
    if (unlinkat(parent, name, 0) != 0)
        syserror("remove", file->path);
    sync_fd(parent, file->path);
    close(parent);
}

static void apply(struct database db, int work, const struct package *before,
                  const struct package *after)
{
    const char *name = after->name ? after->name : before->name;
    if (before->name) {
        write_record(work, "before", before);
        backup_files(db.root, work, before);
    }
    if (after->name)
        write_record(work, "after", after);
    sync_fd(work, PREPARE);

    // A pending transaction always rolls back. Renaming it to done commits it.
    if (renameat(db.dir, PREPARE, db.dir, PENDING) != 0)
        syserror("start transaction for", name);
    sync_fd(db.dir, PENDING);

    for (size_t i = 0; i < before->file_count; ++i)
        unlink_old(db.root, work, &before->files[i]);
    for (size_t i = 0; i < after->file_count; ++i) {
        const struct package_file *file = &after->files[i];
        char filename[NAME_MAX + 1], staged[32];
        int parent = pkg_parent(db.root, file->path, true, filename);
        pkg_slot(staged, "new", file->slot);
        if (linkat(work, staged, parent, filename, 0) != 0)
            syserror("install", file->path);
        sync_fd(parent, file->path);
        close(parent);
    }
    replace_record(db.dir, work, name, after);
    finish_work(db.dir);
}

void db_install(struct database db, const char *path, const char *expected, bool upgrade)
{
    if (db.dir < 0)
        fail("no packages installed in this root");
    struct package before = {0}, after = {0};
    int work = prepare_work(db);
    pkg_read(path, &after, work, false);
    if (expected && strcmp(expected, after.name))
        fail("expected %s but archive contains %s", expected, after.name);

    struct utsname platform;
    if (uname(&platform) != 0)
        syserror("read", "machine architecture");
    if (strcmp(after.arch, platform.machine) && strcmp(after.arch, "noarch"))
        fail("package architecture %s does not match %s", after.arch, platform.machine);
    bool exists = installed(db.dir, after.name, &before);
    if (upgrade) {
        if (!exists)
            fail("%s is not installed", after.name);
        if (strverscmp(after.version, before.version) <= 0)
            fail("%s %s is not newer than %s", after.name, after.version, before.version);
    } else if (exists)
        fail("%s is already installed; use 'pkgr upgrade FILE'", after.name);

    check_dependencies(db.dir, &after);
    check_owners(db.dir, &after);
    for (size_t i = 0; i < after.file_count; ++i) {
        const char *file = after.files[i].path;
        struct stat st;
        validate_parents(&after, file);
        if (probe_file(db.root, file, &st) && (!S_ISREG(st.st_mode) || !pkg_has_file(&before, file)))
            fail("file already exists: %s", file);
    }
    apply(db, work, &before, &after);
    close(work);
    printf("%s %s %s\n", upgrade ? "Upgraded" : "Installed", after.name, after.version);
    pkg_free(&before);
    pkg_free(&after);
}

void db_remove(struct database db, const char *name)
{
    struct package before = {0}, empty = {0};
    if (db.dir < 0 || !installed(db.dir, name, &before))
        fail("%s is not installed", name);
    check_dependents(db.dir, name);
    int work = prepare_work(db);
    apply(db, work, &before, &empty);
    close(work);
    printf("Removed %s %s\n", before.name, before.version);
    pkg_free(&before);
}

static int compare_names(const void *left, const void *right)
{
    const char *const *a = left, *const *b = right;
    return strcmp(*a, *b);
}

void db_list(struct database db)
{
    if (db.dir < 0)
        return;
    DIR *dir = entries_at(db.dir);
    char **names = NULL;
    size_t count = 0;
    for (struct dirent *entry; (entry = next_entry(dir)); ) {
        if (!has_suffix(entry->d_name, RECORD_SUFFIX))
            continue;
        if ((count & (count - 1)) == 0)
            names = grow_array(names, count ? count * 2 : 1, sizeof(*names));
        names[count++] = copy_string(entry->d_name);
    }
    closedir(dir);
    if (count > 1)
        qsort(names, count, sizeof(*names), compare_names);
    for (size_t i = 0; i < count; ++i) {
        struct package pkg = {0};
        if (!read_record(db.dir, names[i], &pkg))
            fail("package record disappeared: %s", names[i]);
        printf("%s %s\n", pkg.name, pkg.version);
        pkg_free(&pkg);
        free(names[i]);
    }
    free(names);
}

void db_info(struct database db, const char *name)
{
    struct package pkg = {0};
    if (db.dir < 0 || !installed(db.dir, name, &pkg))
        fail("%s is not installed", name);
    pkg_print(&pkg);
    pkg_free(&pkg);
}
