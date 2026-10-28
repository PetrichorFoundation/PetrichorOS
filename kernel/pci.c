/* PCI configuration space (mechanism #1) and device discovery across all buses. */
#include "kernel.h"

u32 pci_read(u32 bus, u32 slot, u32 fn, u32 off)
{
    outl(0xCF8, 0x80000000u | bus << 16 | slot << 11 | fn << 8 | (off & 0xFC));
    return inl(0xCFC);
}

void pci_write(u32 bus, u32 slot, u32 fn, u32 off, u32 v)
{
    outl(0xCF8, 0x80000000u | bus << 16 | slot << 11 | fn << 8 | (off & 0xFC));
    outl(0xCFC, v);
}

u32 pci_cfg_read(const struct pcidev *d, u32 off) { return pci_read(d->bus, d->slot, d->fn, off); }
void pci_cfg_write(const struct pcidev *d, u32 off, u32 v) { pci_write(d->bus, d->slot, d->fn, off, v); }

void pci_enable(const struct pcidev *d)
{
    pci_cfg_write(d, 0x04, pci_cfg_read(d, 0x04) | 0x6);      /* memory space + bus master */
}

/* Finds the index-th function whose class code (class<<16 | subclass<<8 | progif) matches. */
int pci_find_class(u32 class24, int index, struct pcidev *out)
{
    int seen = 0;
    for (u32 bus = 0; bus < 256; bus++) {
        for (u32 slot = 0; slot < 32; slot++) {
            if ((pci_read(bus, slot, 0, 0) & 0xFFFF) == 0xFFFF)
                continue;
            int multi = (pci_read(bus, slot, 0, 0x0C) >> 16) & 0x80;
            for (u32 fn = 0; fn < (multi ? 8u : 1u); fn++) {
                if ((pci_read(bus, slot, fn, 0) & 0xFFFF) == 0xFFFF)
                    continue;
                if ((pci_read(bus, slot, fn, 0x08) >> 8) == class24 && seen++ == index) {
                    out->bus = bus;
                    out->slot = slot;
                    out->fn = fn;
                    return 0;
                }
            }
        }
    }
    return -1;
}
