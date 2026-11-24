#include "pkgr.h"

#include <stdlib.h>
#include <string.h>

struct install_plan {
    struct database db;
    const char *root;
    const char *repo;
    struct package_change *entries;
    size_t count;
};

struct dependency {
    const char *name;
    const struct dependency *parent;
};

static char *find_archive(const struct install_plan *plan, struct package_requirement requirement,
                          const char *newer_than)
{
    if (repo_is_remote(plan->repo))
        return remote_find(plan->db, plan->root, plan->repo, requirement, newer_than);
    return repo_find(plan->repo, requirement, newer_than);
}

static const struct package_change *planned(const struct install_plan *plan, const char *name)
{
    for (size_t i = 0; i < plan->count; ++i)
        if (!strcmp(plan->entries[i].name, name))
            return &plan->entries[i];
    return NULL;
}

static void plan_package(struct install_plan *plan, const char *expression, char *path,
                         bool upgrade, const struct dependency *parent, size_t depth)
{
    if (depth > 128)
        fail("dependency chain is too deep at %s", expression);
    struct package_requirement requirement = {0};
    if (expression)
        requirement = pkg_requirement(expression);
    if (parent) {
        const struct package_change *change = planned(plan, requirement.name);
        if (change) {
            if (!pkg_satisfies(requirement, change->version))
                fail("%s requires %s, but %s is selected", parent->name, expression, change->version);
            return;
        }
        struct package installed = {0};
        bool exists = db_installed(plan->db, requirement.name, &installed);
        if (exists && pkg_satisfies(requirement, installed.version)) {
            pkg_free(&installed);
            return;
        }
        for (const struct dependency *dep = parent; dep; dep = dep->parent)
            if (!strcmp(dep->name, requirement.name))
                fail("dependency cycle involving %s", requirement.name);

        path = find_archive(plan, requirement, installed.version);
        if (!path)
            fail("%s requires %s, but %s is installed", parent->name, expression, installed.version);
        pkg_free(&installed);
        upgrade = exists;
    }

    if (!path)
        path = find_archive(plan, requirement, NULL);
    struct package pkg = {0}, before = {0};
    pkg_read(path, &pkg, -1, true);
    if (expression && strcmp(requirement.name, pkg.name))
        fail("expected %s but archive contains %s", requirement.name, pkg.name);
    db_check_install(plan->db, &pkg, &before, upgrade);
    pkg_free(&before);

    struct dependency current = { pkg.name, parent };
    for (size_t i = 0; i < pkg.dependency_count; ++i)
        plan_package(plan, pkg.depends[i], NULL, false, &current, depth + 1);

    plan->entries = grow_array(plan->entries, plan->count + 1, sizeof(*plan->entries));
    plan->entries[plan->count++] = (struct package_change){
        .name = copy_string(pkg.name), .path = path, .version = copy_string(pkg.version), .upgrade = upgrade
    };
    pkg_free(&pkg);
}

static void apply_plan(struct install_plan *plan)
{
    db_apply(plan->db, plan->entries, plan->count);
    for (size_t i = 0; i < plan->count; ++i) {
        free(plan->entries[i].path);
        free(plan->entries[i].name);
        free(plan->entries[i].version);
    }
    free(plan->entries);
}

void install_package(struct database db, const char *root, const char *repo,
                     const char *name, const char *path, bool upgrade)
{
    if (db.dir < 0)
        fail("no packages installed in this root");
    struct install_plan plan = { .db = db, .root = root, .repo = repo };
    plan_package(&plan, name, path ? copy_string(path) : NULL, upgrade, NULL, 0);
    apply_plan(&plan);
}

static void plan_upgrade(struct install_plan *plan, const char *name, bool from_release)
{
    if (planned(plan, name))
        return;
    struct package old = {0};
    if (!db_installed(plan->db, name, &old))
        fail("%s is not installed", name);
    char *archive = from_release ? pkgr_release(old.version) : find_archive(plan, pkg_requirement(name), old.version);
    pkg_free(&old);
    if (archive)
        plan_package(plan, name, archive, true, NULL, 0);
}

void upgrade_packages(struct database db, const char *root, const char *repo,
                      const char *name, bool from_release)
{
    struct install_plan plan = { .db = db, .root = root, .repo = repo };
    if (name) {
        plan_upgrade(&plan, name, from_release);
        if (!plan.count)
            printf("No newer version of %s\n", name);
    } else {
        size_t count;
        char **names = db_package_names(db, &count);
        for (size_t i = 0; i < count; ++i) {
            plan_upgrade(&plan, names[i], false);
            free(names[i]);
        }
        free(names);
        if (!plan.count)
            puts("No package updates available in the selected repository");
    }
    apply_plan(&plan);
}
