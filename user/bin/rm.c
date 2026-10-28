#include <ulib.h>

static int recursive, force;

static int remove(const char *path)
{
    struct stat st;
    int e = stat(path, &st);
    if (e < 0) {
        if (!force)
            fprintf(2, "rm: %s: %s\n", path, strerror(e));
        return force ? 0 : 1;
    }
    if (st.type == T_DIR) {
        if (!recursive) {
            fprintf(2, "rm: %s: is a directory (use -r)\n", path);
            return 1;
        }
        int fd = open(path, O_RDONLY);
        struct dirent de;
        int rc = 0;
        while (fd >= 0 && readdir(fd, &de) > 0) {
            if (!strcmp(de.name, ".") || !strcmp(de.name, ".."))
                continue;
            char child[PATH_MAX];
            snprintf(child, sizeof child, "%s/%s", path, de.name);
            rc |= remove(child);
            /* removal shifts entries down: restart the scan */
            close(fd);
            fd = open(path, O_RDONLY);
        }
        if (fd >= 0)
            close(fd);
        if (rc)
            return rc;
    }
    e = unlink(path);
    if (e < 0) {
        fprintf(2, "rm: %s: %s\n", path, strerror(e));
        return 1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    int i = 1, rc = 0;
    for (; i < argc && argv[i][0] == '-'; i++) {
        for (char *f = argv[i] + 1; *f; f++) {
            if (*f == 'r') recursive = 1;
            else if (*f == 'f') force = 1;
        }
    }
    if (i == argc) {
        fprintf(2, "usage: rm [-rf] path...\n");
        return 1;
    }
    for (; i < argc; i++)
        rc |= remove(argv[i]);
    return rc;
}
