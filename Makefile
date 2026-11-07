# Petrichor OS build.  Needs: clang (any recent), ld.lld, qemu-system-x86_64.

CC     := clang
LD     := ld.lld
HOSTCC := cc
QEMU   := qemu-system-x86_64
B      := build

TARGET := -target x86_64-unknown-none-elf
COMMON := $(TARGET) -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -mno-sse -mno-mmx -mno-red-zone -nostdlibinc -O2 -Wall -Wextra -Wno-unused-parameter \
          -std=gnu11 -Iinclude -MMD -MP
KCFLAGS := $(COMMON) -Ikernel -Ilib
UCFLAGS := $(COMMON) -Iuser/lib -Ilib
ASFLAGS := $(TARGET) -Iinclude -I$B
LDFLAGS := -m elf_x86_64 -z max-page-size=4096 -z noexecstack

# ---- kernel ----
KSRC := $(wildcard kernel/*.c) lib/string.c lib/format.c lib/sha256.c
KASM := kernel/boot.S kernel/isr.S kernel/initrd.S
KOBJ := $(patsubst %.c,$B/%.o,$(KSRC)) $(patsubst %.S,$B/%.o,$(KASM))

# ---- userland ----
ULIB := $B/user/lib/ulib.o $B/user/lib/net.o $B/user/lib/http.o $B/lib/sha256.o $B/user/lib/usys.o $B/lib/string.o $B/lib/format.o
# TLS (HTTPS) is linked only into the programs that talk to web servers.
TLSLIB := $(addprefix $B/user/lib/,https.o $(addprefix tls/,hash.o gcm.o bn.o ec.o rsa.o x509.o tls.o))
TLSBINS := fetch wget ppm
UBIN := $(patsubst user/bin/%.c,%,$(wildcard user/bin/*.c))
UEXE := $(addprefix $B/user/out/,$(UBIN))

# Multi-file programs live in user/apps/<name>/*.c and may use the graphics library.
GFXLIB := $B/user/lib/gfx.o
UAPPS  := $(notdir $(wildcard user/apps/*))
define APP_RULE
$B/user/out/$(1): $(patsubst %.c,$B/%.o,$(wildcard user/apps/$(1)/*.c)) $(GFXLIB) $(ULIB) user/user.ld
	@mkdir -p $$(dir $$@)
	$$(LD) $$(LDFLAGS) -T user/user.ld -o $$@ $$(filter %.o,$$^)
endef
$(foreach a,$(UAPPS),$(eval $(call APP_RULE,$(a))))
UEXE += $(addprefix $B/user/out/,$(UAPPS))

.DEFAULT_GOAL := all
.PHONY: all iso run run-uefi run-uefi-iso run-multiboot run-nographic run-iso run-installer run-installed clean-target clean
all: $B/petrichor.elf $B/disk.img $B/petrichor.iso $B/uefi.img

$B/petrichor.elf: $(KOBJ) kernel/kernel.ld
	$(LD) $(LDFLAGS) -T kernel/kernel.ld -o $@ $(KOBJ)

$B/kernel/initrd.o: $B/initrd.img

$B/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(if $(filter user/%,$<),$(UCFLAGS),$(KCFLAGS)) -c $< -o $@

$B/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

# The JavaScript engine computes with doubles: SSE on for its files only (nothing else in the desktop passes
# floating-point values across a call, and the kernel saves SSE state per process).
$B/user/apps/pdesk/js.o $B/user/apps/pdesk/jsdom.o: UCFLAGS += -msse2

$B/user/out/%: $B/user/bin/%.o $(ULIB) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T user/user.ld -o $@ $(filter %.o,$^)
$(addprefix $B/user/out/,$(TLSBINS)): $(TLSLIB)
$B/user/out/pinstall: $B/lib/efiimg.o

# ---- packages ----
# Packages live in their own repository (github.com/krspybacon/Petrichor-Packages), whose Makefile calls this
# rule once per package directory:
#   make package PKGDIR=/abs/path/packages/<name> OUT=/abs/path/to/the/repo
# <name>/package holds VERSION, DEPS, DESC; src/*.c -> bin/<prog>; files/ is copied as-is.
$B/mkppk: tools/mkppk.c lib/sha256.c lib/sha256.h
	@mkdir -p $B
	$(HOSTCC) -O2 -Ilib -o $@ tools/mkppk.c lib/sha256.c

.PHONY: package
package: $B/mkppk $(ULIB) user/user.ld
	@test -n "$(PKGDIR)" -a -n "$(OUT)" || { echo "usage: make package PKGDIR=<dir> OUT=<repo dir>"; exit 1; }
	@test -f "$(PKGDIR)/package" || { echo "$(PKGDIR)/package is missing"; exit 1; }
	@set -e; p=$$(basename "$(PKGDIR)"); st=$B/pkgstage/$$p; mkdir -p "$$st" $B/pkgobj; \
	for s in "$(PKGDIR)"/src/*.c; do \
	  [ -f "$$s" ] || continue; n=$$(basename "$$s" .c); mkdir -p "$$st/bin"; \
	  echo "  CC  $$p/$$n"; \
	  $(CC) $(UCFLAGS) -c "$$s" -o $B/pkgobj/$$p-$$n.o; \
	  $(LD) $(LDFLAGS) -T user/user.ld -o "$$st/bin/$$n" $B/pkgobj/$$p-$$n.o $(ULIB); \
	done; \
	if [ -d "$(PKGDIR)/files" ]; then cp -R "$(PKGDIR)/files/." "$$st/"; fi; \
	( . "$(PKGDIR)/package"; $B/mkppk "$(OUT)" "$$st" "$$p" "$$VERSION" "$$DEPS" "$$DESC" )

# ---- bootloader and disk image ----
$B/boot/%.bin: boot/%.S boot/%.ld
	@mkdir -p $(dir $@)
	$(CC) -target i386-unknown-none-elf -c $< -o $B/boot/$*.o
	$(LD) -m elf_i386 -T boot/$*.ld --oformat binary -o $@ $B/boot/$*.o

# ---- UEFI: the loader (a PE32+ application), the FAT boot partition and a GPT disk image ----
LLDLINK := lld-link

$B/BOOTX64.EFI: boot/uefi.c
	@mkdir -p $B/boot
	$(CC) -target x86_64-unknown-windows -ffreestanding -fno-stack-protector -mno-red-zone -fshort-wchar \
	      -fno-builtin -O2 -Wall -c $< -o $B/boot/uefi.o
	$(LLDLINK) /subsystem:efi_application /entry:efi_main /nodefaultlib /out:$@ $B/boot/uefi.o

$B/mkefi: tools/mkefi.c lib/efiimg.c lib/efiimg.h include/ptypes.h
	@mkdir -p $B
	$(HOSTCC) -O2 -Ilib -Iinclude -o $@ tools/mkefi.c lib/efiimg.c

$B/esp.img: $B/mkefi $B/BOOTX64.EFI $B/kernel.bin
	$B/mkefi fat $@ $B/BOOTX64.EFI $B/kernel.bin

$B/uefi.img: $B/mkefi $B/esp.img
	$B/mkefi gpt $@ $B/esp.img 16

$B/kernel.bin: $(KOBJ) kernel/kernel.ld
	$(LD) $(LDFLAGS) -T kernel/kernel.ld --oformat binary -o $@ $(KOBJ)

$B/mkdisk: tools/mkdisk.c
	@mkdir -p $B
	$(HOSTCC) -O2 -o $@ $<

# The installer/live ISO: El Torito CD boot, and also a bootable USB image (dd it to a stick).
$B/mkiso: tools/mkiso.c
	@mkdir -p $B
	$(HOSTCC) -O2 -o $@ $<

$B/petrichor.iso: $B/mkiso $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin $B/esp.img
	$B/mkiso $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin $@ $B/esp.img

iso: $B/petrichor.iso

# Updates boot code + kernel in place; the data partition is left alone.
$B/disk.img: $B/mkdisk $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin
	$B/mkdisk $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin $@

# ---- root filesystem image ----
$B/mkinitrd: tools/mkinitrd.c
	@mkdir -p $B
	$(HOSTCC) -O2 -o $@ $<

ROOTFILES := $(shell find rootfs -type f 2>/dev/null)

$B/initrd.img: $B/mkinitrd $(UEXE) $(ROOTFILES) $B/boot/stage1.bin $B/boot/stage2.bin $B/BOOTX64.EFI
	rm -rf $B/root
	mkdir -p $B/root/bin $B/root/dev $B/root/tmp $B/root/home $B/root/root
	cp -R rootfs/. $B/root/
	mkdir -p $B/root/boot
	cp $B/boot/stage1.bin $B/boot/stage2.bin $B/BOOTX64.EFI $B/root/boot/
	cp $(UEXE) $B/root/bin/
	$B/mkinitrd $@ $B/root

# ---- running ----
QEMUFLAGS := -m 128 -no-reboot -drive file=$B/disk.img,format=raw,if=ide,index=0 -nic user,model=rtl8139 \
             -device qemu-xhci -device usb-tablet          # tablet: the pointer follows the host mouse, no grab

# A blank disk to install onto (kept between runs; `make clean-target` recreates it).
$B/target.img:
	@mkdir -p $B
	dd if=/dev/zero of=$@ bs=1048576 count=64 2>/dev/null

run: all
	$(QEMU) $(QEMUFLAGS) -serial stdio

# Boot the ISO as a CD with a blank disk attached: pick "Install Petrichor" on the desktop.
run-iso: iso $B/target.img
	$(QEMU) -m 128 -no-reboot -boot d -cdrom $B/petrichor.iso -drive file=$B/target.img,format=raw,if=ide,index=0 -nic user,model=rtl8139 -serial stdio

# Boot the installer medium with a blank second disk, then run "Install Petrichor" on the desktop.
run-installer: all $B/target.img
	$(QEMU) $(QEMUFLAGS) -drive file=$B/target.img,format=raw,if=ide,index=1 -serial stdio

# Boot the disk the installer wrote, with no installer medium attached.
run-installed: all $B/target.img
	$(QEMU) -m 128 -no-reboot -drive file=$B/target.img,format=raw,if=ide,index=0 -nic user,model=rtl8139 -serial stdio

clean-target:
	rm -f $B/target.img

# UEFI: boot with OVMF (EDK II) firmware, from the GPT disk image or from the ISO.
OVMF_CODE ?= $(firstword $(wildcard /opt/homebrew/share/qemu/edk2-x86_64-code.fd /usr/local/share/qemu/edk2-x86_64-code.fd \
             /usr/share/qemu/edk2-x86_64-code.fd /usr/share/OVMF/OVMF_CODE.fd /usr/share/edk2/ovmf/OVMF_CODE.fd))
OVMF_VARS ?= $(firstword $(wildcard /opt/homebrew/share/qemu/edk2-i386-vars.fd /usr/local/share/qemu/edk2-i386-vars.fd \
             /usr/share/qemu/edk2-i386-vars.fd /usr/share/OVMF/OVMF_VARS.fd /usr/share/edk2/ovmf/OVMF_VARS.fd))
UEFIFLAGS = -m 128 -no-reboot -drive if=pflash,format=raw,readonly=on,file=$(OVMF_CODE) \
            -drive if=pflash,format=raw,file=$B/ovmf-vars.fd -nic user,model=rtl8139 -serial stdio

$B/ovmf-vars.fd:
	@test -n "$(OVMF_VARS)" || { echo "no OVMF firmware found: set OVMF_CODE and OVMF_VARS"; exit 1; }
	cp $(OVMF_VARS) $@

run-uefi: all $B/ovmf-vars.fd
	$(QEMU) $(UEFIFLAGS) -drive file=$B/uefi.img,format=raw,if=none,id=d0 -device ide-hd,drive=d0

run-uefi-iso: all $B/ovmf-vars.fd
	$(QEMU) $(UEFIFLAGS) -cdrom $B/petrichor.iso -boot d

run-multiboot: all
	$(QEMU) $(QEMUFLAGS) -kernel $B/kernel.bin -serial stdio

run-nographic: all
	$(QEMU) $(QEMUFLAGS) -display none -serial mon:stdio

clean:
	rm -rf $B

-include $(shell find $B -name '*.d' 2>/dev/null)
