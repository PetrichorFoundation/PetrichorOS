/* ACPI soft-off: find the FADT through the RSDP/RSDT, read the S5 sleep type out of the DSDT,
 * and write it to the PM1 control register.  This is how real PCs power off. */
#include "kernel.h"

struct sdt {
    char sig[4];
    u32 len;
    u8 rev, csum;
    char oem[6], oem_table[8];
    u32 oem_rev, creator, creator_rev;
} __attribute__((packed));

/* Tables can sit above the identity-mapped RAM, so map each one explicitly. */
static const u8 *map_table(u32 phys)
{
    const struct sdt *h = mmio_map(phys, sizeof(struct sdt));
    if (!h || h->len < sizeof *h || h->len > (1u << 20))
        return NULL;
    return mmio_map(phys, h->len);
}

static u32 find_rsdt(void)
{
    u32 ebda = (*(volatile u16 *)0x40E) << 4;
    u32 ranges[2][2] = {{ebda, ebda + 1024}, {0xE0000, 0x100000}};
    for (int r = 0; r < 2; r++) {
        if (r == 0 && (ebda < 0x80000 || ebda >= 0xA0000))
            continue;
        for (u32 a = ranges[r][0]; a + 20 <= ranges[r][1]; a += 16)
            if (!memcmp((void *)(uptr)a, "RSD PTR ", 8))
                return *(u32 *)(uptr)(a + 16);                    /* RsdtAddress */
    }
    return 0;
}

/* Powers the machine off; returns only if ACPI isn't usable. */
void acpi_poweroff(void)
{
    u32 rsdt_phys = find_rsdt();
    if (!rsdt_phys)
        return;
    const u8 *rsdt = map_table(rsdt_phys);
    if (!rsdt)
        return;
    u32 n = (((const struct sdt *)rsdt)->len - sizeof(struct sdt)) / 4;

    const u8 *fadt = NULL;
    for (u32 i = 0; i < n && !fadt; i++) {
        u32 t = ((const u32 *)(rsdt + sizeof(struct sdt)))[i];
        const struct sdt *h = mmio_map(t, sizeof *h);
        if (h && !memcmp(h->sig, "FACP", 4))
            fadt = map_table(t);
    }
    if (!fadt)
        return;
    u32 dsdt_phys = *(const u32 *)(fadt + 40), smi_cmd = *(const u32 *)(fadt + 48);
    u8 acpi_enable = fadt[52];
    u32 pm1a = *(const u32 *)(fadt + 64), pm1b = *(const u32 *)(fadt + 68);
    if (!pm1a)
        return;

    const u8 *dsdt = map_table(dsdt_phys);
    if (!dsdt)
        return;
    u32 dlen = ((const struct sdt *)dsdt)->len;
    int found = 0;
    u16 typa = 0, typb = 0;
    for (u32 i = sizeof(struct sdt); i + 12 < dlen; i++) {
        if (memcmp(dsdt + i, "_S5_", 4) || dsdt[i + 4] != 0x12)       /* a Package follows the name */
            continue;
        const u8 *p = dsdt + i + 5;
        p += ((*p & 0xC0) >> 6) + 2;                                  /* skip PkgLength and NumElements */
        if (*p == 0x0A)
            p++;                                                      /* byte prefix */
        typa = *p << 10;
        p++;
        if (*p == 0x0A)
            p++;
        typb = *p << 10;
        found = 1;
        break;
    }
    if (!found)
        return;

    if (!(inw(pm1a) & 1) && smi_cmd && acpi_enable) {                 /* switch the chipset into ACPI mode */
        outb(smi_cmd, acpi_enable);
        for (int i = 0; i < 300 && !(inw(pm1a) & 1); i++)
            for (volatile int d = 0; d < 100000; d++)
                ;
    }
    kprintf("[acpi] powering off (PM1a=%x SLP_TYP=%x)\n", pm1a, typa >> 10);
    outw(pm1a, typa | (1 << 13));                                     /* SLP_EN */
    if (pm1b)
        outw(pm1b, typb | (1 << 13));
    for (volatile int i = 0; i < 100000000; i++)
        ;
}
