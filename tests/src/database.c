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
#define BATCH ".batch"
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
        struct stat st;
        if (fstatat(fd, entry->d_name, &st, AT_SYMLINK_NOFOLLOW) != 0)
            syserror("inspect transaction file", entry->d_name);
        if (S_ISDIR(st.st_mode))
            clear_work(fd, entry->d_name);
        else if (unlinkat(fd, entry->d_name, 0) != 0)
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

static void finish_work(int db, const char *pending)
{
    if (renameat(db, pending, db, DONE) != 0)
        syserror("finish transaction", pending);
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

static int batch_package(int batch, size_t index, bool create)
{
    char slot[32];
    pkg_slot(slot, "pkg", index);
    if (create && mkdirat(batch, slot, 0700) != 0)
        syserror("create package stage", slot);
    int work = open_work(batch, slot);
    if (work < 0)
        fail("missing package stage %s", slot);
    return work;
}

static void rollback_batch(struct database db, int batch)
{
    size_t count = 0;
    DIR *dir = entries_at(batch);
    for (struct dirent *entry; (entry = next_entry(dir)); )
        if (entry->d_name[0] != '.')
            ++count;
    closedir(dir);
    while (count) {
        int work = batch_package(batch, --count, false);
        rollback(db, work);
        close(work);
    }
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
        finish_work(db.dir, PENDING);
    }
    work = open_work(db.dir, BATCH);
    if (work >= 0) {
        rollback_batch(db, work);
        close(work);
        finish_work(db.dir, BATCH);
    }
    return db;
}

void db_close(struct database db)
{
    if (db.dir >= 0)
        close(db.dir);
    close(db.root);
}

static void check_dependencies(struct database db, const struct package *pkg,
                               const struct package *planned, size_t count)
{
    for (size_t i = 0; i < pkg->dependency_count; ++i) {
        struct package_requirement requirement = pkg_requirement(pkg->depends[i]);
        struct package installed = {0};
        const char *version = NULL;
        for (size_t j = 0; j < count; ++j)
            if (!strcmp(planned[j].name, requirement.name)) {
                version = planned[j].version;
                break;
            }
        if (!version && db_installed(db, requirement.name, &installed))
            version = installed.version;
        if (!pkg_satisfies(requirement, version))
            fail("%s requires %s", pkg->name, pkg->depends[i]);
        pkg_free(&installed);
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
            if (!strcmp(pkg_requirement(pkg.depends[i]).name, name))
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

static void digest_at(int dir, const char *name, char digest[65])
{
    int fd = openat(dir, name, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        syserror("read configuration", name);
    char path[80];
    snprintf(path, sizeof(path), "/proc/%ld/fd/%d", (long)getpid(), fd);
    file_digest(path, digest);
    close(fd);
}

static bool config_modified(int parent, const char *name, const struct stat *st,
                            const struct package_config *config)
{
    if (!config)
        return false;
    if (!S_ISREG(st->st_mode) || !*config->digest)
        return true;
    char digest[65];
    digest_at(parent, name, digest);
    return strcmp(digest, config->digest) != 0;
}

static void config_digests(struct package *pkg, int work)
{
    for (size_t i = 0; i < pkg->file_count; ++i) {
        const struct package_file *file = &pkg->files[i];
        struct package_config *config = pkg_config(pkg, file->path);
        if (config) {
            char slot[32];
            pkg_slot(slot, "new", file->slot);
            digest_at(work, slot, config->digest);
        }
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
            if (!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode))
                fail("unsupported file at managed path: %s", file->path);
            if (config_modified(parent, name, &st, pkg_config(pkg, file->path))) {
                printf("Preserved modified configuration %s\n", file->path);
                close(parent);
                continue;
            }
            pkg_slot(backup, "old", file->slot);
            if (linkat(parent, name, work, backup, 0) != 0)
                syserror("back up", file->path);
        }
        close(parent);
    }
}

static void update_files(struct database db, int work, const struct package *before,
                         const struct package *after)
{
    for (size_t i = 0; i < after->file_count; ++i) {
        const struct package_file *file = &after->files[i];
        char name[NAME_MAX + 1], staged[32];
        int parent = pkg_parent(db.root, file->path, true, name);
        pkg_slot(staged, "new", file->slot);
        struct stat current;
        if (path_status(parent, name, &current)) {
            const struct package_file *old = pkg_file(before, file->path);
            if (!old)
                fail("file already exists: %s", file->path);
            char backup[32];
            pkg_slot(backup, "old", old->slot);
            struct stat saved;
            if (!path_status(work, backup, &saved)) {
                if (!pkg_config(before, file->path))
                    fail("managed path changed during transaction: %s", file->path);
                close(parent);
                continue;
            }
            if (!same_file(&current, &saved))
                fail("managed path changed during transaction: %s", file->path);
            if (linkat(work, staged, work, "replace", 0) != 0 ||
                renameat(work, "replace", parent, name) != 0)
                syserror("replace", file->path);
        } else if (linkat(work, staged, parent, name, 0) != 0)
            syserror("install", file->path);
        sync_fd(parent, file->path);
        close(parent);
    }

    for (size_t i = 0; i < before->file_count; ++i) {
        const struct package_file *file = &before->files[i];
        char name[NAME_MAX + 1], backup[32];
        struct stat saved, current;
        if (pkg_has_file(after, file->path))
            continue;
        pkg_slot(backup, "old", file->slot);
        if (!path_status(work, backup, &saved))
            continue;
        int parent = pkg_parent(db.root, file->path, false, name);
        if (parent < 0 || !path_status(parent, name, &current) || !same_file(&current, &saved))
            fail("managed path changed during transaction: %s", file->path);
        if (unlinkat(parent, name, 0) != 0)
            syserror("remove", file->path);
        sync_fd(parent, file->path);
        close(parent);
    }
}

static void prepare_records(struct database db, int work, const struct package *before,
                            const struct package *after)
{
    if (before->name) {
        write_record(work, "before", before);
        backup_files(db.root, work, before);
    }
    if (after->name)
        write_record(work, "after", after);
    sync_fd(work, PREPARE);
}

static void start_work(struct database db, const char *pending)
{
    if (renameat(db.dir, PREPARE, db.dir, pending) != 0)
        syserror("start transaction", pending);
    sync_fd(db.dir, pending);
}

bool db_installed(struct database db, const char *name, struct package *pkg)
{
    return db.dir >= 0 && installed(db.dir, name, pkg);
}

void db_check_install(struct database db, const struct package *pkg,
                      struct package *before, bool upgrade)
{
    struct utsname platform;
    if (uname(&platform) != 0)
        syserror("read", "machine architecture");
    if (strcmp(pkg->arch, platform.machine) && strcmp(pkg->arch, "noarch"))
        fail("package architecture %s does not match %s", pkg->arch, platform.machine);

    bool exists = db_installed(db, pkg->name, before);
    if (upgrade) {
        if (!exists)
            fail("%s is not installed", pkg->name);
        if (strverscmp(pkg->version, before->version) <= 0)
            fail("%s %s is not newer than %s", pkg->name, pkg->version, before->version);
    } else if (exists)
        fail("%s is already installed; use 'pkgr upgrade FILE'", pkg->name);
}

static void preflight_files(struct database db, const struct package *before,
                            const struct package *after)
{
    struct stat database;
    if (fstat(db.dir, &database) != 0)
        syserror("stat", "package database");
    for (size_t i = 0; i < after->file_count; ++i) {
        const char *file = after->files[i].path;
        char name[NAME_MAX + 1];
        struct stat st;
        validate_parents(after, file);
        int parent = pkg_parent(db.root, file, false, name);
        if (parent >= 0 && path_status(parent, name, &st) &&
            ((!S_ISREG(st.st_mode) && !S_ISLNK(st.st_mode)) || !pkg_has_file(before, file)))
            fail("file already exists: %s", file);
        char ancestor[PATH_MAX];
        strcpy(ancestor, file);
        while (parent < 0) {
            *strrchr(ancestor, '/') = '\0';
            parent = pkg_parent(db.root, ancestor, false, name);
        }
        if (fstat(parent, &st) != 0)
            syserror("stat parent of", file);
        if (database.st_dev != st.st_dev)
            fail("package paths must be on the database filesystem: %s", file);
        close(parent);
    }
}

static void check_batch(struct database db, const struct package *after, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        check_dependencies(db, &after[i], after, count);
        for (size_t j = 0; j < count; ++j) {
            if (i == j)
                continue;
            if (!strcmp(after[i].name, after[j].name))
                fail("duplicate planned package: %s", after[i].name);
            for (size_t k = 0; k < after[i].file_count; ++k) {
                const char *path = after[i].files[k].path;
                if (pkg_has_file(&after[j], path))
                    fail("%s and %s both install %s", after[i].name, after[j].name, path);
                validate_parents(&after[j], path);
            }
        }
    }

    size_t installed_count;
    char **names = db_package_names(db, &installed_count);
    for (size_t i = 0; i < installed_count; ++i) {
        bool replaced = false;
        for (size_t j = 0; j < count; ++j)
            if (!strcmp(names[i], after[j].name))
                replaced = true;
        if (!replaced) {
            struct package installed = {0};
            if (!db_installed(db, names[i], &installed))
                fail("package record disappeared: %s", names[i]);
            check_dependencies(db, &installed, after, count);
            pkg_free(&installed);
        }
        free(names[i]);
    }
    free(names);
}

void db_apply(struct database db, const struct package_change *changes, size_t count)
{
    if (!count)
        return;
    int batch = prepare_work(db);
    struct package *before = calloc(count, sizeof(*before));
    struct package *after = calloc(count, sizeof(*after));
    if (!before || !after)
        fail("out of memory");
    for (size_t i = 0; i < count; ++i) {
        int work = batch_package(batch, i, true);
        pkg_read(changes[i].path, &after[i], work, false);
        if (strcmp(changes[i].name, after[i].name) || strcmp(changes[i].version, after[i].version))
            fail("package archive changed after planning: %s", changes[i].path);
        db_check_install(db, &after[i], &before[i], changes[i].upgrade);
        after[i].essential |= before[i].essential;
        check_owners(db.dir, &after[i]);
        preflight_files(db, &before[i], &after[i]);
        config_digests(&after[i], work);
        prepare_records(db, work, &before[i], &after[i]);
        close(work);
    }
    check_batch(db, after, count);
    sync_fd(batch, PREPARE);

    start_work(db, BATCH);
    for (size_t i = 0; i < count; ++i) {
        int work = batch_package(batch, i, false);
        update_files(db, work, &before[i], &after[i]);
        replace_record(db.dir, work, after[i].name, &after[i]);
        close(work);
    }
    finish_work(db.dir, BATCH);
    close(batch);
    for (size_t i = 0; i < count; ++i) {
        printf("%s %s %s\n", changes[i].upgrade ? "Upgraded" : "Installed",
               after[i].name, after[i].version);
        pkg_free(&before[i]);
        pkg_free(&after[i]);
    }
    free(before);
    free(after);
}

static bool matching_files(int left_dir, const char *left_name, const struct stat *left,
                           int right_dir, const char *right_name, const struct stat *right)
{
    if (S_ISLNK(left->st_mode) && S_ISLNK(right->st_mode)) {
        char a[PATH_MAX], b[PATH_MAX];
        ssize_t length = readlinkat(left_dir, left_name, a, sizeof(a));
        ssize_t other = readlinkat(right_dir, right_name, b, sizeof(b));
        return length >= 0 && length < PATH_MAX && length == other && !memcmp(a, b, length);
    }
    if (!S_ISREG(left->st_mode) || !S_ISREG(right->st_mode) ||
        left->st_mode != right->st_mode || left->st_size != right->st_size)
        return false;
    char a[65], b[65];
    digest_at(left_dir, left_name, a);
    digest_at(right_dir, right_name, b);
    return !strcmp(a, b);
}

void db_adopt(struct database db, const char *path)
{
    struct package before = {0}, after = {0};
    int work = prepare_work(db);
    pkg_read(path, &after, work, false);
    db_check_install(db, &after, &before, false);
    check_dependencies(db, &after, NULL, 0);
    check_owners(db.dir, &after);
    config_digests(&after, work);
    for (size_t i = 0; i < after.file_count; ++i) {
        const struct package_file *file = &after.files[i];
        char name[NAME_MAX + 1], slot[32];
        pkg_slot(slot, "new", file->slot);
        int parent = pkg_parent(db.root, file->path, false, name);
        struct stat current, staged;
        if (parent < 0 || !path_status(parent, name, &current))
            fail("cannot adopt missing file: %s", file->path);
        if (!path_status(work, slot, &staged))
            fail("missing adoption payload: %s", file->path);
        if (!(pkg_config(&after, file->path) && S_ISREG(current.st_mode)) &&
            !matching_files(parent, name, &current, work, slot, &staged))
            fail("cannot adopt modified file: %s", file->path);
        close(parent);
    }
    replace_record(db.dir, work, after.name, &after);
    close(work);
    clear_work(db.dir, PREPARE);
    printf("Adopted %s %s\n", after.name, after.version);
    pkg_free(&after);
}

void db_remove(struct database db, const char *name)
{
    struct package before = {0}, empty = {0};
    if (db.dir < 0 || !installed(db.dir, name, &before))
        fail("%s is not installed", name);
    if (before.essential || !strcmp(name, "pkgr"))
        fail("%s is an essential system package", name);
    check_dependents(db.dir, name);
    int work = prepare_work(db);
    prepare_records(db, work, &before, &empty);
    start_work(db, PENDING);
    update_files(db, work, &before, &empty);
    replace_record(db.dir, work, name, &empty);
    finish_work(db.dir, PENDING);
    close(work);
    printf("Removed %s %s\n", before.name, before.version);
    pkg_free(&before);
}

static int compare_names(const void *left, const void *right)
{
    const char *const *a = left, *const *b = right;
    return strcmp(*a, *b);
}

char **db_package_names(struct database db, size_t *count)
{
    *count = 0;
    if (db.dir < 0)
        return NULL;
    DIR *dir = entries_at(db.dir);
    char **names = NULL;
    for (struct dirent *entry; (entry = next_entry(dir)); ) {
        if (!has_suffix(entry->d_name, RECORD_SUFFIX))
            continue;
        if ((*count & (*count - 1)) == 0)
            names = grow_array(names, *count ? *count * 2 : 1, sizeof(*names));
        char *name = copy_string(entry->d_name);
        name[strlen(name) - strlen(RECORD_SUFFIX)] = '\0';
        names[(*count)++] = name;
    }
    closedir(dir);
    if (*count > 1)
        qsort(names, *count, sizeof(*names), compare_names);
    return names;
}

void db_list(struct database db)
{
    size_t count;
    char **names = db_package_names(db, &count);
    for (size_t i = 0; i < count; ++i) {
        struct package pkg = {0};
        if (!db_installed(db, names[i], &pkg))
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
