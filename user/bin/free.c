#include <ulib.h>

int main(void)
{
    struct meminfo mi;
    meminfo(&mi);
    printf("            total      used      free\n");
    printf("RAM    %10u%10u%10u KiB\n", mi.total_kb, mi.total_kb - mi.free_kb, mi.free_kb);
    printf("Heap   %10u%10u%10u KiB (kernel)\n", mi.heap_total_kb,
           mi.heap_total_kb - mi.heap_free_kb, mi.heap_free_kb);
    return 0;
}
