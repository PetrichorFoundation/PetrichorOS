#include <ulib.h>

int main(int argc, char **argv)
{
    if (argc != 2) {
        fprintf(2, "usage: sleep seconds\n");
        return 1;
    }
    return msleep(atoi(argv[1]) * 1000) < 0;
}
