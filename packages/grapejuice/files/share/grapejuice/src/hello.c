/* A Windows program that uses only kernel32: prints its command line, allocates memory, and exits. */
typedef void *HANDLE;
typedef unsigned int DWORD;

__declspec(dllimport) HANDLE GetStdHandle(DWORD n);
__declspec(dllimport) int WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov);
__declspec(dllimport) char *GetCommandLineA(void);
__declspec(dllimport) void *HeapAlloc(HANDLE heap, DWORD flags, unsigned long long n);
__declspec(dllimport) HANDLE GetProcessHeap(void);
__declspec(dllimport) void ExitProcess(DWORD code);

static void say(const char *s)
{
    DWORD n = 0, w;
    while (s[n]) n++;
    WriteFile(GetStdHandle((DWORD)-11), s, n, &w, 0);
}

int main(void)
{
    say("Hello from a Windows program, running on Petrichor!\r\n");
    say("command line: ");
    say(GetCommandLineA());
    say("\r\n");
    char *mem = HeapAlloc(GetProcessHeap(), 8, 64);
    mem[0] = 'o'; mem[1] = 'k'; mem[2] = '\n';
    say("heap: ");
    say(mem);
    ExitProcess(0);
    return 0;
}
