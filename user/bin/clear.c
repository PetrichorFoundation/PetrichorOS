#include <ulib.h>

int main(void)
{
    fputs(1, "\033[2J\033[H");
    return 0;
}
