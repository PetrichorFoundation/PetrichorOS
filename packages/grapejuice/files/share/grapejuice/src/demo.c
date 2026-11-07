/* A Windows program that uses the C runtime (msvcrt) and a message box. */
__declspec(dllimport) int printf(const char *fmt, ...);
__declspec(dllimport) int sprintf(char *buf, const char *fmt, ...);
__declspec(dllimport) void *malloc(unsigned long long n);
__declspec(dllimport) void free(void *p);
__declspec(dllimport) int MessageBoxA(void *wnd, const char *text, const char *caption, unsigned type);

static int is_prime(int n)
{
    if (n < 2) return 0;
    for (int i = 2; i * i <= n; i++)
        if (n % i == 0) return 0;
    return 1;
}

int main(int argc, char **argv)
{
    printf("GrapeJuice demo: %d argument(s)\n", argc);
    for (int i = 0; i < argc; i++)
        printf("  argv[%d] = \"%s\"\n", i, argv[i]);
    printf("primes below 50:");
    int count = 0;
    for (int i = 0; i < 50; i++)
        if (is_prime(i)) { printf(" %d", i); count++; }
    printf("  (%d of them)\n", count);
    char *buf = malloc(64);
    sprintf(buf, "%-6s|%6s|%04x|%lld", "left", "right", 0xbeef, 1234567890123LL);
    printf("formatted: %s\n", buf);
    free(buf);
    MessageBoxA(0, "Windows programs can run here now.", "GrapeJuice", 0);
    return 7;
}
