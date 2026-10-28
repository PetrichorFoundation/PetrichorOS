#include <ulib.h>

int main(void)
{
    struct diskinfo di;
    diskinfo(&di);
    printf("Filesystem      Size(K)    Used(K)   Avail(K)  Mounted on\n");
    if (!di.mounted) {
        printf("(no disk)\n");
        return 0;
    }
    printf("pfs  %14u %10u %10u  /disk\n", di.total_kb, di.total_kb - di.free_kb, di.free_kb);
    return 0;
}
