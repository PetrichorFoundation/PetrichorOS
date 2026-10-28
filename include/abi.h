/* The kernel <-> userland contract: syscall numbers, errno, shared structs. */
#pragma once

#define PETRICHOR_NAME    "Petrichor"
#define PETRICHOR_VERSION "0.1.0"

#define SYS_exit      1
#define SYS_fork      2
#define SYS_read      3
#define SYS_write     4
#define SYS_open      5
#define SYS_close     6
#define SYS_wait      7
#define SYS_exec      8
#define SYS_getpid    9
#define SYS_sbrk      10
#define SYS_sleep     11
#define SYS_pipe      12
#define SYS_dup       13
#define SYS_dup2      14
#define SYS_chdir     15
#define SYS_getcwd    16
#define SYS_mkdir     17
#define SYS_unlink    18
#define SYS_fstat     19
#define SYS_stat      20
#define SYS_readdir   21
#define SYS_kill      22
#define SYS_uptime    23
#define SYS_lseek     24
#define SYS_getppid   25
#define SYS_ps        26
#define SYS_rename    27
#define SYS_setpgid   28
#define SYS_getpgid   29
#define SYS_tcsetpgrp 30
#define SYS_meminfo   31
#define SYS_shutdown  32

/* open() flags */
#define O_RDONLY 0x000
#define O_WRONLY 0x001
#define O_RDWR   0x002
#define O_CREAT  0x100
#define O_TRUNC  0x200
#define O_APPEND 0x400

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define WNOHANG 1

/* shutdown() modes */
#define SHUT_POWEROFF 0
#define SHUT_REBOOT   1

/* inode types */
#define T_FILE 1
#define T_DIR  2
#define T_DEV  3
#define T_PIPE 4

/* process states (as reported by ps) */
#define PS_UNUSED   0
#define PS_EMBRYO   1
#define PS_SLEEPING 2
#define PS_RUNNABLE 3
#define PS_RUNNING  4
#define PS_ZOMBIE   5

/* errno values; syscalls return -E... on failure */
#define EPERM        1
#define ENOENT       2
#define ESRCH        3
#define EINTR        4
#define EIO          5
#define E2BIG        7
#define ENOEXEC      8
#define EBADF        9
#define ECHILD       10
#define EAGAIN       11
#define ENOMEM       12
#define EACCES       13
#define EFAULT       14
#define EEXIST       17
#define ENODEV       19
#define ENOTDIR      20
#define EISDIR       21
#define EINVAL       22
#define EMFILE       24
#define EFBIG        27
#define ENOSPC       28
#define ESPIPE       29
#define EPIPE        32
#define ENAMETOOLONG 36
#define ENOSYS       38
#define ENOTEMPTY    39

#ifndef __ASSEMBLER__
#include "types.h"

#define NAME_MAX 27
#define PATH_MAX 256

struct stat {
    u32 type;
    u32 size;
    u32 nlink;
    u32 ino;
};

struct dirent {
    u32 ino;
    u32 type;
    char name[NAME_MAX + 1];
};

struct procinfo {
    int pid;
    int ppid;
    int state;
    u32 mem_kb;
    u32 ticks;
    char name[16];
};

struct meminfo {
    u32 total_kb;
    u32 free_kb;
    u32 heap_total_kb;
    u32 heap_free_kb;
};
#endif
