/* GrapeJuice: a small Windows translation layer for Petrichor.
 *
 *   grape program.exe [arguments...]
 *
 * It loads a 64-bit Windows program (PE32+), applies its relocations, fills in its imports with its own
 * implementations of the Windows (kernel32, user32's MessageBox) and C runtime (msvcrt/ucrt) functions the
 * program calls, and jumps to its entry point.  Windows functions use the Microsoft x64 calling convention;
 * every function below is declared ms_abi so the compiler handles the difference from Petrichor's own.
 *
 * What it can run: console programs that are self-contained or that use only the functions listed in
 * `imports` below, and that were built with relocations (the default).  What it can't: 32-bit programs,
 * programs that need a thread environment block (the CRT startup code of MSVC/MinGW), exceptions (SEH),
 * threads, DLLs of their own, or graphics.  An unimplemented import is reported by name when it is called.
 */
#include <ulib.h>

#define WINAPI __attribute__((ms_abi))
typedef void *HANDLE;
typedef unsigned int DWORD;
typedef int BOOL;

static int verbose;
static DWORD last_error;
static const char *cmdline;
static u8 *image_base;

/* ------------------------------------------------------------------ small helpers */

static u16 rd16(const u8 *p) { return p[0] | p[1] << 8; }
static u32 rd32(const u8 *p) { return p[0] | p[1] << 8 | p[2] << 16 | (u32)p[3] << 24; }
static u64 rd64(const u8 *p) { return rd32(p) | (u64)rd32(p + 4) << 32; }

static void die(const char *msg, const char *detail)
{
    fprintf(2, "grape: %s%s%s\n", msg, detail ? ": " : "", detail ? detail : "");
    exit(1);
}

/* Windows paths to Petrichor paths: "C:\dir\file" -> "/dir/file" */
static void unix_path(char *out, int cap, const char *in)
{
    int n = 0;
    if (in[0] && in[1] == ':')
        in += 2;
    for (; *in && n < cap - 1; in++)
        out[n++] = *in == '\\' ? '/' : *in;
    out[n] = 0;
}

/* Handles: the three standard ones, then file descriptors. */
#define H_STDIN  ((HANDLE)0x10)
#define H_STDOUT ((HANDLE)0x11)
#define H_STDERR ((HANDLE)0x12)
#define H_INVALID ((HANDLE)(i64)-1)

static HANDLE fd_handle(int fd) { return (HANDLE)(u64)(fd + 0x20); }

static int handle_fd(HANDLE h)
{
    u64 v = (u64)h;
    if (v >= 0x10 && v <= 0x12)
        return (int)(v - 0x10);
    if (v >= 0x20 && v < 0x20 + 64)
        return (int)(v - 0x20);
    return -1;
}

/* ------------------------------------------------------------------ printf for Microsoft varargs */

typedef __builtin_ms_va_list ms_va_list;

static int emit(char *buf, int cap, int n, char c)
{
    if (n + 1 < cap)
        buf[n] = c;
    return n + 1;
}

static int ms_vsnprintf(char *buf, int cap, const char *fmt, ms_va_list ap)
{
    int n = 0;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            n = emit(buf, cap, n, *fmt);
            continue;
        }
        fmt++;
        int left = 0, zero = 0, width = 0, prec = -1, big = 0, plus = 0;
        for (;; fmt++) {
            if (*fmt == '-') left = 1;
            else if (*fmt == '0') zero = 1;
            else if (*fmt == '+') plus = 1;
            else if (*fmt == ' ' || *fmt == '#') {}
            else break;
        }
        if (*fmt == '*') {
            width = __builtin_va_arg(ap, int);
            if (width < 0) { left = 1; width = -width; }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                width = width * 10 + *fmt++ - '0';
        }
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') { prec = __builtin_va_arg(ap, int); fmt++; }
            else while (*fmt >= '0' && *fmt <= '9') prec = prec * 10 + *fmt++ - '0';
        }
        for (;; fmt++) {                                  /* length modifiers; long is 32 bits on Windows */
            if (*fmt == 'l' && fmt[1] == 'l') { big = 1; fmt++; }
            else if (*fmt == 'l' || *fmt == 'h' || *fmt == 'w') {}
            else if (*fmt == 'z' || *fmt == 'j' || *fmt == 't') big = 1;
            else if (*fmt == 'I' && fmt[1] == '6' && fmt[2] == '4') { big = 1; fmt += 2; }
            else break;
        }
        char tmp[24];
        const char *s = tmp;
        int len = 0, numeric = 0;
        char sign = 0;
        switch (*fmt) {
        case 'd': case 'i': {
            i64 v = big ? __builtin_va_arg(ap, i64) : (i64)__builtin_va_arg(ap, int);
            u64 u = v < 0 ? -(u64)v : (u64)v;
            if (v < 0) sign = '-'; else if (plus) sign = '+';
            char r[24]; int k = 0;
            do { r[k++] = '0' + u % 10; u /= 10; } while (u);
            while (k) tmp[len++] = r[--k];
            numeric = 1;
            break;
        }
        case 'u': case 'x': case 'X': case 'p': {
            u64 u = *fmt == 'p' || big ? __builtin_va_arg(ap, u64) : (u64)__builtin_va_arg(ap, unsigned int);
            u32 base = *fmt == 'u' ? 10 : 16;
            const char *dig = *fmt == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char r[24]; int k = 0;
            do { r[k++] = dig[u % base]; u /= base; } while (u);
            while (k) tmp[len++] = r[--k];
            numeric = 1;
            break;
        }
        case 'c':
            tmp[0] = (char)__builtin_va_arg(ap, int);
            len = 1;
            break;
        case 's': {
            s = __builtin_va_arg(ap, const char *);
            if (!s) s = "(null)";
            len = 0;
            while (s[len] && (prec < 0 || len < prec)) len++;
            break;
        }
        case '%':
            tmp[0] = '%';
            len = 1;
            break;
        case 0:
            return n;
        default:                                          /* floats and anything else: not supported */
            tmp[0] = '?';
            len = 1;
            (void)__builtin_va_arg(ap, u64);
            break;
        }
        if (numeric && prec >= 0 && len < prec) {
            memmove(tmp + (prec - len), tmp, len);
            for (int i = 0; i < prec - len; i++) tmp[i] = '0';
            len = prec;
        }
        int total = len + (sign ? 1 : 0), padn = width > total ? width - total : 0;
        if (!left && !(zero && numeric))
            for (int i = 0; i < padn; i++) n = emit(buf, cap, n, ' ');
        if (sign) n = emit(buf, cap, n, sign);
        if (!left && zero && numeric)
            for (int i = 0; i < padn; i++) n = emit(buf, cap, n, '0');
        for (int i = 0; i < len; i++) n = emit(buf, cap, n, s[i]);
        if (left)
            for (int i = 0; i < padn; i++) n = emit(buf, cap, n, ' ');
    }
    return n;
}

static int out_text(int fd, const char *s, int n)
{
    int off = 0;
    while (off < n) {
        int w = write(fd, s + off, n - off);
        if (w <= 0)
            return off ? off : -1;
        off += w;
    }
    return n;
}

static int out_fmt(int fd, const char *fmt, ms_va_list ap)
{
    char *buf = malloc(4096);
    if (!buf)
        return -1;
    int n = ms_vsnprintf(buf, 4096, fmt, ap);
    int shown = n < 4095 ? n : 4095;
    out_text(fd, buf, shown);
    free(buf);
    return n;
}

/* ------------------------------------------------------------------ kernel32 */

static HANDLE WINAPI k_GetStdHandle(DWORD n) { return n == (DWORD)-10 ? H_STDIN : n == (DWORD)-11 ? H_STDOUT : n == (DWORD)-12 ? H_STDERR : H_INVALID; }

static BOOL WINAPI k_WriteFile(HANDLE h, const void *buf, DWORD n, DWORD *written, void *ov)
{
    int fd = handle_fd(h);
    if (fd < 0) { last_error = 6; return 0; }
    int w = out_text(fd, buf, n);
    if (written) *written = w < 0 ? 0 : w;
    return w >= 0;
}

static BOOL WINAPI k_ReadFile(HANDLE h, void *buf, DWORD n, DWORD *got, void *ov)
{
    int fd = handle_fd(h);
    if (fd < 0) { last_error = 6; return 0; }
    int r = read(fd, buf, n);
    if (got) *got = r < 0 ? 0 : r;
    return r >= 0;
}

static BOOL WINAPI k_WriteConsoleA(HANDLE h, const void *buf, DWORD n, DWORD *written, void *res) { return k_WriteFile(h, buf, n, written, 0); }

static BOOL WINAPI k_WriteConsoleW(HANDLE h, const u16 *buf, DWORD n, DWORD *written, void *res)
{
    char *tmp = malloc(n + 1);
    if (!tmp) return 0;
    for (DWORD i = 0; i < n; i++) tmp[i] = buf[i] < 128 ? (char)buf[i] : '?';
    BOOL ok = k_WriteFile(h, tmp, n, written, 0);
    free(tmp);
    return ok;
}

static BOOL WINAPI k_ReadConsoleA(HANDLE h, void *buf, DWORD n, DWORD *got, void *ctl) { return k_ReadFile(h, buf, n, got, 0); }

static void WINAPI k_ExitProcess(DWORD code) { exit((int)code); }
static BOOL WINAPI k_TerminateProcess(HANDLE h, DWORD code) { exit((int)code); }
static const char *WINAPI k_GetCommandLineA(void) { return cmdline; }

static u16 *wide_cmdline;
static const u16 *WINAPI k_GetCommandLineW(void)
{
    if (!wide_cmdline) {
        int n = strlen(cmdline);
        wide_cmdline = malloc((n + 1) * 2);
        for (int i = 0; i <= n && wide_cmdline; i++) wide_cmdline[i] = (u8)cmdline[i];
    }
    return wide_cmdline;
}

static DWORD WINAPI k_GetLastError(void) { return last_error; }
static void WINAPI k_SetLastError(DWORD e) { last_error = e; }
static HANDLE WINAPI k_GetModuleHandleA(const char *name) { return name ? NULL : image_base; }
static HANDLE WINAPI k_GetModuleHandleW(const u16 *name) { return name ? NULL : image_base; }

static void *lookup_import(const char *name);
static void *WINAPI k_GetProcAddress(HANDLE mod, const char *name) { return lookup_import(name); }
static HANDLE WINAPI k_LoadLibraryA(const char *name) { return (HANDLE)1; }   /* every library is "loaded": imports resolve by name */
static BOOL WINAPI k_FreeLibrary(HANDLE h) { return 1; }

static void *WINAPI k_VirtualAlloc(void *addr, u64 size, DWORD type, DWORD prot)
{
    u8 *raw = calloc(1, size + 4096 + 16);
    if (!raw) { last_error = 8; return NULL; }
    u8 *p = (u8 *)(((u64)raw + 16 + 4095) & ~4095ull);
    ((u8 **)p)[-1] = raw;
    return p;
}

static BOOL WINAPI k_VirtualFree(void *addr, u64 size, DWORD type)
{
    if (addr && (type & 0x8000))                          /* MEM_RELEASE */
        free(((u8 **)addr)[-1]);
    return 1;
}

static BOOL WINAPI k_VirtualProtect(void *addr, u64 size, DWORD prot, DWORD *old) { if (old) *old = 4; return 1; }

static HANDLE WINAPI k_GetProcessHeap(void) { return (HANDLE)0x30; }
static HANDLE WINAPI k_HeapCreate(DWORD opts, u64 init, u64 max) { return (HANDLE)0x30; }
static void *WINAPI k_HeapAlloc(HANDLE h, DWORD flags, u64 n) { return flags & 8 ? calloc(1, n ? n : 1) : malloc(n ? n : 1); }
static BOOL WINAPI k_HeapFree(HANDLE h, DWORD flags, void *p) { free(p); return 1; }
static void *WINAPI k_HeapReAlloc(HANDLE h, DWORD flags, void *p, u64 n) { return realloc(p, n ? n : 1); }

static DWORD WINAPI k_GetFileType(HANDLE h) { int fd = handle_fd(h); return fd >= 0 && fd <= 2 ? 2 : 1; }   /* char device / disk */
static BOOL WINAPI k_GetConsoleMode(HANDLE h, DWORD *mode) { if (mode) *mode = 3; return handle_fd(h) <= 2; }
static BOOL WINAPI k_SetConsoleMode(HANDLE h, DWORD mode) { return 1; }
static DWORD WINAPI k_GetConsoleOutputCP(void) { return 437; }
static BOOL WINAPI k_SetConsoleOutputCP(DWORD cp) { return 1; }
static BOOL WINAPI k_SetConsoleTitleA(const char *t) { return 1; }
static DWORD WINAPI k_GetACP(void) { return 1252; }

static void WINAPI k_Sleep(DWORD ms) { msleep(ms); }
static DWORD WINAPI k_GetTickCount(void) { return uptime() * 10; }
static u64 WINAPI k_GetTickCount64(void) { return (u64)uptime() * 10; }
static BOOL WINAPI k_QueryPerformanceCounter(i64 *v) { *v = (i64)uptime() * 10000; return 1; }
static BOOL WINAPI k_QueryPerformanceFrequency(i64 *v) { *v = 1000000; return 1; }
static DWORD WINAPI k_GetCurrentProcessId(void) { return getpid(); }
static DWORD WINAPI k_GetCurrentThreadId(void) { return getpid(); }
static HANDLE WINAPI k_GetCurrentProcess(void) { return (HANDLE)(i64)-1; }
static HANDLE WINAPI k_GetCurrentThread(void) { return (HANDLE)(i64)-2; }
static int WINAPI k_lstrlenA(const char *s) { return s ? strlen(s) : 0; }
static BOOL WINAPI k_IsDebuggerPresent(void) { return 0; }
static void *WINAPI k_SetUnhandledExceptionFilter(void *f) { return NULL; }
static void WINAPI k_CriticalSection(void *cs) {}

static BOOL WINAPI k_CloseHandle(HANDLE h)
{
    int fd = handle_fd(h);
    if (fd > 2) close(fd);
    return fd >= 0;
}

static HANDLE WINAPI k_CreateFileA(const char *name, DWORD access, DWORD share, void *sec, DWORD disp, DWORD flags, HANDLE tmpl)
{
    char path[PATH_MAX];
    unix_path(path, sizeof path, name);
    int mode = access & 0x40000000 ? O_WRONLY : O_RDONLY;
    struct stat st;
    int exists = stat(path, &st) == 0;
    if (disp == 1 && exists) { last_error = 80; return H_INVALID; }               /* CREATE_NEW */
    if ((disp == 3 || disp == 5) && !exists) { last_error = 2; return H_INVALID; }  /* OPEN_EXISTING / TRUNCATE_EXISTING */
    if (disp == 1 || disp == 2 || (disp == 4 && !exists))
        mode |= O_WRONLY | O_CREAT | (disp == 2 ? O_TRUNC : 0);
    if (disp == 5) mode |= O_WRONLY | O_TRUNC;
    int fd = open(path, mode);
    if (fd < 0) { last_error = 2; return H_INVALID; }
    return fd_handle(fd);
}

static DWORD WINAPI k_GetFileSize(HANDLE h, DWORD *high)
{
    struct stat st;
    if (high) *high = 0;
    int fd = handle_fd(h);
    if (fd < 0 || fstat(fd, &st) < 0) return (DWORD)-1;
    return st.size;
}

static DWORD WINAPI k_SetFilePointer(HANDLE h, i32 lo, i32 *hi, DWORD method)
{
    int fd = handle_fd(h);
    int r = fd < 0 ? -1 : lseek(fd, lo, method == 0 ? 0 : method == 1 ? 1 : 2);
    return r < 0 ? (DWORD)-1 : (DWORD)r;
}

static BOOL WINAPI k_FlushFileBuffers(HANDLE h) { return 1; }
static BOOL WINAPI k_DeleteFileA(const char *name) { char p[PATH_MAX]; unix_path(p, sizeof p, name); return unlink(p) == 0; }

struct systime { u16 year, month, dow, day, hour, minute, second, ms; };

static void WINAPI k_GetSystemTime(struct systime *t)
{
    struct rtc_time r;
    gettime(&r);
    t->year = r.year; t->month = r.month; t->dow = 0; t->day = r.day; t->hour = r.hour; t->minute = r.min; t->second = r.sec; t->ms = 0;
}

static int WINAPI k_MultiByteToWideChar(DWORD cp, DWORD fl, const char *s, int n, u16 *out, int cap)
{
    if (n < 0) n = strlen(s) + 1;
    if (cap == 0) return n;
    for (int i = 0; i < n && i < cap; i++) out[i] = (u8)s[i];
    return n < cap ? n : cap;
}

static int WINAPI k_WideCharToMultiByte(DWORD cp, DWORD fl, const u16 *s, int n, char *out, int cap, const char *def, BOOL *used)
{
    if (n < 0) { n = 0; while (s[n]) n++; n++; }
    if (cap == 0) return n;
    for (int i = 0; i < n && i < cap; i++) out[i] = s[i] < 128 ? (char)s[i] : '?';
    return n < cap ? n : cap;
}

static DWORD WINAPI k_GetEnvironmentVariableA(const char *name, char *buf, DWORD n) { last_error = 203; return 0; }
static void WINAPI k_GetStartupInfoA(void *si) { memset(si, 0, 104); *(DWORD *)si = 104; }
static int WINAPI u_MessageBoxA(HANDLE wnd, const char *text, const char *caption, DWORD type)
{
    printf("[%s] %s\n", caption ? caption : "Message", text ? text : "");
    return 1;                                              /* IDOK */
}

/* ------------------------------------------------------------------ the C runtime */

struct cfile { int fd; int eof; };
static struct cfile std_files[3] = {{0, 0}, {1, 0}, {2, 0}};

static struct cfile *WINAPI c_iob_func(DWORD i) { return &std_files[i < 3 ? i : 0]; }
static struct cfile *WINAPI c_iob_array(void) { return std_files; }

static int WINAPI c_printf(const char *fmt, ...)
{
    ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int n = out_fmt(1, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_fprintf(struct cfile *f, const char *fmt, ...)
{
    ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int n = out_fmt(f->fd, fmt, ap);
    __builtin_ms_va_end(ap);
    return n;
}

static int WINAPI c_sprintf(char *buf, const char *fmt, ...)
{
    ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int n = ms_vsnprintf(buf, 1 << 30, fmt, ap);
    __builtin_ms_va_end(ap);
    if (n >= 0) buf[n] = 0;
    return n;
}

static int WINAPI c_snprintf(char *buf, u64 cap, const char *fmt, ...)
{
    ms_va_list ap;
    __builtin_ms_va_start(ap, fmt);
    int n = ms_vsnprintf(buf, (int)cap, fmt, ap);
    __builtin_ms_va_end(ap);
    if (cap) buf[n < (int)cap ? n : (int)cap - 1] = 0;
    return n;
}

static int WINAPI c_puts(const char *s) { out_text(1, s, strlen(s)); out_text(1, "\n", 1); return 1; }
static int WINAPI c_putchar(int c) { char ch = c; out_text(1, &ch, 1); return c; }
static int WINAPI c_getchar(void) { u8 c; return read(0, &c, 1) == 1 ? c : -1; }
static int WINAPI c_fputs(const char *s, struct cfile *f) { return out_text(f->fd, s, strlen(s)) < 0 ? -1 : 1; }
static int WINAPI c_fputc(int c, struct cfile *f) { char ch = c; return out_text(f->fd, &ch, 1) < 0 ? -1 : c; }
static u64 WINAPI c_fwrite(const void *p, u64 size, u64 n, struct cfile *f) { int w = out_text(f->fd, p, size * n); return w < 0 || !size ? 0 : w / size; }
static u64 WINAPI c_fread(void *p, u64 size, u64 n, struct cfile *f)
{
    int r = read(f->fd, p, size * n);
    if (r <= 0) { f->eof = 1; return 0; }
    return size ? r / size : 0;
}
static char *WINAPI c_fgets(char *buf, int n, struct cfile *f)
{
    int i = 0;
    while (i < n - 1) {
        u8 c;
        if (read(f->fd, &c, 1) != 1) { f->eof = 1; break; }
        buf[i++] = c;
        if (c == '\n') break;
    }
    buf[i] = 0;
    return i ? buf : NULL;
}
static int WINAPI c_fflush(struct cfile *f) { return 0; }
static int WINAPI c_feof(struct cfile *f) { return f->eof; }

static struct cfile *WINAPI c_fopen(const char *name, const char *mode)
{
    char path[PATH_MAX];
    unix_path(path, sizeof path, name);
    int flags = mode[0] == 'r' ? O_RDONLY : O_WRONLY | O_CREAT | (mode[0] == 'w' ? O_TRUNC : 0);
    int fd = open(path, flags);
    if (fd < 0) return NULL;
    struct cfile *f = malloc(sizeof *f);
    f->fd = fd;
    f->eof = 0;
    return f;
}

static int WINAPI c_fclose(struct cfile *f) { if (f->fd > 2) { close(f->fd); free(f); } return 0; }

static u64 WINAPI c_strlen(const char *s) { return strlen(s); }
static int WINAPI c_strcmp(const char *a, const char *b) { return strcmp(a, b); }
static int WINAPI c_strncmp(const char *a, const char *b, u64 n) { return strncmp(a, b, n); }
static char *WINAPI c_strcpy(char *d, const char *s) { return strcpy(d, s); }
static char *WINAPI c_strncpy(char *d, const char *s, u64 n) { return strncpy(d, s, n); }
static char *WINAPI c_strcat(char *d, const char *s) { return strcat(d, s); }
static char *WINAPI c_strchr(const char *s, int c) { return strchr(s, c); }
static char *WINAPI c_strrchr(const char *s, int c) { return strrchr(s, c); }
static char *WINAPI c_strstr(const char *a, const char *b) { return strstr(a, b); }
static void *WINAPI c_memcpy(void *d, const void *s, u64 n) { return memcpy(d, s, n); }
static void *WINAPI c_memmove(void *d, const void *s, u64 n) { return memmove(d, s, n); }
static void *WINAPI c_memset(void *d, int c, u64 n) { return memset(d, c, n); }
static int WINAPI c_memcmp(const void *a, const void *b, u64 n) { return memcmp(a, b, n); }
static void *WINAPI c_malloc(u64 n) { return malloc(n ? n : 1); }
static void *WINAPI c_calloc(u64 n, u64 m) { return calloc(n ? n : 1, m ? m : 1); }
static void *WINAPI c_realloc(void *p, u64 n) { return realloc(p, n ? n : 1); }
static void WINAPI c_free(void *p) { free(p); }
static void WINAPI c_exit(int code) { exit(code); }
static void WINAPI c_abort(void) { fprintf(2, "grape: the program called abort()\n"); exit(3); }
static int WINAPI c_atoi(const char *s) { return atoi(s); }
static int WINAPI c_abs(int v) { return v < 0 ? -v : v; }
static int WINAPI c_toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }
static int WINAPI c_tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
static int WINAPI c_isdigit(int c) { return c >= '0' && c <= '9'; }
static int WINAPI c_isalpha(int c) { return (c | 32) >= 'a' && (c | 32) <= 'z'; }
static int WINAPI c_isspace(int c) { return c == ' ' || (c >= 9 && c <= 13); }
static unsigned rng = 1;
static void WINAPI c_srand(unsigned s) { rng = s; }
static int WINAPI c_rand(void) { rng = rng * 1103515245 + 12345; return (rng >> 16) & 0x7fff; }

/* ------------------------------------------------------------------ imports */

struct imp {
    const char *name;
    void *fn;
};

#define K(n) {#n, (void *)k_##n}
#define C(n) {#n, (void *)c_##n}

static const struct imp imports[] = {
    K(GetStdHandle), K(WriteFile), K(ReadFile), K(WriteConsoleA), K(WriteConsoleW), K(ReadConsoleA), K(ExitProcess),
    K(TerminateProcess), K(GetCommandLineA), K(GetCommandLineW), K(GetLastError), K(SetLastError), K(GetModuleHandleA),
    K(GetModuleHandleW), K(GetProcAddress), K(LoadLibraryA), K(FreeLibrary), K(VirtualAlloc), K(VirtualFree),
    K(VirtualProtect), K(GetProcessHeap), K(HeapCreate), K(HeapAlloc), K(HeapFree), K(HeapReAlloc), K(GetFileType),
    K(GetConsoleMode), K(SetConsoleMode), K(GetConsoleOutputCP), K(SetConsoleOutputCP), K(SetConsoleTitleA), K(GetACP),
    K(Sleep), K(GetTickCount), K(GetTickCount64), K(QueryPerformanceCounter), K(QueryPerformanceFrequency),
    K(GetCurrentProcessId), K(GetCurrentThreadId), K(GetCurrentProcess), K(GetCurrentThread), K(lstrlenA),
    K(IsDebuggerPresent), K(SetUnhandledExceptionFilter), K(CloseHandle), K(CreateFileA), K(GetFileSize),
    K(SetFilePointer), K(FlushFileBuffers), K(DeleteFileA), K(GetSystemTime), K(MultiByteToWideChar),
    K(WideCharToMultiByte), K(GetEnvironmentVariableA), K(GetStartupInfoA),
    {"GetLocalTime", (void *)k_GetSystemTime},
    {"InitializeCriticalSection", (void *)k_CriticalSection}, {"EnterCriticalSection", (void *)k_CriticalSection},
    {"LeaveCriticalSection", (void *)k_CriticalSection}, {"DeleteCriticalSection", (void *)k_CriticalSection},
    {"MessageBoxA", (void *)u_MessageBoxA},
    C(printf), C(fprintf), C(sprintf), C(puts), C(putchar), C(getchar), C(fputs), C(fputc), C(fwrite), C(fread),
    C(fgets), C(fflush), C(feof), C(fopen), C(fclose), C(strlen), C(strcmp), C(strncmp), C(strcpy), C(strncpy),
    C(strcat), C(strchr), C(strrchr), C(strstr), C(memcpy), C(memmove), C(memset), C(memcmp), C(malloc), C(calloc),
    C(realloc), C(free), C(exit), C(abort), C(atoi), C(abs), C(toupper), C(tolower), C(isdigit), C(isalpha),
    C(isspace), C(srand), C(rand),
    {"_snprintf", (void *)c_snprintf}, {"snprintf", (void *)c_snprintf}, {"__acrt_iob_func", (void *)c_iob_func},
    {"__iob_func", (void *)c_iob_array}, {"_exit", (void *)c_exit},
};

static void *lookup_import(const char *name)
{
    for (u32 i = 0; i < sizeof imports / sizeof imports[0]; i++)
        if (!strcmp(imports[i].name, name))
            return imports[i].fn;
    return NULL;
}

static void WINAPI unimplemented(const char *name)
{
    fprintf(2, "\ngrape: the program called %s, which GrapeJuice doesn't implement yet\n", name);
    exit(127);
}

/* A tiny trampoline per unresolved import: mov rcx, name; mov rax, unimplemented; jmp rax */
static void *make_stub(const char *name)
{
    u8 *s = malloc(32);
    if (!s)
        die("out of memory", NULL);
    s[0] = 0x48; s[1] = 0xB9;
    u64 a = (u64)name, b = (u64)unimplemented;
    for (int i = 0; i < 8; i++) s[2 + i] = a >> (8 * i);
    s[10] = 0x48; s[11] = 0xB8;
    for (int i = 0; i < 8; i++) s[12 + i] = b >> (8 * i);
    s[20] = 0xFF; s[21] = 0xE0;
    return s;
}

/* ------------------------------------------------------------------ the loader */

static u8 *slurp_file(const char *path, u32 *len)
{
    struct stat st;
    int fd = open(path, O_RDONLY);
    if (fd < 0 || fstat(fd, &st) < 0)
        die("can't open", path);
    u8 *buf = malloc(st.size + 1);
    if (!buf)
        die("out of memory", NULL);
    u32 got = 0;
    while (got < st.size) {
        int r = read(fd, buf + got, st.size - got);
        if (r <= 0)
            die("can't read", path);
        got += r;
    }
    close(fd);
    *len = st.size;
    return buf;
}

static int run_pe(const char *path, int argc, char **argv)
{
    u32 len;
    u8 *f = slurp_file(path, &len);
    if (len < 0x40 || f[0] != 'M' || f[1] != 'Z')
        die("not a Windows program (no MZ header)", path);
    u32 pe_off = rd32(f + 0x3C);
    if (pe_off + 24 > len || memcmp(f + pe_off, "PE\0\0", 4))
        die("not a Windows program (no PE header)", path);
    const u8 *coff = f + pe_off + 4;
    u16 machine = rd16(coff), nsec = rd16(coff + 2), optsize = rd16(coff + 16);
    const u8 *opt = coff + 20;
    if (machine == 0x14C)
        die("this is a 32-bit Windows program; GrapeJuice runs only 64-bit ones", path);
    if (machine != 0x8664 || rd16(opt) != 0x20B)
        die("unsupported program (not x86-64 PE32+)", path);
    u32 entry = rd32(opt + 16), size_of_image = rd32(opt + 56), size_of_headers = rd32(opt + 60);
    u64 pref_base = rd64(opt + 24);
    u16 subsystem = rd16(opt + 68);
    u32 ndirs = rd32(opt + 108);
    const u8 *dirs = opt + 112;
    if (!size_of_image || size_of_image > 256u * 1024 * 1024)
        die("bad image size", path);

    u8 *raw = calloc(1, size_of_image + 4096);
    if (!raw)
        die("out of memory for the program image", NULL);
    u8 *base = (u8 *)(((u64)raw + 4095) & ~4095ull);
    image_base = base;
    memcpy(base, f, size_of_headers < len ? size_of_headers : len);
    const u8 *sec = opt + optsize;
    for (int i = 0; i < nsec; i++, sec += 40) {
        u32 vsize = rd32(sec + 8), va = rd32(sec + 12), rsize = rd32(sec + 16), roff = rd32(sec + 20);
        if (va + vsize > size_of_image || roff + rsize > len)
            die("corrupt section table", path);
        memcpy(base + va, f + roff, rsize < vsize || !vsize ? rsize : vsize);
    }

    i64 delta = (i64)(u64)base - (i64)pref_base;                         /* relocations */
    if (delta && ndirs > 5 && rd32(dirs + 5 * 8 + 4)) {
        u32 rva = rd32(dirs + 5 * 8), size = rd32(dirs + 5 * 8 + 4), pos = 0;
        while (pos + 8 <= size) {
            u32 page = rd32(base + rva + pos), blk = rd32(base + rva + pos + 4);
            if (blk < 8)
                break;
            for (u32 e = 8; e + 2 <= blk; e += 2) {
                u16 ent = rd16(base + rva + pos + e);
                u8 *where = base + page + (ent & 0xFFF);
                if ((ent >> 12) == 10) { u64 v = rd64(where) + delta; for (int k = 0; k < 8; k++) where[k] = v >> (8 * k); }
                else if ((ent >> 12) == 3) { u32 v = rd32(where) + (u32)delta; for (int k = 0; k < 4; k++) where[k] = v >> (8 * k); }
            }
            pos += blk;
        }
    } else if (delta && verbose) {
        fprintf(2, "grape: the program has no relocation table; fine if it is position independent\n");
    }

    if (ndirs > 1 && rd32(dirs + 8)) {                                   /* imports */
        const u8 *d = base + rd32(dirs + 8);
        for (; rd32(d + 12); d += 20) {
            const char *dll = (const char *)base + rd32(d + 12);
            u32 thunk_rva = rd32(d) ? rd32(d) : rd32(d + 16);
            u8 *iat = base + rd32(d + 16);
            const u8 *names = base + thunk_rva;
            for (; rd64(names); names += 8, iat += 8) {
                u64 ent = rd64(names);
                const char *name;
                char ord[24];
                if (ent >> 63) {
                    snprintf(ord, sizeof ord, "ordinal %u", (u32)(ent & 0xFFFF));
                    name = strcpy(malloc(strlen(ord) + 1), ord);
                } else {
                    name = (const char *)base + (u32)ent + 2;
                }
                void *fn = ent >> 63 ? NULL : lookup_import(name);
                if (!fn) {
                    if (verbose) fprintf(2, "grape: %s: %s is not implemented\n", dll, name);
                    fn = make_stub(name);
                } else if (verbose) {
                    fprintf(2, "grape: %s: %s\n", dll, name);
                }
                u64 v = (u64)fn;
                for (int k = 0; k < 8; k++) iat[k] = v >> (8 * k);
            }
        }
    }

    if (subsystem == 2 && verbose)
        fprintf(2, "grape: this is a graphical program; windows are not supported, only its text output works\n");

    char *cl = malloc(1024);                                             /* the command line */
    int n = 0;
    for (int i = 0; i < argc; i++)
        n += snprintf(cl + n, 1024 - n, "%s%s", i ? " " : "", argv[i]);
    cmdline = cl;

    typedef int (WINAPI *entry_fn)(long argc, char **argv);
    entry_fn start = (entry_fn)(base + entry);
    if (verbose)
        fprintf(2, "grape: running %s (image at %p, entry %p)\n", path, base, base + entry);
    return start(argc, argv);
}

int main(int argc, char **argv)
{
    int i = 1;
    if (i < argc && !strcmp(argv[i], "-v")) {
        verbose = 1;
        i++;
    }
    if (i >= argc) {
        fprintf(2, "GrapeJuice 0.1: run 64-bit Windows console programs\nusage: grape [-v] program.exe [arguments...]\n");
        return 2;
    }
    return run_pe(argv[i], argc - i, argv + i);
}
