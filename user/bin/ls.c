#include <ulib.h>

static int flag_l, flag_a;

static const char *color(u32 type)
{
    switch (type) {
    case T_DIR: return "\033[1;34m";
    case T_DEV: return "\033[1;33m";
    }
    return "";
}

static void show(const char *dir, const char *name, u32 type, int *col)
{
    if (flag_l) {
        char path[PATH_MAX];
        struct stat st = {type, 0, 0, 0};
        snprintf(path, sizeof path, "%s/%s", dir, name);
        stat(path, &st);
        printf("%c %3d %8d  %s%s\033[0m\n", "?-dcp"[st.type <= 4 ? st.type : 0], st.nlink,
               st.size, color(type), name);
        return;
    }
    int len = strlen(name) + 2;
    if (*col + len > 78) {
        printf("\n");
        *col = 0;
    }
    printf("%s%s\033[0m  ", color(type), name);
    *col += len;
}

static int by_name(const void *a, const void *b)
{
    return strcmp(((const struct dirent *)a)->name, ((const struct dirent *)b)->name);
}

static void sort(struct dirent *v, int n)
{
    for (int i = 1; i < n; i++) {
        struct dirent t = v[i];
        int j = i - 1;
        while (j >= 0 && by_name(&v[j], &t) > 0) {
            v[j + 1] = v[j];
            j--;
        }
        v[j + 1] = t;
    }
}

static int list(const char *path, int header)
{
    struct stat st;
    int e = stat(path, &st);
    if (e < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(e));
        return 1;
    }
    int col = 0;
    if (st.type != T_DIR) {
        show(".", path, st.type, &col);
        if (!flag_l)
            printf("\n");
        return 0;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(2, "ls: %s: %s\n", path, strerror(fd));
        return 1;
    }
    struct dirent *v = malloc((st.size + 1) * sizeof *v);
    int n = 0;
    while (n < (int)st.size + 1 && readdir(fd, &v[n]) > 0)
        n++;
    close(fd);
    sort(v, n);
    if (header)
        printf("%s:\n", path);
    for (int i = 0; i < n; i++)
        if (flag_a || v[i].name[0] != '.')
            show(path, v[i].name, v[i].type, &col);
    if (!flag_l && col)
        printf("\n");
    free(v);
    return 0;
}

int main(int argc, char **argv)
{
    int first = 1;
    for (; first < argc && argv[first][0] == '-'; first++) {
        for (char *f = argv[first] + 1; *f; f++) {
            if (*f == 'l') flag_l = 1;
            else if (*f == 'a') flag_a = 1;
        }
    }
    int rc = 0, paths = argc - first;
    if (paths == 0)
        return list(".", 0);
    for (int i = first; i < argc; i++)
        rc |= list(argv[i], paths > 1);
    return rc;
}
