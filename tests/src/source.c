#include "pkgr.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>

char *build_source(int argc, char **argv)
{
    const char *source = argv[0];
    bool git = !strncmp(source, "https://", 8) || !strncmp(source, "ssh://", 6) ||
               !strncmp(source, "file://", 7);
    if (!git) {
        if (argc != 1)
            fail("a local recipe takes one argument");
        return pkg_build(source);
    }
    if (geteuid() == 0)
        fail("run source builds as a regular user, then install the resulting archive");
    if (argc == 3 && (!*argv[2] || argv[2][0] == '-'))
        fail("invalid Git revision");

    const char *work = temp_directory();
    char *checkout = path_join(work, "source");
    run_tool((char *[]){"git", "clone", "--", argv[0], checkout, NULL}, NULL, STDERR_FILENO);
    if (argc == 3)
        run_tool((char *[]){"git", "-C", checkout, "checkout", "--detach", argv[2], "--", NULL},
                 NULL, STDERR_FILENO);

    char *path = path_join(checkout, argc >= 2 ? argv[1] : ".");
    char *recipe = realpath(path, NULL);
    if (!recipe)
        syserror("resolve recipe", path);
    size_t prefix = strlen(checkout);
    if (strncmp(recipe, checkout, prefix) || (recipe[prefix] && recipe[prefix] != '/'))
        fail("recipe directory is outside the Git checkout");
    char *archive = pkg_build(recipe);
    free(recipe);
    free(path);
    free(checkout);
    return archive;
}
