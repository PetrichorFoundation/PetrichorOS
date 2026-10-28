#include <ulib.h>

int main(void)
{
    static const char *names[] = {"unused", "embryo", "sleep", "ready", "run", "zombie"};
    struct procinfo pi[64];
    int n = ps(pi, 64);
    printf("  PID  PPID STATE    MEM(K)  TICKS CMD\n");
    for (int i = 0; i < n; i++)
        printf("%5d %5d %-6s %8d %6d %s\n", pi[i].pid, pi[i].ppid,
               names[pi[i].state <= PS_ZOMBIE ? pi[i].state : 0], pi[i].mem_kb, pi[i].ticks,
               pi[i].name);
    return 0;
}
