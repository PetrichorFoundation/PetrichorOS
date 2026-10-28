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
make run                  # boots build/disk.img: boot menu, then straight into the desktop
make run-nographic        # same, with the serial console in your terminal (Ctrl-A X quits)
make run-multiboot        # skip the bootloader: QEMU loads the kernel directly
```

Petrichor shows a boot menu (it boots the first entry after four seconds), then starts the graphical
desktop by itself. Choose "text console" in the menu, or "Exit to Console" in the Applications menu,
for a shell. In a shell try `help`, `ls /bin`, `selftest`, `df`, `lsdisk`, `ifconfig`,
`ping 10.0.2.2`, `nslookup example.com`; `pdesk` starts the desktop and `halt` powers off.

**ISO and USB.** `make iso` builds `build/petrichor.iso`: a bootable CD image that is also a bootable
USB image (see "Putting it on a real PC" below). `make run-iso` boots it in QEMU with a blank disk to
install onto. The ISO runs live (nothing is saved) and can install Petrichor onto a disk.

**Installing.** The installer copies Petrichor to a disk. In QEMU:

```sh
make run-installer        # boots the installer medium with a blank second disk attached
                          #   then double-click "Install" on the desktop and follow the wizard
make run-installed        # boots the disk the installer wrote, with no installer medium
```

`pinstall` is the engine behind the wizard and also works from a shell (`pinstall`,
`pinstall --probe hdb`, or `pinstall -y hdb`). It works from any boot medium (CD, USB, disk), builds
the image from the bootloader files shipped in the system plus the pristine kernel the bootloader
kept in RAM, refuses to touch the disk it is running from, verifies the copy byte for byte, and leaves
the data partition to be formatted on the new disk's first boot. The wizard shows what is already on
each disk (partition tables) and needs an explicit "I understand" tick before it erases anything.

## Putting it on a real PC

I could only test this in QEMU. Real PCs vary a lot, so try it in a virtual machine first, and treat the
first real boot as an experiment on a machine you can afford to wipe. What Petrichor needs:

| Needs | Details |
| --- | --- |
| BIOS boot | Legacy/CSM/"BIOS" boot mode. There is no UEFI loader, so a UEFI-only machine won't boot it. |
| Keyboard and mouse | PS/2, or USB with the firmware's "legacy USB support" (it emulates PS/2). There are no USB drivers; USB mice often aren't emulated, so the desktop may have a keyboard but no pointer. |
| Disk | SATA in **AHCI** mode, **NVMe**, or legacy IDE. RAID/"Intel RST" mode, USB drives and SD cards aren't supported. |
| Display | A 1024x768 32-bit VESA mode from the BIOS (most have one). Otherwise you get the text console only. |
| Network | Only the RTL8139 (rare on real PCs), so expect no network. |

To write the ISO to a USB stick on macOS (double-check the disk number: this overwrites the whole stick):

```sh
diskutil list                       # find the stick, e.g. /dev/disk4
diskutil unmountDisk /dev/disk4
sudo dd if=build/petrichor.iso of=/dev/rdisk4 bs=1m
diskutil eject /dev/disk4
```

Boot from the stick (usually F12/F10/Esc for the boot menu, choosing the entry marked "Legacy" or
"BIOS"), pick **Petrichor**, and double-click **Install** on the desktop. The installer lists the
internal disks it can see; installing **erases the partition table of the disk you pick**, so it is
not a dual-boot installer. Remove the stick and boot from the disk. The ISO also works as a CD in
QEMU/VirtualBox/VMware. If you boot the stick and it shows no disks, the machine's storage mode
isn't one of the supported ones.

**Package manager demo.** In one terminal on the host run `make serve-repo` (serves `build/repo` on
port 8080), then inside Petrichor: `ppm update`, `ppm search`, `ppm install lolcow`, `lolcow`.

Files you put in `/disk` survive reboots (`make` updates the bootloader and kernel inside
`build/disk.img` but leaves the data partition alone; `make clean` deletes the disk).

## What's in it

**Bootloader** (`boot/`, from scratch, real-mode assembly). BIOS loads the 512-byte MBR, which
loads stage 2 with INT 13h extended reads. Stage 2 enables A20, asks the BIOS for the RAM size
(INT 15h/E801), shows a boot menu (arrow keys or 1-4, auto-boot after four seconds), copies the
kernel above 1 MiB in 32 KiB chunks (INT 15h/87h), switches to 32-bit protected mode and jumps to the
kernel using the Multiboot register convention, passing the chosen entry as a kernel command line.
`tools/mkdisk` lays it all out on the disk image.

**Graphics at boot.** For the default and "graphical desktop" menu entries the bootloader asks the BIOS
(VESA VBE) for a 1024x768 32-bit linear framebuffer before starting the kernel and passes its
address in the Multiboot info. The kernel then draws its text console on that framebuffer (so boot
messages and the shell work without VGA text mode) and hands the same framebuffer to the desktop.
Under QEMU's own VGA the older Bochs interface is still used if no VESA mode was set.

**Boot flow.** The kernel exposes the command line as `/etc/cmdline` (`mode=text`, `mode=desktop`,
`nodisk`). `init` reads it, falling back to `autostart=` in `/disk/etc/petrichor.conf` (set in
Settings) and then to the desktop. If the desktop can't start or keeps crashing, `init` falls back
to the text console. Logging out restarts the desktop; "Exit to Console" opens a shell and returns
to the desktop when it exits.

**Kernel** (`kernel/`, ~5,000 lines of C and assembly)

| Area | Details |
| --- | --- |
| Boot | Multiboot entry (own bootloader or QEMU/GRUB), GDT with ring 0/3 segments and a TSS |
| Interrupts | IDT, remapped 8259 PIC, 100 Hz PIT, `int 0x80` syscalls |
| Memory | Physical frame bitmap, first-fit kernel heap, 2-level paging, per-process address spaces, shared device-memory mappings |
| Processes | `fork`, `exec` (ELF32 and `#!` scripts), `wait`, `exit`, `kill`, process groups, preemptive round-robin scheduler for user code, sleep/wakeup with timeouts, `poll` |
| Filesystems | A RAM filesystem for `/` (unpacked from an initrd) and **PFS**, an on-disk filesystem mounted at `/disk` (below) |
| Disk | A block-device layer with three drivers: legacy IDE (PIO), **AHCI/SATA** (DMA) and **NVMe**; raw `/dev/hda`.. block devices; the boot disk is identified by content, so any bus works; 64-block write-through buffer cache |
| Power | ACPI soft-off (RSDP/RSDT/FADT/DSDT `_S5_`), keyboard-controller reset |
| Network | PCI scan, RTL8139 driver, ARP, IPv4, ICMP, UDP, TCP client (below) |
| Graphics and input | Bochs/QEMU VBE linear framebuffer (1024x768x32), raw keyboard events (arrows, F-keys, modifiers), PS/2 mouse with scroll wheel, `/dev/input` event stream, CMOS real-time clock |
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

**Desktop** (`user/apps/pdesk`; starts automatically at boot, or run `pdesk`). Real XFCE can't run
here (it needs X11, GTK, glib, a POSIX libc, dynamic linking and threads), so Petrichor has its own
XFCE-style desktop, written from scratch on top of the framebuffer and input devices:

- window manager: draggable, resizable, minimizable, maximizable windows with shadows, edge snapping
  (left/right half, top to maximize), Alt+Tab, Alt+F4, Alt+F2 (Run), double-click-title maximize,
  damage-tracked repainting and a software mouse cursor
- panel with Applications menu, taskbar, system tray (network, CPU, memory) and clock with a calendar
  popup; a floating dock; desktop icons; right-click context menu; toast notifications
- appearance: five wallpapers and six accent colors, saved in `/disk/etc/pdesk.conf`
- apps: **Web Browser** (below), **Terminal** (runs `sh` over pipes, ANSI colors, wheel/PgUp scrollback, `^C`/`^D`),
  **File Manager**, **Text Editor**, **System Monitor**, **Software** (a front-end for `ppm`:
  browse, install, remove, upgrade), **Settings** (appearance, startup mode, system info,
  shortcuts), **Run Program**, **Install Petrichor** (wizard, shown when a second disk is attached),
  and About
- needs QEMU's default VGA (`-vga std`); other graphics hardware isn't supported yet

**Web browser** (`user/apps/pdesk/browser.c`, `html.c`; `fetch` for loading). Opens plain `http://` pages
and local `file://` pages (including directory listings; `.html` files open here from the File
Manager). Pages load in a background `fetch` process, which follows redirects and decodes chunked
responses, so the desktop never freezes. The renderer is written from scratch: headings, paragraphs,
bold/italic/code, links, nested bulleted and numbered lists, simple tables, preformatted text,
blockquotes, centered text, entities and common UTF-8 punctuation, with wrapping that follows the
window size. There is an address bar (bare names like `example.com` work), back/forward/reload/stop,
home, link hover URLs in the status bar and a draggable scrollbar. Not supported: **HTTPS** (there is no
TLS, so https:// pages show an explanation and many sites won't load), images (shown as
`[image: alt]`), CSS, JavaScript, forms that submit, and cookies. It was tested against a local server
(redirects, chunked, 404) and real plain-HTTP sites such as example.com.

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
the network tools `ifconfig ping nslookup wget nc fetch`, and `ppm`.

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

`selftest` (run inside Petrichor) performs 85 end-to-end checks of files, directories, the on-disk
filesystem (including a 100 KiB file across indirect blocks), raw disks, devices, sockets, `#!` scripts,
`poll`, the clock, fork/exec/wait/kill, pipes (including blocking on a full pipe), the heap, and a
physical-memory leak check across repeated fork/exit. Checks that need a disk or NIC are skipped
if none is attached. The desktop, installer, boot menu and package manager were tested by driving
QEMU's monitor (injected mouse/keys plus screenshots), a two-disk setup, and a host HTTP server.

## Limits and ideas

Single CPU, no SMP. The kernel is 32-bit with at most 512 MiB of RAM, and disks over 2 TiB aren't
addressed fully. The root filesystem isn't persistent and PFS has no journal and caps files at ~4 MiB.
Disk drivers are polled (no interrupts), AHCI/NVMe use one queue and one command at a time, and NVMe
needs 512-byte sectors. The installer copies the whole boot region and has no partitioning, no
dual-boot support and no choice of what to install. Boot is BIOS-only (no UEFI), and there are no USB
drivers (keyboard and mouse rely on firmware PS/2 emulation; USB sticks can boot it but can't be
read once it is running). The network stack has no DHCP (static QEMU addresses), IP fragmentation,
TCP listen/accept or window-based flow control, and only the RTL8139 is supported. The desktop needs a
1024x768 32-bit VESA mode and runs in a single process (apps are built in rather than separate
clients of a window server). Terminal lines don't reflow when a window is resized. `ppm` has no
package signatures beyond SHA-256 in the index, and the repository is plain HTTP. There are no real
signals (`kill` terminates) and no copy-on-write `fork`. The bitmap font is the standard VGA text-mode
font as rendered by QEMU's VGA BIOS (LGPL-licensed); swap in your own if you need different terms.

Natural next steps: TLS/HTTPS and image decoding for the browser, UEFI boot (and GPT), USB (xHCI) keyboard/mouse/storage drivers, more NICs (e1000,
RTL8168), AHCI/NVMe interrupts and multiple queues, selectable display modes, partitioning and
dual-boot in the installer, a window-server protocol, DHCP and `listen`/`accept`, a journal, signed
packages, real signals, a 64-bit port.
