# Petrichor OS build.  Needs: clang (any recent), ld.lld, qemu-system-i386.

CC     := clang
LD     := ld.lld
HOSTCC := cc
QEMU   := qemu-system-i386
B      := build

TARGET := -target i686-unknown-none-elf
COMMON := $(TARGET) -ffreestanding -fno-pic -fno-pie -fno-stack-protector \
          -mno-sse -mno-mmx -nostdlibinc -O2 -Wall -Wextra -Wno-unused-parameter \
          -std=gnu11 -Iinclude -MMD -MP
KCFLAGS := $(COMMON) -Ikernel -Ilib
UCFLAGS := $(COMMON) -Iuser/lib -Ilib
ASFLAGS := $(TARGET) -Iinclude -I$B
LDFLAGS := -m elf_i386 -z max-page-size=4096 -z noexecstack

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
.PHONY: all iso run run-multiboot run-nographic run-iso run-installer run-installed clean-target clean
all: $B/petrichor.elf $B/disk.img $B/petrichor.iso $B/repo/index

$B/petrichor.elf: $(KOBJ) kernel/kernel.ld
	$(LD) $(LDFLAGS) -T kernel/kernel.ld -o $@ $(KOBJ)

$B/kernel/initrd.o: $B/initrd.img

$B/%.o: %.c
	@mkdir -p $(dir $@)
	$(CC) $(if $(filter user/%,$<),$(UCFLAGS),$(KCFLAGS)) -c $< -o $@

$B/%.o: %.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$B/user/out/%: $B/user/bin/%.o $(ULIB) user/user.ld
	@mkdir -p $(dir $@)
	$(LD) $(LDFLAGS) -T user/user.ld -o $@ $(filter %.o,$^)
$(addprefix $B/user/out/,$(TLSBINS)): $(TLSLIB)

# ---- packages and the package repository ----
# packages/<name>/package (VERSION, DEPS, DESC), src/*.c -> bin/<prog>, files/ copied as-is.
PKGS    := $(notdir $(wildcard packages/*))
PKG_SRC := $(wildcard packages/*/src/*.c)
define PKGBIN_RULE
$B/pkgstage/$(1)/bin/$(2): packages/$(1)/src/$(2).c $(ULIB) user/user.ld
	@mkdir -p $$(dir $$@) $B/pkgobj
	$$(CC) $$(UCFLAGS) -c $$< -o $B/pkgobj/$(1)-$(2).o
	$$(LD) $$(LDFLAGS) -T user/user.ld -o $$@ $B/pkgobj/$(1)-$(2).o $$(ULIB)
endef
$(foreach s,$(PKG_SRC),$(eval $(call PKGBIN_RULE,$(word 2,$(subst /, ,$(s))),$(basename $(notdir $(s))))))
PKG_BIN := $(foreach s,$(PKG_SRC),$B/pkgstage/$(word 2,$(subst /, ,$(s)))/bin/$(basename $(notdir $(s))))
PKG_FILES := $(shell find packages -type f 2>/dev/null)

$B/mkppk: tools/mkppk.c lib/sha256.c lib/sha256.h
	@mkdir -p $B
	$(HOSTCC) -O2 -Ilib -o $@ tools/mkppk.c lib/sha256.c

.PHONY: repo serve-repo
repo: $B/repo/index

$B/repo/index: $B/mkppk $(PKG_BIN) $(PKG_FILES)
	rm -rf $B/repo
	mkdir -p $B/repo
	@for p in $(PKGS); do \
	  mkdir -p $B/pkgstage/$$p; \
	  if [ -d packages/$$p/files ]; then cp -R packages/$$p/files/. $B/pkgstage/$$p/; fi; \
	  ( . packages/$$p/package; $B/mkppk $B/repo $B/pkgstage/$$p $$p "$$VERSION" "$$DEPS" "$$DESC" ) || exit 1; \
	done

# Host side of `ppm update` (QEMU reaches the host at 10.0.2.2).
serve-repo: repo
	cd $B/repo && python3 -m http.server 8080 --bind 127.0.0.1

# ---- bootloader and disk image ----
$B/boot/%.bin: boot/%.S boot/%.ld
	@mkdir -p $(dir $@)
	$(CC) -target i386-unknown-none-elf -c $< -o $B/boot/$*.o
	$(LD) -m elf_i386 -T boot/$*.ld --oformat binary -o $@ $B/boot/$*.o

$B/kernel.bin: $(KOBJ) kernel/kernel.ld
	$(LD) $(LDFLAGS) -T kernel/kernel.ld --oformat binary -o $@ $(KOBJ)

$B/mkdisk: tools/mkdisk.c
	@mkdir -p $B
	$(HOSTCC) -O2 -o $@ $<

# The installer/live ISO: El Torito CD boot, and also a bootable USB image (dd it to a stick).
$B/mkiso: tools/mkiso.c
	@mkdir -p $B
	$(HOSTCC) -O2 -o $@ $<

$B/petrichor.iso: $B/mkiso $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin
	$B/mkiso $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin $@

iso: $B/petrichor.iso

# Updates boot code + kernel in place; the data partition is left alone.
$B/disk.img: $B/mkdisk $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin
	$B/mkdisk $B/boot/stage1.bin $B/boot/stage2.bin $B/kernel.bin $@

# ---- root filesystem image ----
$B/mkinitrd: tools/mkinitrd.c
	@mkdir -p $B
	$(HOSTCC) -O2 -o $@ $<

ROOTFILES := $(shell find rootfs -type f 2>/dev/null)

$B/initrd.img: $B/mkinitrd $(UEXE) $(ROOTFILES) $B/boot/stage1.bin $B/boot/stage2.bin
	rm -rf $B/root
	mkdir -p $B/root/bin $B/root/dev $B/root/tmp $B/root/home $B/root/root
	cp -R rootfs/. $B/root/
	mkdir -p $B/root/boot
	cp $B/boot/stage1.bin $B/boot/stage2.bin $B/root/boot/
	cp $(UEXE) $B/root/bin/
	$B/mkinitrd $@ $B/root

# ---- running ----
QEMUFLAGS := -m 128 -no-reboot -drive file=$B/disk.img,format=raw,if=ide,index=0 -nic user,model=rtl8139

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

run-multiboot: all
	$(QEMU) $(QEMUFLAGS) -kernel $B/petrichor.elf -serial stdio

run-nographic: all
	$(QEMU) $(QEMUFLAGS) -display none -serial mon:stdio

clean:
	rm -rf $B

-include $(shell find $B -name '*.d' 2>/dev/null)
