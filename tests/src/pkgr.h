#ifndef PKGR_H
#define PKGR_H

#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/types.h>

#define PKGR_VERSION "0.5.3"
#define DEFAULT_REPO "usr/share/pkgr/repo"
#define PACKAGE_SUFFIX ".pkgr.tar.gz"
#define INDEX_HEADER "PKGR-INDEX-1\n"

struct package_file {
    char *path;
    size_t slot;
};

struct package_config {
    char *path;
    char digest[65];
};

struct package_requirement {
    char name[121];
    const char *version;
    bool minimum;
};

struct package_change {
    char *name;
    char *path;
    char *version;
    bool upgrade;
};

struct package {
    char *name;
    char *version;
    char *arch;
    char *description;
    bool essential;
    struct package_config *configs;
    size_t config_count;
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
const char *temp_directory(void);
void run_tool(char *const args[], char *const fallback[], int output);
void sync_fd(int fd, const char *path);
bool valid_token(const char *text);
bool has_suffix(const char *str, const char *suffix);

struct package_requirement pkg_requirement(const char *expression);
bool pkg_satisfies(struct package_requirement requirement, const char *version);
struct package_config *pkg_config(const struct package *pkg, const char *path);
void pkg_slot(char name[32], const char *prefix, size_t slot);
void pkg_free(struct package *pkg);
void pkg_parse(FILE *stream, struct package *pkg, bool record);
void pkg_write(FILE *stream, const struct package *pkg, bool record);
const struct package_file *pkg_file(const struct package *pkg, const char *path);
bool pkg_has_file(const struct package *pkg, const char *path);
void pkg_read(const char *path, struct package *pkg, int stage, bool metadata_only);
char *pkg_pack(const char *manifest, const char *root);
char *pkg_build(const char *recipe);
void pkg_print(const struct package *pkg);
char *repo_find(const char *directory, struct package_requirement requirement,
                const char *newer_than);
void repo_search(const char *directory, const char *filter);
void repo_index(const char *directory);
char *build_source(int argc, char **argv);
bool repo_is_remote(const char *url);
bool repo_filename(const char *name);
void file_digest(const char *path, char digest[65]);
void repo_sync(const char *root, const char *url);
void remote_search(const char *root, const char *url, const char *filter);
char *remote_find(struct database db, const char *root, const char *url,
                  struct package_requirement requirement, const char *newer_than);
char *pkgr_release(const char *newer_than);
void install_package(struct database db, const char *root, const char *repo,
                     const char *name, const char *path, bool upgrade);

void upgrade_packages(struct database db, const char *root, const char *repo,
                      const char *name, bool from_release);

struct database db_open(const char *path, bool create);
void db_close(struct database db);
bool db_installed(struct database db, const char *name, struct package *pkg);
void db_check_install(struct database db, const struct package *pkg,
                      struct package *before, bool upgrade);
void db_apply(struct database db, const struct package_change *changes, size_t count);
void db_adopt(struct database db, const char *path);
void db_remove(struct database db, const char *name);
char **db_package_names(struct database db, size_t *count);
void db_list(struct database db);
void db_info(struct database db, const char *name);

#endif
