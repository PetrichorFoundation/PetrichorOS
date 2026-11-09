# Petrichor OS

*petrichor* (n.): the smell of rain on dry earth.

A small Unix-like operating system written from scratch for 64-bit x86 (x86_64). No Linux, no BSD,
no borrowed kernel or libc: the kernel, C library, shell and utilities are all in this repo.

```
  ___    _       _      _                 ___  ___
 | _ \__| |_ _ _(_)__| |_  ___ _ _      / _ \/ __|
 |  _/ -_)  _| '_| / _| ' \/ _ \ '_|    | (_) \__ \
 |_| \___|\__|_| |_\__|_||_\___/_|       \___/|___/
```

## Quick start (macOS or Linux host)

The one-line way: from the repository folder, run `./run`. It checks for the tools (installing them with
Homebrew, apt, dnf or pacman if they're missing), builds everything and starts the desktop in QEMU.
`./run iso`, `./run text` and `./run installed` start the other modes (`./run --check` only builds).

```sh
brew install lld qemu     # plus clang (Xcode command line tools); the build uses qemu-system-x86_64
make run                  # boots build/disk.img: boot menu, then straight into the desktop
make run-uefi             # boot the GPT/EFI disk image with OVMF firmware (UEFI)
make run-uefi-iso         # boot the ISO as a UEFI machine would
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
| Boot mode | Legacy/CSM BIOS or UEFI (x86_64); the same ISO or USB stick boots both. |
| Keyboard and mouse | PS/2, or USB on an **xHCI** (USB 3) controller: boot-protocol keyboards and mice plugged straight into a port. Not supported: devices behind a USB hub (including many front-panel ports and docks), hot-plugging, wireless receivers that aren't boot-protocol, touchscreens and tablets, and old EHCI/OHCI/UHCI-only controllers. |
| Disk | SATA in **AHCI** mode, **NVMe**, legacy IDE, **virtio-blk**, and **USB flash drives/disks** (xHCI, 512-byte sectors, not behind a hub). RAID/"Intel RST" mode and SD card readers on non-USB buses aren't supported. |
| Display | A 32-bit linear framebuffer: VESA (BIOS) or GOP (UEFI). The boot loader reads the monitor's EDID and uses its native resolution (largest mode up to 2560x1600); with no EDID it keeps 1024x768 (BIOS) or the firmware's current mode (UEFI). |
| Network | Intel e1000/e1000e (I210/I211 IDs accepted, untested), virtio-net, Realtek RTL8111/8168 (written but untested) and RTL8139. No Wi-Fi or USB adapters. |

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

**Package manager demo.** Inside Petrichor: `ppm update`, `ppm search`, `ppm install lolcow`, `lolcow`. Packages come
from the separate [Petrichor-Packages](https://github.com/krspybacon/Petrichor-Packages) repository; this repository
holds no packages.

Files you put in `/disk` survive reboots (`make` updates the bootloader and kernel inside
`build/disk.img` but leaves the data partition alone; `make clean` deletes the disk).

## UEFI

Petrichor boots on both BIOS and UEFI machines. `boot/uefi.c` is a UEFI application (`BOOTX64.EFI`, built as PE32+
with clang and `lld-link`) that loads `KERNEL.BIN` from the EFI System Partition to 1 MiB, picks a 1024x768 GOP
framebuffer, reads the memory map (the kernel learns the usable ranges, holes included) and the ACPI tables, keeps a
pristine copy of the kernel for the installer, leaves the firmware and jumps to the kernel's 64-bit entry point,
`_start64`. An optional `CMDLINE.TXT` on the boot volume sets the kernel command line (for example `mode=text`).
`tools/mkefi.c` and the shared `lib/efiimg.c` build the FAT16 ESP image and GPT disks. `petrichor.iso` carries an
El Torito UEFI entry and an 0xEF partition, so the same ISO or USB stick boots under BIOS or UEFI.

**Installing** works from either: the installer (the Install icon on the desktop, or `pinstall`) matches how the live
system was booted. Booted through BIOS it writes the BIOS layout (MBR bootloader, kernel, data partition at LBA 4096);
booted through UEFI it writes a GPT disk with an EFI System Partition (`BOOTX64.EFI` + the kernel) and a data partition
(`/disk`, found at boot by its partition type), and reads everything back to verify it. A UEFI live session never
touches a disk that isn't a Petrichor install; an installed system mounts its own data partition and keeps it between
boots. A UEFI install needs a disk of at least 40 MiB. Tested in QEMU with OVMF: install from the ISO, then boot the
installed disk twice (the data partition is formatted on the first boot and mounted on the second).

## What's in it

**Bootloader** (`boot/`, from scratch, real-mode assembly). BIOS loads the 512-byte MBR, which
loads stage 2 with INT 13h extended reads. Stage 2 enables A20, asks the BIOS for the RAM size
(INT 15h/E801), shows a boot menu (arrow keys or 1-4, auto-boot after four seconds), copies the
kernel above 1 MiB in 32 KiB chunks (INT 15h/87h), switches to 32-bit protected mode and jumps to the
kernel's entry stub using the Multiboot register convention, passing the chosen entry as a kernel command
line. The stub (`kernel/boot.S`) builds page tables, turns on PAE and long mode, and calls the 64-bit `kmain`;
it stops with a message on a CPU that lacks long mode. The kernel image carries a Multiboot header with the
"a.out kludge", so QEMU's `-kernel` can load the flat `build/kernel.bin` directly.
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
| Boot | Multiboot entry (own bootloader or QEMU) in 32-bit mode, then long mode; 64-bit GDT with ring 0/3 segments and a TSS |
| Interrupts | IDT, remapped 8259 PIC, 100 Hz PIT, `int 0x80` syscalls |
| Memory | Physical frame bitmap, first-fit kernel heap, 2-level paging, per-process address spaces, shared device-memory mappings |
| Processes | `fork`, `exec` (ELF64 and `#!` scripts), `wait`, `exit`, `kill`, process groups, preemptive round-robin scheduler for user code, sleep/wakeup with timeouts, `poll` |
| Filesystems | A RAM filesystem for `/` (unpacked from an initrd) and **PFS**, an on-disk filesystem mounted at `/disk` (below) |
| Disk | A block-device layer with five drivers: legacy IDE (PIO), **AHCI/SATA** (DMA), **NVMe**, **virtio-blk** and **USB mass storage** (xHCI, Bulk-Only/SCSI); raw `/dev/hda`.. block devices; the boot disk is identified by content, so any bus works; 64-block write-through buffer cache |
| Power | ACPI soft-off (RSDP/RSDT/FADT/DSDT `_S5_`), keyboard-controller reset |
| Network | PCI scan, RTL8139 driver, ARP, IPv4, ICMP, UDP, TCP client (below) |
| Graphics and input | Linear framebuffer at the monitor's native size (VBE/GOP, chosen from EDID; Bochs dispi under QEMU `-kernel`), raw keyboard events (arrows, F-keys, modifiers), PS/2 and USB keyboards and mice with scroll wheel, `/dev/input` event stream, CMOS real-time clock |
| I/O | Pipes, sockets, VGA text console with ANSI colors, 16550 serial, PS/2 keyboard, a cooked tty with line editing, `^C`, `^D` |
| Isolation | User pointers are validated; faults and wild pointers kill only the offending process |

**PFS** has a superblock, inode and block bitmaps, an inode table, and 4 KiB blocks. Inodes have
12 direct blocks, one indirect and one double-indirect block (files up to 4 GiB; partitions up to 16 GiB;
16384 inodes on new filesystems; filesystems made by older versions keep working). Directories are files
of 32-byte entries. Every write goes straight to disk, so there is nothing to `sync`. The VFS
dispatches per inode, crosses mount points in both directions (including `..` and `getcwd`),
returns `EXDEV` for cross-device renames, and can `exec` programs stored on disk. The kernel
formats the partition on first boot.

**Networking.** Drivers for the common wired cards, picked automatically at boot: Intel PRO/1000 ("e1000", the default
card in VirtualBox and VMware and in many physical Intel NICs, including QEMU's e1000 and e1000e/82574L), virtio-net
(QEMU/KVM and most cloud VMs) the Realtek RTL8111/8168/8101 family and the RTL8139. A DHCP client gets the address, netmask, gateway and DNS server
from whatever network it is on; if no DHCP server answers within about 15 seconds it falls back to QEMU's user-mode
defaults (`10.0.2.15`). Sockets are file descriptors: `socket`, `connect`, `bind`, `sendto`, `recvfrom`, plus ordinary
`read`/`write`; a socket call made while DHCP is still running waits for it. TCP is a client with stop-and-wait
transmission, retransmission with timeouts, and orderly close; there is no listen/accept yet. Not supported: Wi-Fi,
USB network adapters, newer Intel I217-I225 cards (their IDs are not accepted: those chips need PHY power-up
steps that aren't implemented), IPv6, and DHCP lease renewal. The RTL8168 driver is written to the chip's documented
ring interface but QEMU has no model of it, so it has never run on real hardware. Tested in QEMU with rtl8139, e1000, e1000e and
virtio-net, on the default network and on a different subnet.

**Desktop** (`user/apps/pdesk`; starts automatically at boot, or run `pdesk`). Real XFCE can't run
here (it needs X11, GTK, glib, a POSIX libc, dynamic linking and threads), so Petrichor has its own
XFCE-style desktop, written from scratch on top of the framebuffer and input devices:

- window manager: draggable, resizable, minimizable, maximizable windows with shadows, edge snapping
  (left/right half, top to maximize), Alt+Tab, Alt+F4, Alt+F2 (Run), double-click-title maximize,
  damage-tracked repainting and a software mouse cursor
- panel with Applications menu, taskbar, system tray (network, CPU, memory) and clock with a calendar
  popup; a floating dock; desktop icons; right-click context menu; toast notifications
- appearance: five wallpapers and six accent colors, saved in `/disk/etc/pdesk.conf`; animations (windows open,
  close, minimize and restore by scaling and fading a snapshot, menus unroll, the dock lifts hovered icons and
  bounces launching ones, notifications slide in) that Settings can turn off
- apps: **Web Browser** (a package: see "The browser is a package" below), **Terminal** (runs `sh` over pipes, ANSI colors, wheel/PgUp scrollback, `^C`/`^D`),
  **File Manager**, **Text Editor**, **System Monitor**, **Software** (a front-end for `ppm`:
  browse, install, remove, upgrade), **Settings** (appearance, startup mode, system info,
  shortcuts), **Run Program**, **Install Petrichor** (wizard, shown when a second disk is attached),
  and About
- needs QEMU's default VGA (`-vga std`); other graphics hardware isn't supported yet

**Web browser** (now the `browser` package in Petrichor-Packages: `browser.c`, plus the page engine `dom.c css.c layout.c paint.c img.c`;
`fetch` does the downloading). Opens `http://` and `https://` pages (a padlock marks a verified connection) and
local `file://` pages. A page and its stylesheets and images load in background `fetch` processes (three at a
time), so the desktop never freezes, and the page re-renders as things arrive. The engine is written from scratch:

- **HTML**: a real DOM with implied end tags, entities, tables, lists, forms (drawn, not interactive), `<img>`
  (including `data:` URIs), `<noscript>` hidden, legacy attributes (`bgcolor`, `align`, `cellpadding`, ...).
- **CSS**: `<style>`, linked sheets and `style=""`; the cascade with specificity and `!important`; selectors
  (type, class, id, attribute, descendant/child/sibling combinators, `:first-child`, `:nth-child`, `:not`, ...);
  custom properties and `var()`; `@media` (width queries); colors (named, hex, `rgb()`, `hsl()`, alpha),
  `px/em/rem/%/vw/vh`, `calc()` of lengths; fonts (size, weight, style, family -> proportional or monospace),
  `text-align`, `line-height`, `text-decoration`, `text-transform`, `white-space`; the box model (margin
  collapsing, padding, borders, `border-radius`, `box-sizing`, min/max sizes), backgrounds (colors, gradients,
  images with `cover`/`contain`).
- **Layout**: block and inline flow with line breaking, floats and `clear`, inline-block, **flexbox** (wrap,
  grow/shrink, justify, align, gaps, auto margins), basic **grid** (explicit and auto-fit columns), tables
  (column sizing, spans, vertical alignment), list markers.
- **Images**: PNG (all color types, interlaced), JPEG (baseline and progressive) and GIF decoders, box-filter scaled.
- **Text**: the 8x16 bitmap font is rendered at any size with per-glyph proportional advances, bold and italic.

Not supported: the parts of JavaScript listed under "JavaScript and interactive pages", `position: absolute/fixed`
elements (they are left out), SVG, WebP/AVIF, web fonts (everything uses the built-in font), CSS animations,
shadows, `::before/::after`, and POST form submission. It was tested on local pages and on real sites (Wikipedia,
Hacker News, NPR, example.com).

**HTTPS** (`user/lib/tls/`). A TLS 1.2 client written from scratch, with no third-party code: SHA-2,
HMAC and the TLS PRF, AES-128/256-GCM, big-integer arithmetic (Montgomery), RSA PKCS#1 v1.5 and ECDSA
signature checks, ECDHE on P-256 and P-384, and an X.509 parser. Cipher suites: `ECDHE_{RSA,ECDSA}_WITH_AES_{128,256}_GCM`,
with extended master secret when the server offers it. Servers are verified for real: the chain must
lead to one of the 157 trusted roots in `/etc/ssl/roots.bin` (extracted from the macOS system keychain
by `tools/mkroots.py`; run it again to refresh the list), signatures, validity dates (the clock is
read from the RTC; a day of slack tolerates a clock kept in local time), CA flags and path length, key
usage, and the host name (subjectAltName, wildcards, IP addresses). Expired, self-signed, untrusted and
wrong-host certificates are refused with a readable reason (checked against badssl.com). Extra roots
for a private server go in `/etc/ssl/extra.bin` (`tools/mkroots.py rootfs/etc/ssl/extra.bin my.pem`).
`fetch`, `wget` and `ppm` speak HTTPS; the TLS code is linked only into those programs. `fetch -k` and
`wget -k` skip certificate checks for testing. Limits: TLS 1.2 only (no TLS 1.3, so a server that
refuses 1.2 can't be reached), no ChaCha20, no session resumption, no client certificates, no
revocation checking (CRL/OCSP), no RSA-PSS certificate signatures, and the X.509 name matching is a
byte comparison. Big-integer code is not constant-time. Randomness comes from `/dev/random`: a
SHA-256 pool stirred by RDRAND (where the CPU has it) and by timing jitter from interrupts, which is
solid on real hardware but weaker under an emulator without RDRAND.

**GrapeJuice** (in Petrichor-Packages, install with `ppm install grapejuice`) is a small Windows translation layer:
`grape program.exe [args]` loads a 64-bit Windows program (PE32+), applies its relocations, resolves its imports
against GrapeJuice's own implementations of the Windows API, and runs it. Typing `program.exe` in the shell (or
double-clicking it in the File Manager) runs it through `grape`.

- **Console programs**: ~80 kernel32 and C runtime (msvcrt/ucrt) functions (printf, malloc, file I/O, heap, time...).
- **GUI programs** (from a terminal in the desktop): Win32 windows with `RegisterClass`/`CreateWindowEx`, the message
  loop (`GetMessage`/`DispatchMessage`, `WM_PAINT`, mouse, keyboard, `WM_TIMER`, `WM_CLOSE`...), GDI drawing (pens,
  brushes, rectangles, ellipses, polygons, lines, text, bitmaps, `BitBlt`/`StretchDIBits`, DIB sections), built-in
  BUTTON / STATIC / EDIT controls (push buttons, check boxes, radio buttons, group boxes, single-line edit) and a
  real `MessageBox` dialog. Each top-level window becomes a window of the desktop: the program draws into its own
  pixel buffer and sends rows over two pipes that every terminal gives its shell (`include/winproto.h`); the desktop
  sends back mouse, keyboard and close events. The kernel now enables SSE with per-process register state, because
  compiled Windows code uses it everywhere.
- Samples ship with the package (`/disk/share/grapejuice/hello.exe`, `demo.exe`, `gui1.exe`, `gui2.exe`, built by
  `tools/mkwinsamples.sh`; sources are alongside).

It is **not Wine**. It can run small Win32 programs that use the subset above; it can't run 32-bit programs (the
kernel is 64-bit only), anything using DirectX, OpenGL, .NET, COM or MFC-style resources, menus (they are accepted but
not shown), dialog templates, common controls (list views, trees, tabs), threads, their own DLLs, or the standard
MSVC/MinGW startup code (needs a thread environment block and exception handling). `grape -v` lists the imports it
resolved; calling one it lacks stops the program with its name. Windows can't be resized by the user.

**Package manager** (`ppm`). Packages are `.ppk` archives served over HTTP from a repository whose
`index` lists name, version, size, SHA-256 and dependencies. `ppm update | search | info |
install | remove | upgrade | list | files | repo`. Installs resolve dependencies first, verify size
and SHA-256 before writing anything, install under `/disk` (so they persist), refuse to remove a
package that others need, and the shell finds programs in `/bin` and `/disk/bin`. Host tools:
`tools/mkppk` builds packages and `make package PKGDIR=... OUT=...` builds one package directory; the
Petrichor-Packages repository's own `make` runs it for every package, and its `make serve` serves the result.

By default `ppm` downloads from a public GitHub repository, `krspybacon/Petrichor-Packages`, over HTTPS (it
falls back to the development server below if that can't be reached), so on a running Petrichor
`ppm update && ppm install cowsay` just works. `tools/publish-packages.sh OWNER/petrichor-packages` builds the
repository and pushes it to GitHub; `ppm repo URL` points a machine at a different repository.

**Icons.** The desktop's icons are drawn at run time by a small anti-aliased vector rasterizer
(`user/apps/pdesk/icons.c`: rounded rectangles, ellipses, polygons, gradients, soft shadows) into a
64x64 master that is cached and box-filtered to each requested size, so they stay crisp at any size.
File Manager shows type-specific icons (folders, text, code, web, archives, programs).
`pdesk --icons` opens a gallery of all of them.

**`.rain` files** are Petrichor's executables: a native program or a `#!` script whose name ends in `.rain`.
Run one by typing its name (`hello.rain` in the current directory, or a path), or leave the extension off for
programs in `/bin` and `/disk/bin` (`hello` finds `hello.rain` if there is no plain `hello`). In the File Manager they
get the program icon and double-clicking runs them in a terminal. `/bin/hello.rain` is a sample script.

**Userland** (`user/`): a tiny libc (`printf`, `malloc`, DNS resolver, HTTP/HTTPS client, SHA-256, a 2D
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
rootfs/     files copied into the root filesystem (/etc/motd, ...)
tools/      host tools: mkinitrd (root image), mkdisk (bootloader + kernel -> disk image), mkppk (packages),
            mkroots.py (trusted roots for HTTPS), gen_crypto_tables.py (SHA-512/AES tables)
```

## Design notes

- **Memory map.** The kernel is 64-bit, but everything lives in the low 4 GiB and the layout is simple: the
  kernel sits at 1 MiB and RAM is identity-mapped (supervisor-only, 2 MiB pages) at [0, 1 GiB) in every
  address space, so the kernel can reach any physical frame directly. User programs are linked at
  `0x40000000` and the stack tops out at `0xC0000000`; device memory (PCI BARs, the framebuffer) is mapped in
  a shared window above 3 GiB. Each process has its own 4-level page tables (PML4).
- **Concurrency model.** The kernel runs with interrupts off and is not preemptible; only user
  code is preempted. This keeps the kernel free of locks. Blocking calls sleep on a channel.
- **System calls.** `int 0x80` with `rax` = number, `rdi rsi rdx r10 r8` = arguments, result in `rax`, negative
  values are `-errno`. User programs use the normal x86_64 C calling convention; `user/lib/usys.S` moves the
  fourth argument from `rcx` to `r10`. See `include/abi.h`.
- **Storage.** `/` is a RAM filesystem rebuilt from the initrd each boot, so changes outside
  `/disk` are lost on reboot. `/disk` is persistent.
- **Disk image layout.** LBA 0 MBR, 1-15 stage 2, 16+ kernel image, 4096+ PFS partition.

## Testing

`selftest` (run inside Petrichor) performs 87 end-to-end checks of files, directories, the on-disk
filesystem (including a 100 KiB file across indirect blocks), raw disks, devices, sockets, `#!` scripts,
`poll`, the clock, fork/exec/wait/kill, pipes (including blocking on a full pipe), the heap, and a
physical-memory leak check across repeated fork/exit. The crypto primitives were checked on the host
against OpenSSL and Python's `hashlib`/`hmac` (hashes, HMAC, PRF, GCM vectors, ECDH, ECDSA and RSA
signatures including tampered ones); the TLS client was tested against live sites, `openssl s_server`
(AES-128/256, RSA and ECDSA certificates, with and without extended master secret) and badssl.com. Checks that need a disk or NIC are skipped
if none is attached. The desktop, installer, boot menu and package manager were tested by driving
QEMU's monitor (injected mouse/keys plus screenshots), a two-disk setup, and a host HTTP server.

## Limits and ideas

Single CPU, no SMP. The kernel is 64-bit but only uses the low 4 GiB (at most 512 MiB of RAM is used), and needs an x86_64 CPU, and disks over 2 TiB aren't
addressed fully. The root filesystem isn't persistent and PFS has no journal and file names are limited to 27 characters.
Disk drivers are polled (no interrupts), AHCI/NVMe use one queue and one command at a time, and NVMe
needs 512-byte sectors. The installer copies the whole boot region and has no partitioning, no
dual-boot support and no choice of what to install. USB support is xHCI only, polled, with no hubs and no hot-plug. The network stack has no IP fragmentation, DHCP renewal,
TCP listen/accept or window-based flow control, and Wi-Fi and USB adapters aren't supported. The desktop needs a
32-bit framebuffer and runs in a single process (apps are built in rather than separate
clients of a window server). Terminal lines don't reflow when a window is resized. `ppm` has no
package signatures beyond SHA-256 in the index (the repository can now be served over HTTPS). There are no real
signals (`kill` terminates) and no copy-on-write `fork`. The bitmap font is the standard VGA text-mode
font as rendered by QEMU's VGA BIOS (LGPL-licensed); swap in your own if you need different terms.

Natural next steps: TLS 1.3, regular expressions and promises for JavaScript, SVG/WebP and positioned elements for the browser, UEFI boot (and GPT), USB hubs/hot-plug and EHCI, more NICs (Wi-Fi, USB), AHCI/NVMe interrupts and multiple queues, real mode-setting GPU drivers, partitioning and
dual-boot in the installer, a window-server protocol, DHCP and `listen`/`accept`, a journal, signed
packages, real signals, a 64-bit port.

## License

Petrichor OS is released under the [MIT License](LICENSE), except for the 8x16 bitmap font in
`lib/font8x16.h`, which is the standard VGA text-mode font (LGPL-licensed; see above).

## Display, USB and drivers added later

**Display.** The BIOS stage 2 reads the monitor's EDID (VBE/DDC) and the UEFI loader reads the EDID protocol, then
both pick the largest 32-bit BGR mode that fits the monitor's preferred resolution. The desktop (`pdesk`) takes its
size from the framebuffer. **Settings > Display** shows the screen mode and the monitor's preferred mode and refresh
rate, and lets you draw the desktop at a smaller size (it is stretched to the screen) or at the native size; the
choice and the animation frame-rate cap are saved in `/disk/etc/pdesk.conf`. What it can't do: change the real
video mode or refresh rate on hardware (that takes a GPU-specific driver; VBE/GOP only offer what was set at boot),
and sync to vblank. The frame-rate cap paces animation to the monitor's rate (default) but is limited by the kernel's
100 Hz clock to about 100 fps, and frames land on 10 ms boundaries. Verified in QEMU (BIOS and OVMF) with EDID
monitors; real-panel behaviour is untested, and a monitor whose native size has no VBE mode gets the next smaller one.

**USB.** `kernel/xhci.c` takes the controller from the firmware, enumerates the root ports at boot, drives HID
boot-protocol keyboards (with software key repeat) and mice, and USB mass storage as `/dev/hdX`. A USB stick that
holds the install ISO is recognised and can never be chosen as an install target. Verified with QEMU's xHCI,
`usb-kbd`, `usb-mouse` and `usb-storage` (read/write and install from a USB-attached ISO), under BIOS and UEFI.

**Disks and network.** virtio-blk (`kernel/virtio_blk.c`, tested in QEMU) and the Realtek RTL8168 family driver
(`kernel/rtl8168.c`, untested). Writes to a raw disk must still be whole 512-byte sectors.

## Daily-use features

- **Clipboard** shared by every app: Ctrl+C/X/V in the editor and the browser's address bar, Ctrl+Shift+C/V in the
  terminal (dragging selects and copies), middle click pastes.
- **Text editor:** selection with Shift+arrows or the mouse, Ctrl+A, undo/redo (Ctrl+Z/Ctrl+Y), files up to 1 MiB,
  and a warning before closing with unsaved changes.
- **Files:** Ctrl+C / Ctrl+V copies files and whole folders (never overwriting: "name (2)"), a Pictures place,
  and images open in the new **Image Viewer** (PNG, JPEG, GIF; Left/Right for the rest of the folder).
- **Screenshots:** Print Screen saves a PNG in `/disk/Pictures`.
- **Power:** Restart uses the ACPI reset register, then the PCI reset port and the keyboard controller, so it works on
  machines without a PS/2 controller. Settings > Power turns the screen off (black) after 5-60 idle minutes.
  Not supported: battery level, sleep, brightness keys, real DPMS monitor standby (all need an ACPI AML interpreter
  or GPU driver).
- **Storage:** files up to 4 GiB and data partitions up to 16 GiB (was 4 MiB and 128 MiB).

## JavaScript and interactive pages

The browser runs JavaScript with its own interpreter (`js.c` for the language, `jsdom.c` for the DOM, in the
browser package). `file:///disk/share/browser/jsdemo.html` (linked from the browser's home page) shows it: a counter kept in localStorage, a
to-do list built with a form, a clock on a timer, and a tic-tac-toe game written as a class.

- **Language:** var/let/const, functions, arrow functions, closures, default/rest parameters, destructuring,
  spread, template literals, classes (extends, super, getters/setters, fields), prototypes and `new`, try/catch,
  for-in/of, switch, `?.`, `??`, and Object, Array, String, Number, Math, JSON, Map, Set, Date, console.
- **DOM:** getElementById/querySelector(All), createElement, appendChild/insertBefore/remove, textContent,
  innerHTML, attributes, classList, style, dataset, addEventListener and `onclick=` handlers with bubbling,
  preventDefault/stopPropagation, setTimeout/setInterval/requestAnimationFrame, alert, location, and
  localStorage (saved per site under `/disk/etc/web`).
- **Interaction:** clicks reach page elements; text fields and text areas take typing (with a caret and paste);
  checkboxes, radio buttons, labels, buttons and form submission (GET) work; the page gets keydown, input,
  change, submit, focus and blur events. CSS `:hover`, `:focus` and `:checked` are applied.
- **Not supported:** regular expressions (a regex literal is treated as a plain substring pattern), promises,
  async/await (async functions run synchronously), fetch/XMLHttpRequest, generators, modules, canvas, and POST
  forms. Big web apps that depend on those will not work. Each page's scripts are stopped if they run for too
  long or use more than 32 MiB; there is no garbage collector, the memory is released when you leave the page.

## The browser is a package

Petrichor ships without a web browser. Install it with **Software** (or `ppm install browser`); the dock's Web
Browser button and opening an `.html` file in Files say so and open Software when it is missing. The package
(sources in Petrichor-Packages `packages/browser/`) is the same engine as before, running as its own program, parses HTML with hubbub, NetSurf's HTML5 parser, and styles pages with NetSurf's libcss together with its own CSS
engine (which still handles var(), grid, gradients and rounded corners). Text uses real TrueType fonts (DejaVu, rendered with stb_truetype). User-space `malloc` now returns 16-byte
aligned memory, as SSE-compiled code (such as libcss) expects.

Separate graphical programs work like this: the desktop gives them a window through the window protocol
(`include/winproto.h`) on file descriptors 3 and 4, and the window's pixels live in **shared memory** (a new
`shm()` system call, `kernel/shm.c`), so the program draws directly into a buffer the desktop shows; only "this
part changed" messages and input events travel through the pipes. Windows can be resized and maximized. The
desktop starts such programs itself (`user/apps/pdesk/host.c`), and programs started from a desktop terminal
can open them too. The clipboard is shared through `/tmp/clipboard`.

Packages can now be multi-file programs (`src/<program>/*.c`) and set `CFLAGS` and `LIBS` (for example `gfx`)
in their `package` file.
