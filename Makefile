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
KCFLAGS := $(COMMON) -Ikernel
UCFLAGS := $(COMMON) -Iuser/lib
ASFLAGS := $(TARGET) -Iinclude -I$B
LDFLAGS := -m elf_i386 -z max-page-size=4096 -z noexecstack

# ---- kernel ----
KSRC := $(wildcard kernel/*.c) lib/string.c lib/format.c
KASM := kernel/boot.S kernel/isr.S kernel/initrd.S
KOBJ := $(patsubst %.c,$B/%.o,$(KSRC)) $(patsubst %.S,$B/%.o,$(KASM))

# ---- userland ----
ULIB := $B/user/lib/ulib.o $B/user/lib/usys.o $B/lib/string.o $B/lib/format.o
UBIN := $(patsubst user/bin/%.c,%,$(wildcard user/bin/*.c))
UEXE := $(addprefix $B/user/out/,$(UBIN))

.PHONY: all run run-nographic clean
all: $B/petrichor.elf

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
	$(LD) $(LDFLAGS) -T user/user.ld -o $@ $< $(ULIB)

# ---- root filesystem image ----
$B/mkinitrd: tools/mkinitrd.c
	@mkdir -p $B
	$(HOSTCC) -O2 -o $@ $<

ROOTFILES := $(shell find rootfs -type f 2>/dev/null)

$B/initrd.img: $B/mkinitrd $(UEXE) $(ROOTFILES)
	rm -rf $B/root
	mkdir -p $B/root/bin $B/root/dev $B/root/tmp $B/root/home $B/root/root
	cp -R rootfs/. $B/root/
	cp $(UEXE) $B/root/bin/
	$B/mkinitrd $@ $B/root

# ---- running ----
QFLAGS := -kernel $B/petrichor.elf -m 128 -no-reboot

run: all
	$(QEMU) $(QFLAGS) -serial stdio

run-nographic: all
	$(QEMU) $(QFLAGS) -display none -serial mon:stdio

clean:
	rm -rf $B

-include $(shell find $B -name '*.d' 2>/dev/null)
