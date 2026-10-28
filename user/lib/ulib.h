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
int read_line(int fd, char *buf, int max);   /* returns length, or -1 at EOF */
