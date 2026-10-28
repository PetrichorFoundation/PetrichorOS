#include <ulib.h>

int main(void)
{
    char buf[PATH_MAX];
    int e = getcwd(buf, sizeof buf);
    if (e < 0) {
        fprintf(2, "pwd: %s\n", strerror(e));
        return 1;
    }
    puts(buf);
    return 0;
}
