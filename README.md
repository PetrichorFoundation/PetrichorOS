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

## UEFI

Petrichor boots on both BIOS and UEFI machines. `boot/uefi.c` is a UEFI application (`BOOTX64.EFI`, built as PE32+
with clang and `lld-link`) that loads `KERNEL.BIN` from the EFI System Partition to 1 MiB, picks a 1024x768 GOP
framebuffer, reads the memory map (the kernel learns the usable ranges, holes included) and the ACPI tables, leaves
the firmware and jumps to the kernel's 64-bit entry point, `_start64`. `tools/mkefi.c` builds the FAT16 ESP image and
a GPT disk image (`build/uefi.img`, usable on a USB stick or in a VM); `petrichor.iso` carries an El Torito UEFI entry
and an 0xEF partition, so the same ISO or stick boots under BIOS or UEFI. Limits: a UEFI boot is a live session
(it never touches the boot disk, so `/disk` stays empty), and the installer is hidden, because it writes
BIOS boot code; the loader also needs 1 MiB to be free RAM (it is on common firmware) and a 32-bit BGR GOP mode.

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
- appearance: five wallpapers and six accent colors, saved in `/disk/etc/pdesk.conf`; animations (windows open,
  close, minimize and restore by scaling and fading a snapshot, menus unroll, the dock lifts hovered icons and
  bounces launching ones, notifications slide in) that Settings can turn off
- apps: **Web Browser** (below), **Terminal** (runs `sh` over pipes, ANSI colors, wheel/PgUp scrollback, `^C`/`^D`),
  **File Manager**, **Text Editor**, **System Monitor**, **Software** (a front-end for `ppm`:
  browse, install, remove, upgrade), **Settings** (appearance, startup mode, system info,
  shortcuts), **Run Program**, **Install Petrichor** (wizard, shown when a second disk is attached),
  and About
- needs QEMU's default VGA (`-vga std`); other graphics hardware isn't supported yet

**Web browser** (`user/apps/pdesk/`: `browser.c`, plus the page engine `dom.c css.c layout.c paint.c img.c`;
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

Not supported: JavaScript (so no menus, lazy-loaded images or single-page apps), `position: absolute/fixed`
elements (they are left out), SVG, WebP/AVIF, web fonts (everything uses the built-in font), CSS animations,
shadows, `::before/::after`, and form submission. It was tested on local pages and on real sites (Wikipedia,
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

**GrapeJuice** (`packages/grapejuice`, install with `ppm install grapejuice`) is a small Windows translation layer:
`grape program.exe [args]` loads a 64-bit Windows program (PE32+), applies its relocations, resolves its imports
against GrapeJuice's own implementations of ~80 Windows (kernel32, `MessageBoxA` as text) and C runtime (msvcrt/ucrt)
functions, and runs it. The Windows x64 calling convention is handled by compiling the shims as `ms_abi`. Once it is
installed, typing `program.exe` in the shell (or double-clicking it in the File Manager) runs it through `grape`.
Two samples ship with it (`/disk/share/grapejuice/hello.exe`, `demo.exe`, built by `tools/mkwinsamples.sh`). What it
can't do: 32-bit programs (the kernel is 64-bit only), programs whose startup code needs a thread environment block
or exception handling (the standard MSVC/MinGW runtime startup), threads, their own DLLs, floating-point `printf`
formats, and anything graphical beyond text; `grape -v` lists the imports it resolved, and calling one it lacks stops
the program with its name. In short: it runs small self-contained console programs, not Wine-class software.

**Package manager** (`ppm`). Packages are `.ppk` archives served over HTTP from a repository whose
`index` lists name, version, size, SHA-256 and dependencies. `ppm update | search | info |
install | remove | upgrade | list | files | repo`. Installs resolve dependencies first, verify size
and SHA-256 before writing anything, install under `/disk` (so they persist), refuse to remove a
package that others need, and the shell finds programs in `/bin` and `/disk/bin`. Host tools:
`tools/mkppk` builds packages; `make repo` builds the sample repository from `packages/`
(`cowsay`, `fortune`, `lolcow`, `calc`, `hd`); `make serve-repo` serves it.

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
packages/   sample packages for the repository (package metadata, sources, data files)
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
addressed fully. The root filesystem isn't persistent and PFS has no journal and caps files at ~4 MiB.
Disk drivers are polled (no interrupts), AHCI/NVMe use one queue and one command at a time, and NVMe
needs 512-byte sectors. The installer copies the whole boot region and has no partitioning, no
dual-boot support and no choice of what to install. Boot is BIOS-only (no UEFI), and there are no USB
drivers (keyboard and mouse rely on firmware PS/2 emulation; USB sticks can boot it but can't be
read once it is running). The network stack has no DHCP (static QEMU addresses), IP fragmentation,
TCP listen/accept or window-based flow control, and only the RTL8139 is supported. The desktop needs a
1024x768 32-bit VESA mode and runs in a single process (apps are built in rather than separate
clients of a window server). Terminal lines don't reflow when a window is resized. `ppm` has no
package signatures beyond SHA-256 in the index (the repository can now be served over HTTPS). There are no real
signals (`kill` terminates) and no copy-on-write `fork`. The bitmap font is the standard VGA text-mode
font as rendered by QEMU's VGA BIOS (LGPL-licensed); swap in your own if you need different terms.

Natural next steps: TLS 1.3, JavaScript, SVG/WebP and positioned elements for the browser, UEFI boot (and GPT), USB (xHCI) keyboard/mouse/storage drivers, more NICs (e1000,
RTL8168), AHCI/NVMe interrupts and multiple queues, selectable display modes, partitioning and
dual-boot in the installer, a window-server protocol, DHCP and `listen`/`accept`, a journal, signed
packages, real signals, a 64-bit port.

## License

Petrichor OS is released under the [MIT License](LICENSE), except for the 8x16 bitmap font in
`lib/font8x16.h`, which is the standard VGA text-mode font (LGPL-licensed; see above).
