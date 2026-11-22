#ifndef PKGR_H
#define PKGR_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define PKGR_VERSION "0.1.1"
#define DEFAULT_REPO "usr/share/pkgr/repo"
#define PACKAGE_SUFFIX ".pkgr.tar.gz"

struct package_file {
    char *path;
    size_t slot;
};

struct package {
    char *name;
    char *version;
    char *arch;
    char *description;
    char **depends;
    size_t dependency_count;
    struct package_file *files;
    size_t file_count;
};

struct database {
    int root;
    int dir;
};

_Noreturn void fail(const char *format, ...) __attribute__((format(printf, 1, 2)));
_Noreturn void syserror(const char *operation, const char *path);
char *copy_string(const char *value);
void *grow_array(void *array, size_t count, size_t size);
char *path_join(const char *base, const char *name);
void remove_tree(const char *path);
void sync_fd(int fd, const char *path);
bool valid_token(const char *text);
bool has_suffix(const char *str, const char *suffix);

void pkg_slot(char name[32], const char *prefix, size_t slot);
void pkg_free(struct package *pkg);
void pkg_parse(FILE *stream, struct package *pkg, bool record);
void pkg_write(FILE *stream, const struct package *pkg, bool record);
bool pkg_has_file(const struct package *pkg, const char *path);
void pkg_read(const char *path, struct package *pkg, int stage, bool metadata_only);
char *pkg_pack(const char *manifest, const char *root);
char *pkg_build(const char *recipe);
void pkg_print(const struct package *pkg);
char *repo_find(const char *directory, const char *name);
void repo_search(const char *directory, const char *filter);

struct database db_open(const char *path, bool create);
void db_close(struct database db);
void db_install(struct database db, const char *path, const char *expected, bool upgrade);
void db_remove(struct database db, const char *name);
void db_list(struct database db);
void db_info(struct database db, const char *name);

#endif
