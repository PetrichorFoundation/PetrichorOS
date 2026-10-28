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
#define SYS_diskinfo  33
#define SYS_socket    34
#define SYS_connect   35
#define SYS_bind      36
#define SYS_sendto    37
#define SYS_recvfrom  38
#define SYS_netinfo   39
#define SYS_ping      40
#define SYS_settimeout 41
#define SYS_poll      42
#define SYS_fbmap     43
#define SYS_gettime   44
#define SYS_drives    45

/* input events (read from /dev/input) */
#define INPUT_KEY   1
#define INPUT_MOUSE 2
#define INPUT_WHEEL 3           /* dy = +1 scroll down, -1 scroll up */
#define IEF_DOWN    0x01        /* key: pressed (else released) */
#define IEF_SHIFT   0x02
#define IEF_CTRL    0x04
#define IEF_ALT     0x08
#define BTN_LEFT    1
#define BTN_RIGHT   2
#define BTN_MIDDLE  4
/* key codes: ASCII for printable keys, plus: */
#define KEY_UP      0x101
#define KEY_DOWN    0x102
#define KEY_LEFT    0x103
#define KEY_RIGHT   0x104
#define KEY_HOME    0x105
#define KEY_END     0x106
#define KEY_PGUP    0x107
#define KEY_PGDN    0x108
#define KEY_DELETE  0x109
#define KEY_INSERT  0x10A
#define KEY_F1      0x111   /* F1..F10 are consecutive */

#define POLLIN   1
#define POLLHUP  0x10
#define POLLNVAL 0x20

#define SOCK_DGRAM  1
#define SOCK_STREAM 2

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
#define T_SOCK 5

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
#define EBUSY        16
#define EEXIST       17
#define EXDEV        18
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
#define ENOTSOCK     88
#define EADDRINUSE   98
#define ENETDOWN     100
#define ECONNRESET   104
#define EISCONN      106
#define ENOTCONN     107
#define ETIMEDOUT    110
#define ECONNREFUSED 111
#define EHOSTUNREACH 113
#define EMSGSIZE     90

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

struct input_event {
    u8 type;
    u8 flags;
    u16 code;                   /* key code, or mouse button mask */
    i16 dx, dy;                 /* mouse motion (y grows downward) */
};

struct driveinfo {
    u32 sectors;                /* 512-byte sectors */
    u8 index, boot, pad[2];     /* boot: the disk this system booted from and mounts as /disk */
    char name[8];               /* "hda".. */
    char model[40];
};

struct pollfd {
    int fd;
    short events;
    short revents;
};

struct fbinfo {
    u32 addr;                   /* user virtual address of the framebuffer */
    u32 width, height, pitch;   /* pitch in bytes; pixels are 0x00RRGGBB */
};

struct rtc_time {
    u16 year;
    u8 month, day, hour, min, sec, pad;
};

/* IPv4 addresses are u32 in host order: 10.0.2.15 == 0x0A00020F. */
struct sockaddr_in {
    u32 ip;
    u16 port;
    u16 pad;
};

struct netinfo {
    u8 mac[6];
    u8 up, pad;
    u32 ip, mask, gw, dns;
    u32 rx_packets, tx_packets;
};

struct diskinfo {
    u32 mounted;
    u32 total_kb;
    u32 free_kb;
};

struct meminfo {
    u32 total_kb;
    u32 free_kb;
    u32 heap_total_kb;
    u32 heap_free_kb;
};
#endif
