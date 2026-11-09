# System call stubs.  The C calling convention already puts the arguments in rdi, rsi, rdx, rcx, r8;
# the kernel wants the fourth in r10 (rcx is destroyed by `syscall`-style ABIs).  The number goes in
# rax, the result (or -errno) comes back in rax.
#include "abi.h"

.macro SYSCALL name, num
.global \name
\name:
    mov %rcx, %r10
    mov $\num, %eax
    int $0x80
    ret
.endm

.text
SYSCALL exit, SYS_exit
SYSCALL fork, SYS_fork
SYSCALL read, SYS_read
SYSCALL write, SYS_write
SYSCALL open, SYS_open
SYSCALL close, SYS_close
SYSCALL wait2, SYS_wait
SYSCALL exec, SYS_exec
SYSCALL getpid, SYS_getpid
SYSCALL sbrk, SYS_sbrk
SYSCALL msleep, SYS_sleep
SYSCALL pipe, SYS_pipe
SYSCALL dup, SYS_dup
SYSCALL dup2, SYS_dup2
SYSCALL chdir, SYS_chdir
SYSCALL getcwd, SYS_getcwd
SYSCALL mkdir, SYS_mkdir
SYSCALL unlink, SYS_unlink
SYSCALL fstat, SYS_fstat
SYSCALL stat, SYS_stat
SYSCALL readdir, SYS_readdir
SYSCALL kill, SYS_kill
SYSCALL uptime, SYS_uptime
SYSCALL lseek, SYS_lseek
SYSCALL getppid, SYS_getppid
SYSCALL ps, SYS_ps
SYSCALL rename, SYS_rename
SYSCALL setpgid, SYS_setpgid
SYSCALL getpgid, SYS_getpgid
SYSCALL tcsetpgrp, SYS_tcsetpgrp
SYSCALL meminfo, SYS_meminfo
SYSCALL shutdown, SYS_shutdown
SYSCALL diskinfo, SYS_diskinfo
SYSCALL socket, SYS_socket
SYSCALL connect, SYS_connect
SYSCALL bind, SYS_bind
SYSCALL sendto, SYS_sendto
SYSCALL recvfrom, SYS_recvfrom
SYSCALL netinfo, SYS_netinfo
SYSCALL ping, SYS_ping
SYSCALL settimeout, SYS_settimeout
SYSCALL poll, SYS_poll
SYSCALL fbmap, SYS_fbmap
SYSCALL gettime, SYS_gettime
SYSCALL drives, SYS_drives
SYSCALL shm, SYS_shm
SYSCALL __mmap, SYS_mmap
SYSCALL __munmap, SYS_munmap
SYSCALL __mprotect, SYS_mprotect
SYSCALL __clone, SYS_clone
SYSCALL futex, SYS_futex
SYSCALL settls, SYS_settls
SYSCALL gettid, SYS_gettid
