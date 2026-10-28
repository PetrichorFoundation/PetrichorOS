#pragma once
#include "types.h"
#include "abi.h"
#include "string.h"

/* system calls (user/lib/usys.S); failures return -errno */
void exit(int status) __attribute__((noreturn));
int fork(void);
int read(int fd, void *buf, u32 n);
int write(int fd, const void *buf, u32 n);
int open(const char *path, int flags);
int close(int fd);
int wait2(int *status, int flags);
int exec(const char *path, char **argv);
int getpid(void);
void *sbrk(int n);
int msleep(u32 ms);
int pipe(int fds[2]);
int dup(int fd);
int dup2(int from, int to);
int chdir(const char *path);
int getcwd(char *buf, u32 size);
int mkdir(const char *path);
int unlink(const char *path);
int fstat(int fd, struct stat *st);
int stat(const char *path, struct stat *st);
int readdir(int fd, struct dirent *de);
int kill(int pid, int sig);
u32 uptime(void);
int lseek(int fd, int off, int whence);
int getppid(void);
int ps(struct procinfo *buf, int max);
int rename(const char *from, const char *to);
int setpgid(int pid, int pgid);
int getpgid(int pid);
int tcsetpgrp(int pgid);
int meminfo(struct meminfo *mi);
int shutdown(int mode);
int diskinfo(struct diskinfo *di);
int socket(int type);
int connect(int fd, u32 ip, u16 port);
int bind(int fd, u16 port);
int sendto(int fd, const void *buf, u32 n, u32 ip, u16 port);
int recvfrom(int fd, void *buf, u32 n, struct sockaddr_in *from);
int netinfo(struct netinfo *ni);
int ping(u32 ip, u32 timeout_ms);            /* round-trip time in ticks (10 ms) */
int settimeout(int fd, u32 ms);
int poll(struct pollfd *fds, int n, int timeout_ms);   /* timeout < 0: wait forever */
int fbmap(struct fbinfo *fb, u32 w, u32 h);            /* fb == NULL: back to text mode */
int gettime(struct rtc_time *t);
int drives(struct driveinfo *out, int max);          /* number of disks found */

/* libc-ish helpers (user/lib/ulib.c) */
int wait(int *status);
int printf(const char *fmt, ...);
int fprintf(int fd, const char *fmt, ...);
int snprintf(char *buf, u32 size, const char *fmt, ...);
int vfprintf(int fd, const char *fmt, va_list ap);
void puts(const char *s);
void fputs(int fd, const char *s);
int atoi(const char *s);
const char *strerror(int err);
void *malloc(u32 n);
void *calloc(u32 n, u32 size);
void *realloc(void *p, u32 n);
void free(void *p);
/* networking helpers (user/lib/net.c); IPv4 addresses are host-order u32 */
int inet_aton(const char *s, u32 *ip);
const char *inet_ntoa(u32 ip);
int resolve(const char *name, u32 *ip);
int url_split(const char *url, char *host, int hostsz, int *port, const char **path);
int http_get(const char *url, char **body, int *len);   /* body is malloc'd, NUL-terminated */
void url_resolve(const char *base, const char *ref, char *out, int outsz);   /* ref relative to base, "../" aware */
int read_line(int fd, char *buf, int max);   /* returns length, or -1 at EOF */
