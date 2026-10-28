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
make run                  # boots build/disk.img through Petrichor's own bootloader
make run-nographic        # same, with the serial console in your terminal (Ctrl-A X quits)
make run-multiboot        # skip the bootloader: QEMU loads the kernel directly
```

Then try: `help`, `ls /bin`, `selftest`, `df`, `ifconfig`, `ping 10.0.2.2`,
`nslookup example.com`, `wget http://example.com/`, and **`pdesk`** to start the desktop.
`halt` powers the VM off.

**Package manager demo.** In one terminal on the host run `make serve-repo` (serves `build/repo` on
port 8080), then inside Petrichor: `ppm update`, `ppm search`, `ppm install lolcow`, `lolcow`.

Files you put in `/disk` survive reboots (`make` updates the bootloader and kernel inside
`build/disk.img` but leaves the data partition alone; `make clean` deletes the disk).

## What's in it

**Bootloader** (`boot/`, from scratch, real-mode assembly). BIOS loads the 512-byte MBR, which
loads stage 2 with INT 13h extended reads. Stage 2 enables A20, asks the BIOS for the RAM size
(INT 15h/E801), copies the kernel above 1 MiB in 32 KiB chunks (INT 15h/87h), switches to
32-bit protected mode and jumps to the kernel using the Multiboot register convention.
`tools/mkdisk` lays it all out on the disk image.

**Kernel** (`kernel/`, ~5,000 lines of C and assembly)

| Area | Details |
| --- | --- |
| Boot | Multiboot entry (own bootloader or QEMU/GRUB), GDT with ring 0/3 segments and a TSS |
| Interrupts | IDT, remapped 8259 PIC, 100 Hz PIT, `int 0x80` syscalls |
| Memory | Physical frame bitmap, first-fit kernel heap, 2-level paging, per-process address spaces, shared device-memory mappings |
| Processes | `fork`, `exec` (ELF32 and `#!` scripts), `wait`, `exit`, `kill`, process groups, preemptive round-robin scheduler for user code, sleep/wakeup with timeouts, `poll` |
| Filesystems | A RAM filesystem for `/` (unpacked from an initrd) and **PFS**, an on-disk filesystem mounted at `/disk` (below) |
| Disk | ATA PIO driver (LBA28), 64-block write-through buffer cache |
| Network | PCI scan, RTL8139 driver, ARP, IPv4, ICMP, UDP, TCP client (below) |
| Graphics and input | Bochs/QEMU VBE linear framebuffer (1024x768x32), raw keyboard events (arrows, F-keys, modifiers), PS/2 mouse, `/dev/input` event stream, CMOS real-time clock |
| I/O | Pipes, sockets, VGA text console with ANSI colors, 16550 serial, PS/2 keyboard, a cooked tty with line editing, `^C`, `^D` |
| Isolation | User pointers are validated; faults and wild pointers kill only the offending process |

**PFS** has a superblock, inode and block bitmaps, an inode table, and 4 KiB blocks. Inodes have
12 direct blocks plus one indirect block (files up to ~4 MiB, 1024 inodes). Directories are files
of 32-byte entries. Every write goes straight to disk, so there is nothing to `sync`. The VFS
dispatches per inode, crosses mount points in both directions (including `..` and `getcwd`),
returns `EXDEV` for cross-device renames, and can `exec` programs stored on disk. The kernel
formats the partition on first boot.

**Networking** targets QEMU's user-mode network (`10.0.2.15`, gateway `10.0.2.2`, DNS
`10.0.2.3`, configured statically). Sockets are file descriptors: `socket`, `connect`, `bind`,
`sendto`, `recvfrom`, plus ordinary `read`/`write`. TCP is a client with stop-and-wait
transmission, retransmission with timeouts, and orderly close; there is no listen/accept yet.

**Desktop** (`user/apps/pdesk`, started with `pdesk`). Real XFCE can't run here (it needs X11, GTK,
glib, a POSIX libc, dynamic linking and threads), so Petrichor has its own XFCE-style desktop,
written from scratch on top of the framebuffer and input devices:

- window manager: draggable, resizable, minimizable, maximizable windows with shadows, Alt+Tab,
  Alt+F4, double-click-title maximize, damage-tracked repainting and a software mouse cursor
- panel with Applications menu, taskbar and clock; a floating dock; desktop icons
- apps: **Terminal** (runs `sh` over pipes, ANSI colors, scrollback, `^C`/`^D`), **File Manager**
  (places sidebar, new folder/file, delete, opens text files in the editor and programs in a
  terminal), **Text Editor** (line numbers, auto-indent, `Ctrl+S`), **System Monitor** (CPU
  history, memory/heap/disk bars, process table with End Process), and About
- needs QEMU's default VGA (`-vga std`); other graphics hardware isn't supported yet

**Package manager** (`ppm`). Packages are `.ppk` archives served over HTTP from a repository whose
`index` lists name, version, size, SHA-256 and dependencies. `ppm update | search | info |
install | remove | upgrade | list | files | repo`. Installs resolve dependencies first, verify size
and SHA-256 before writing anything, install under `/disk` (so they persist), refuse to remove a
package that others need, and the shell finds programs in `/bin` and `/disk/bin`. Host tools:
`tools/mkppk` builds packages; `make repo` builds the sample repository from `packages/`
(`cowsay`, `fortune`, `lolcow`, `calc`, `hd`); `make serve-repo` serves it.

**Userland** (`user/`): a tiny libc (`printf`, `malloc`, DNS resolver, HTTP client, SHA-256, a 2D
graphics library with an 8x16 font), `init`, and `sh` (pipes, `<` `>` `>>`, `;`, `&`, quoting,
scripts, `cd`), plus
`ls cat echo mkdir rm touch cp mv pwd ps uname uptime clear kill sleep wc grep head free df halt reboot hello selftest`,
the network tools `ifconfig ping nslookup wget nc`, and `ppm`.

## Layout

```
boot/       the bootloader: stage1 (MBR) and stage2
kernel/     the kernel (boot.S, isr.S, console, mem, proc, fs, pfs, ata, rtl8139, net, syscall, ...)
lib/        string and printf code shared by kernel and userland
include/    abi.h: the syscall/errno/struct contract between kernel and user
user/       libc (user/lib), programs (user/bin) and multi-file apps such as the desktop (user/apps)
packages/   sample packages for the repository (package metadata, sources, data files)
rootfs/     files copied into the root filesystem (/etc/motd, ...)
tools/      host tools: mkinitrd (root image), mkdisk (bootloader + kernel -> disk image), mkppk (packages)
```

## Design notes

- **Memory map.** The kernel lives at 1 MiB and all RAM is identity-mapped (supervisor-only) in
  every address space, so the kernel can reach any physical frame directly. User programs are
  linked at `0x40000000`; the stack tops out at `0xC0000000`.
- **Concurrency model.** The kernel runs with interrupts off and is not preemptible; only user
  code is preempted. This keeps the kernel free of locks. Blocking calls sleep on a channel.
- **System calls.** `eax` = number, `ebx ecx edx esi edi` = arguments, result in `eax`, negative
  values are `-errno`. See `include/abi.h`.
- **Storage.** `/` is a RAM filesystem rebuilt from the initrd each boot, so changes outside
  `/disk` are lost on reboot. `/disk` is persistent.
- **Disk image layout.** LBA 0 MBR, 1-15 stage 2, 16+ kernel image, 4096+ PFS partition.

## Testing

`selftest` (run inside Petrichor) performs 79 end-to-end checks of files, directories, the on-disk
filesystem (including a 100 KiB file across indirect blocks), devices, sockets, `#!` scripts,
`poll`, the clock, fork/exec/wait/kill, pipes (including blocking on a full pipe), the heap, and a
physical-memory leak check across repeated fork/exit. Checks that need a disk or NIC are skipped
if none is attached. The desktop and package manager were tested by driving QEMU's monitor
(injected mouse/keys plus screenshots) and a host HTTP server.

## Limits and ideas

Single CPU, no SMP. The kernel is 32-bit with at most 512 MiB of RAM. The root filesystem isn't
persistent, PFS has no journal and caps files at ~4 MiB, and the ATA driver is polled PIO (no
DMA, no SATA/AHCI, no partition table). The network stack has no DHCP (static QEMU addresses),
IP fragmentation, TCP listen/accept, or window-based flow control; only the RTL8139 is
supported. Graphics use the Bochs/QEMU VBE interface only, so there is no real-GPU support and the
desktop runs in a single process (apps are built in rather than separate clients talking to a
window server). `ppm` has no package signatures beyond SHA-256 in the index, and the repository
is plain HTTP. There are no real signals (`kill` terminates), no copy-on-write `fork`, and the
bootloader is BIOS-only. The bitmap font is the standard VGA text-mode font as rendered by QEMU's
VGA BIOS (LGPL-licensed); swap in your own if you need different terms.

Natural next steps: a window-server protocol so separate programs can open windows, a USB-free
mouse wheel and clipboard, DHCP and `listen`/`accept`, an e1000 driver, DMA disk I/O, a journal,
signed packages, a persistent root filesystem, real signals, a 64-bit port.
