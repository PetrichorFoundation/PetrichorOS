#include <ulib.h>

int main(void)
{
    u32 t = uptime() / 100;
    printf("up %u:%02u:%02u\n", t / 3600, t / 60 % 60, t % 60);
    return 0;
}
