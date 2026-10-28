# Petrichor OS

*petrichor* (n.): the smell of rain on dry earth.

A small Unix-like operating system written from scratch for 32-bit x86. No Linux, no BSD,
no borrowed kernel or libc: the kernel, C library, shell and utilities are all in this repo.

```
  ___    _       _      _                 ___  ___
 | _ \__| |_ _ _(_)__| |_  ___ _ _      / _ \/ __|
 |  _/ -_)  _| '_| / _| ' \/ _ \ '_|    | (_) \__ \
 |_| \___|\__|_| |_\__|_||_\___/_|       \___/|___/
```

## Quick start (macOS or Linux host)

```sh
brew install lld qemu     # plus clang (Xcode command line tools)
make run                  # opens a QEMU window
make run-nographic        # serial console in your terminal (Ctrl-A X quits)
```

Then try: `help`, `ls /bin`, `selftest`, `ps`, `free`, `cat /etc/motd | wc`, `sleep 5 &`.
`halt` powers the VM off.

## What's in it

**Kernel** (`kernel/`, ~2,700 lines of C and assembly)

| Area | Details |
| --- | --- |
| Boot | Multiboot entry, GDT with ring 0/3 segments and a TSS |
| Interrupts | IDT, remapped 8259 PIC, 100 Hz PIT, `int 0x80` syscalls |
| Memory | Physical frame bitmap, first-fit kernel heap, 2-level paging, per-process address spaces |
| Processes | `fork`, `exec` (ELF32), `wait`, `exit`, `kill`, process groups, preemptive round-robin scheduler for user code, sleep/wakeup |
| Filesystem | In-memory Unix filesystem: files, directories (`.`/`..`), `rename`, `unlink`, `/dev` nodes; root image unpacked from an initrd linked into the kernel |
| I/O | Pipes, VGA text console with ANSI colors, 16550 serial, PS/2 keyboard, a cooked tty with line editing, `^C`, `^D` |
| Isolation | User pointers are validated; faults and wild pointers kill only the offending process |

**Userland** (`user/`): a tiny libc (`printf`, `malloc`, string functions, syscall stubs),
`init`, and `sh` (pipes, `<` `>` `>>`, `;`, `&`, quoting, scripts, `cd`), plus
`ls cat echo mkdir rm touch cp mv pwd ps uname uptime clear kill sleep wc grep head free halt reboot hello selftest`.

## Layout

```
kernel/     the kernel (boot.S, isr.S, console, mem, proc, fs, syscall, ...)
lib/        string and printf code shared by kernel and userland
include/    abi.h: the syscall/errno/struct contract between kernel and user
user/       libc (user/lib) and programs (user/bin)
rootfs/     files copied into the root filesystem (/etc/motd, ...)
tools/      mkinitrd: packs the root filesystem image (host tool)
```

## Design notes

- **Memory map.** The kernel lives at 1 MiB and all RAM is identity-mapped (supervisor-only) in
  every address space, so the kernel can reach any physical frame directly. User programs are
  linked at `0x40000000`; the stack tops out at `0xC0000000`.
- **Concurrency model.** The kernel runs with interrupts off and is not preemptible; only user
  code is preempted. This keeps the kernel free of locks. Blocking calls sleep on a channel.
- **System calls.** `eax` = number, `ebx ecx edx esi edi` = arguments, result in `eax`, negative
  values are `-errno`. See `include/abi.h`.
- **Storage.** The filesystem is RAM-only: changes are lost on reboot. `/bin` and `/etc` are
  rebuilt from the initrd each boot.

## Testing

`selftest` (run inside Petrichor) performs 50 end-to-end checks of files, directories, devices,
fork/exec/wait/kill, pipes (including blocking on a full pipe), the heap, and a physical-memory
leak check across repeated fork/exit.

## Limits and ideas

Single CPU, no SMP. No disk driver or persistent filesystem yet, no networking, no signals
(only `kill` = terminate), no copy-on-write `fork`, and 512 MiB of RAM max. Natural next steps:
an ATA driver with an on-disk filesystem, signals, raw tty mode and a text editor, a 64-bit port.
