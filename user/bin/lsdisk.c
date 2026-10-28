#include <ulib.h>

int main(void)
{
    struct driveinfo d[4];
    int n = drives(d, 4);
    printf("NAME  SIZE        MODEL\n");
    for (int i = 0; i < n; i++)
        printf("%-5s %6u MiB  %s%s\n", d[i].name, d[i].sectors / 2048, d[i].model, d[i].boot ? "  (boot disk)" : "");
    if (!n)
        printf("(no disks)\n");
    return 0;
}
