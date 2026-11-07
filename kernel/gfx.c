/* Graphics: the Bochs/QEMU "dispi" VBE interface gives a linear 32-bit
 * framebuffer without any BIOS calls.  Works on QEMU (-vga std), Bochs,
 * VirtualBox.  A process maps the framebuffer into its own address space. */
#include "kernel.h"

#define DISPI_INDEX 0x1CE
#define DISPI_DATA  0x1CF
#define FB_USER_VA  0x80000000u

int gfx_owner = -1;
static u32 fb_phys, fb_w, fb_h;

static void dispi_write(u16 idx, u16 v)
{
    outw(DISPI_INDEX, idx);
    outw(DISPI_DATA, v);
}

static u16 dispi_read(u16 idx)
{
    outw(DISPI_INDEX, idx);
    return inw(DISPI_DATA);
}

static int map_user_fb(u32 phys, u32 bytes)
{
    for (u32 off = 0; off < bytes; off += PAGE_SIZE)       /* uncached: the screen must see every write */
        if (vmm_map(cur->pd, FB_USER_VA + off, phys + off, PTE_RW | PTE_US | PTE_SHARED | 0x18) < 0)
            return -ENOMEM;
    return 0;
}

int gfx_map(struct fbinfo *out, u32 w, u32 h)
{
    if (bootinfo.fb_addr && bootinfo.fb_bpp == 32) {          /* VESA mode set by our bootloader */
        if (w && (w != bootinfo.fb_w || h != bootinfo.fb_h))     /* 0x0: whatever the bootloader chose */
            return -EINVAL;
        int e = map_user_fb(bootinfo.fb_addr, bootinfo.fb_pitch * bootinfo.fb_h);
        if (e < 0)
            return e;
        console_suspend();
        gfx_owner = cur->pid;
        out->addr = FB_USER_VA;
        out->width = bootinfo.fb_w;
        out->height = bootinfo.fb_h;
        out->pitch = bootinfo.fb_pitch;
        out->mon_w = bootinfo.edid_w;
        out->mon_h = bootinfo.edid_h;
        out->mon_mhz = bootinfo.edid_mhz;
        out->fixed = 1;
        return 0;
    }
    if (!w) {
        w = 1024;
        h = 768;
    }
    if (w < 320 || h < 200 || w > 2560 || h > 1600)
        return -EINVAL;
    u32 slot;
    for (slot = 0; slot < 32; slot++)
        if (pci_read(0, slot, 0, 0) == 0x11111234)      /* QEMU/Bochs VGA */
            break;
    if (slot == 32 || dispi_read(0) < 0xB0C0)
        return -ENODEV;
    pci_write(0, slot, 0, 0x04, pci_read(0, slot, 0, 0x04) | 0x7);   /* memory space + bus master */
    fb_phys = pci_read(0, slot, 0, 0x10) & ~0xFu;

    dispi_write(4, 0);                                  /* disable while reprogramming */
    dispi_write(1, w);
    dispi_write(2, h);
    dispi_write(3, 32);
    dispi_write(4, 0x41);                               /* enabled + linear framebuffer */
    fb_w = w;
    fb_h = h;

    u32 bytes = w * h * 4;
    for (u32 off = 0; off < bytes; off += PAGE_SIZE)
        if (vmm_map(cur->pd, FB_USER_VA + off, fb_phys + off, PTE_RW | PTE_US | PTE_SHARED) < 0)
            return -ENOMEM;
    gfx_owner = cur->pid;
    out->addr = FB_USER_VA;
    out->width = w;
    out->height = h;
    out->pitch = w * 4;
    out->mon_w = out->mon_h = out->mon_mhz = 0;
    out->fixed = 0;
    return 0;
}

/* Leave graphics mode; the VGA text console reappears. */
void gfx_off(void)
{
    if (gfx_owner < 0)
        return;
    if (bootinfo.fb_addr)
        console_resume();                                    /* redraw the text console */
    else
        dispi_write(4, 0);
    gfx_owner = -1;
}
