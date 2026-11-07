/* BOOTX64.EFI: the UEFI loader for Petrichor.
 *
 * It does what the BIOS bootloader does, the UEFI way: load KERNEL.BIN from the same FAT volume to 1 MiB, pick
 * a 1024x768 graphics mode, collect the memory map and the ACPI tables, build a Multiboot-style information
 * block, leave the firmware (ExitBootServices) and jump to the kernel's 64-bit entry point.
 *
 * Built as a PE32+ application (clang -target x86_64-unknown-windows, lld-link), so it uses the Microsoft
 * x64 calling convention, which is also what the kernel's _start64 expects.  The UEFI definitions below are
 * just the parts we use. */

typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;
typedef unsigned long long u64;
typedef u64 UINTN;
typedef u64 EFI_STATUS;
typedef u16 CHAR16;

#define NULL ((void *)0)
#define EFI_SUCCESS 0
#define ERR(n) (0x8000000000000000ull | (n))

typedef struct {
    u32 a;
    u16 b, c;
    u8 d[8];
} GUID;

typedef struct {
    u64 sig;
    u32 rev, size, crc, res;
} TABLE_HEADER;

typedef struct {
    u64 (*Reset)(void *self, u8 verify);
    u64 (*OutputString)(void *self, const CHAR16 *s);
} TEXT_OUT;

typedef struct {
    TABLE_HEADER hdr;
    void *RaiseTPL, *RestoreTPL;
    EFI_STATUS (*AllocatePages)(int type, int memtype, UINTN pages, u64 *addr);
    EFI_STATUS (*FreePages)(u64 addr, UINTN pages);
    EFI_STATUS (*GetMemoryMap)(UINTN *size, void *map, UINTN *key, UINTN *dsize, u32 *dver);
    void *AllocatePool, *FreePool;
    void *CreateEvent, *SetTimer, *WaitForEvent, *SignalEvent, *CloseEvent, *CheckEvent;
    void *InstallProtocolInterface, *ReinstallProtocolInterface, *UninstallProtocolInterface;
    EFI_STATUS (*HandleProtocol)(void *handle, const GUID *g, void **iface);
    void *Reserved, *RegisterProtocolNotify, *LocateHandle, *LocateDevicePath, *InstallConfigurationTable;
    void *LoadImage, *StartImage, *Exit, *UnloadImage;
    EFI_STATUS (*ExitBootServices)(void *image, UINTN key);
    void *GetNextMonotonicCount;
    EFI_STATUS (*Stall)(UINTN us);
    EFI_STATUS (*SetWatchdogTimer)(UINTN timeout, u64 code, UINTN size, CHAR16 *data);
    void *ConnectController, *DisconnectController, *OpenProtocol, *CloseProtocol, *OpenProtocolInformation;
    void *ProtocolsPerHandle, *LocateHandleBuffer;
    EFI_STATUS (*LocateProtocol)(const GUID *g, void *reg, void **iface);
} BOOT_SERVICES;

typedef struct {
    GUID vendor;
    void *table;
} CONFIG_TABLE;

typedef struct {
    TABLE_HEADER hdr;
    CHAR16 *vendor;
    u32 rev, pad;
    void *ConInHandle, *ConIn, *ConOutHandle;
    TEXT_OUT *ConOut;
    void *StdErrHandle, *StdErr, *Runtime;
    BOOT_SERVICES *BS;
    UINTN ntables;
    CONFIG_TABLE *tables;
} SYSTEM_TABLE;

typedef struct {
    u32 rev, pad;
    void *parent;
    SYSTEM_TABLE *st;
    void *device;                               /* the volume the loader came from */
} LOADED_IMAGE;

typedef struct EFI_FILE EFI_FILE;
struct EFI_FILE {
    u64 rev;
    EFI_STATUS (*Open)(EFI_FILE *self, EFI_FILE **out, const CHAR16 *name, u64 mode, u64 attr);
    EFI_STATUS (*Close)(EFI_FILE *self);
    void *Delete;
    EFI_STATUS (*Read)(EFI_FILE *self, UINTN *size, void *buf);
};

typedef struct {
    u64 rev;
    EFI_STATUS (*OpenVolume)(void *self, EFI_FILE **root);
} FILE_SYSTEM;

typedef struct {
    u32 version, hres, vres, format;
    u32 mask[4];
    u32 pixels_per_line;
} GOP_INFO;

typedef struct {
    u32 max_mode, mode;
    GOP_INFO *info;
    UINTN info_size;
    u64 fb_base;
    UINTN fb_size;
} GOP_MODE;

typedef struct {
    EFI_STATUS (*QueryMode)(void *self, u32 n, UINTN *size, GOP_INFO **info);
    EFI_STATUS (*SetMode)(void *self, u32 n);
    void *Blt;
    GOP_MODE *mode;
} GOP;

typedef struct {
    u32 type, pad;
    u64 phys, virt, pages, attr;
} MEM_DESC;

static const GUID LOADED_IMAGE_GUID = {0x5B1B31A1, 0x9562, 0x11d2, {0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const GUID FILE_SYSTEM_GUID = {0x964E5B22, 0x6459, 0x11D2, {0x8E, 0x39, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B}};
static const GUID GOP_GUID = {0x9042A9DE, 0x23DC, 0x4A38, {0x96, 0xFB, 0x7A, 0xDE, 0xD0, 0x80, 0x51, 0x6A}};
static const GUID EDID_ACTIVE_GUID = {0xBD8C1056, 0x9F36, 0x44EC, {0x92, 0xA8, 0xA6, 0x33, 0x7F, 0x81, 0x79, 0x86}};
static const GUID EDID_DISCOVERED_GUID = {0x1C0C34F6, 0xD380, 0x41FA, {0xA0, 0x49, 0x8A, 0xD0, 0x6C, 0x1A, 0x66, 0xAA}};
typedef struct {
    u32 size;
    u8 *edid;
} EDID_PROTO;
static const GUID ACPI20_GUID = {0x8868E871, 0xE4F1, 0x11D3, {0xBC, 0x22, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};
static const GUID ACPI10_GUID = {0xEB9D2D30, 0x2D88, 0x11D3, {0x9A, 0x16, 0x00, 0x80, 0xC7, 0x3C, 0x88, 0x81}};

/* clang may turn loops into calls to these */
void *memcpy(void *d, const void *s, UINTN n)
{
    u8 *dd = d;
    const u8 *ss = s;
    while (n--)
        *dd++ = *ss++;
    return d;
}

void *memset(void *d, int c, UINTN n)
{
    u8 *dd = d;
    while (n--)
        *dd++ = c;
    return d;
}

static int guid_eq(const GUID *a, const GUID *b)
{
    const u8 *x = (const u8 *)a, *y = (const u8 *)b;
    for (int i = 0; i < 16; i++)
        if (x[i] != y[i])
            return 0;
    return 1;
}

static SYSTEM_TABLE *ST;

static void print(const char *s)
{
    CHAR16 buf[96];
    int n = 0;
    for (; *s && n < 94; s++) {
        if (*s == '\n')
            buf[n++] = '\r';
        buf[n++] = (u8)*s;
    }
    buf[n] = 0;
    ST->ConOut->OutputString(ST->ConOut, buf);
}

static void fail(const char *msg)
{
    print("Petrichor: ");
    print(msg);
    print("\n");
    ST->BS->Stall(8000000);
}

#define KERNEL_ADDR 0x100000ull
#define MB_MAGIC_LOADER 0x2BADB002u

static u8 memmap[64 * 1024];

/* Usable RAM above 1 MiB as sorted, merged [start, end) ranges (firmware tables and device memory leave holes).
 * Written to out as {count, start0, end0, start1, end1, ...}; returns the highest end, in bytes. */
static u32 usable_ranges(UINTN map_size, UINTN dsize, u32 *out, u64 hole_start, u64 hole_end)
{
    static u64 start[256], end[256];                          /* static: a big stack frame would need __chkstk */
    int n = 0;
    for (UINTN off = 0; off + dsize <= map_size && n < 256; off += dsize) {
        const MEM_DESC *d = (const MEM_DESC *)(memmap + off);
        if (!(d->type == 1 || d->type == 2 || d->type == 3 || d->type == 4 || d->type == 7))
            continue;
        u64 s = d->phys, e = d->phys + d->pages * 4096;
        if (e <= KERNEL_ADDR)
            continue;
        if (s < KERNEL_ADDR)
            s = KERNEL_ADDR;
        int i = n++;
        while (i > 0 && start[i - 1] > s) {                  /* insertion sort by start */
            start[i] = start[i - 1];
            end[i] = end[i - 1];
            i--;
        }
        start[i] = s;
        end[i] = e;
    }
    int m = 0;
    for (int i = 0; i < n; i++) {                            /* merge touching regions */
        if (m && start[i] <= end[m - 1]) {
            if (end[i] > end[m - 1])
                end[m - 1] = end[i];
        } else {
            start[m] = start[i];
            end[m] = end[i];
            m++;
        }
    }
    u32 count = 0, top = 0;
    const u64 cap = 512ull * 1024 * 1024;
    for (int i = 0; i < m && count < 15; i++) {
        u64 s = start[i], e = end[i] > cap ? cap : end[i];
        for (int piece = 0; piece < 2; piece++) {                      /* cut the module (the pristine kernel copy) out */
            u64 ps = s, pe = e;
            if (hole_end > hole_start && s < hole_end && e > hole_start) {
                if (piece == 0) pe = hole_start > s ? hole_start : s;
                else ps = hole_end < e ? hole_end : e;
            } else if (piece == 1) {
                break;
            }
            if (ps >= cap || pe <= ps || pe - ps < 64 * 1024 || count >= 15)
                continue;
            out[1 + count * 2] = (u32)ps;
            out[2 + count * 2] = (u32)pe;
            count++;
            top = (u32)pe;
        }
    }
    out[0] = count;
    return top;
}

EFI_STATUS efi_main(void *image, SYSTEM_TABLE *st)
{
    ST = st;
    BOOT_SERVICES *bs = st->BS;
    bs->SetWatchdogTimer(0, 0, 0, NULL);
    print("Petrichor: loading the kernel...\n");

    /* the volume we were started from */
    LOADED_IMAGE *li;
    FILE_SYSTEM *fs;
    EFI_FILE *root, *kf;
    if (bs->HandleProtocol(image, &LOADED_IMAGE_GUID, (void **)&li) != EFI_SUCCESS ||
        bs->HandleProtocol(li->device, &FILE_SYSTEM_GUID, (void **)&fs) != EFI_SUCCESS ||
        fs->OpenVolume(fs, &root) != EFI_SUCCESS) {
        fail("can't open the boot volume");
        return ERR(1);
    }
    static const CHAR16 kname[] = {'K', 'E', 'R', 'N', 'E', 'L', '.', 'B', 'I', 'N', 0};
    if (root->Open(root, &kf, kname, 1, 0) != EFI_SUCCESS) {
        fail("KERNEL.BIN not found on the boot volume");
        return ERR(2);
    }

    /* header: where the image ends (including .bss) and the 64-bit entry point */
    u8 hdr[64];
    UINTN n = sizeof hdr;
    if (kf->Read(kf, &n, hdr) != EFI_SUCCESS || n < 36 || *(u32 *)hdr != 0x1BADB002u) {
        fail("KERNEL.BIN is not a Petrichor kernel");
        return ERR(3);
    }
    u32 bss_end = *(u32 *)(hdr + 24), entry64 = *(u32 *)(hdr + 32);   /* bss_end_addr, and our own _start64 word */
    if (bss_end <= KERNEL_ADDR || bss_end > KERNEL_ADDR + 64 * 1024 * 1024) {
        fail("bad kernel header");
        return ERR(3);
    }
    u64 pages = (bss_end - KERNEL_ADDR + 4095) / 4096, addr = KERNEL_ADDR;
    if (bs->AllocatePages(2 /* AllocateAddress */, 2 /* LoaderData */, pages, &addr) != EFI_SUCCESS) {
        fail("can't reserve memory at 1 MiB for the kernel");
        return ERR(4);
    }
    u8 *dst = (u8 *)KERNEL_ADDR;
    memcpy(dst, hdr, n);
    UINTN pos = n;
    for (;;) {                                               /* the rest of the file */
        UINTN chunk = 512 * 1024;
        if (kf->Read(kf, &chunk, dst + pos) != EFI_SUCCESS) {
            fail("error reading KERNEL.BIN");
            return ERR(5);
        }
        if (chunk == 0)
            break;
        pos += chunk;
        if (pos >= bss_end - KERNEL_ADDR) {
            fail("KERNEL.BIN is larger than its header says");
            return ERR(5);
        }
    }
    kf->Close(kf);
    UINTN kernel_file_size = pos;

    /* a pristine copy of the kernel for the installer (the running kernel's data changes as it works) */
    u64 kimg = 0x3FFFFFFF;
    if (bs->AllocatePages(1 /* AllocateMaxAddress */, 2, (kernel_file_size + 4095) / 4096, &kimg) != EFI_SUCCESS)
        kimg = 0;
    else
        memcpy((void *)kimg, (void *)KERNEL_ADDR, kernel_file_size);

    /* an optional CMDLINE.TXT on the boot volume chooses the mode ("mode=text"); "uefi" is always appended */
    static char cmdline_buf[128];
    int cl = 0;
    {
        static const CHAR16 cname[] = {'C', 'M', 'D', 'L', 'I', 'N', 'E', '.', 'T', 'X', 'T', 0};
        EFI_FILE *cf;
        if (root->Open(root, &cf, cname, 1, 0) == EFI_SUCCESS) {
            UINTN cn = 100;
            if (cf->Read(cf, &cn, cmdline_buf) == EFI_SUCCESS)
                for (UINTN i = 0; i < cn && cmdline_buf[i] != '\n' && cmdline_buf[i] != '\r'; i++)
                    cl++;
            cf->Close(cf);
        }
        if (!cl) {
            static const char dflt[] = "mode=desktop";
            memcpy(cmdline_buf, dflt, sizeof dflt - 1);
            cl = sizeof dflt - 1;
        }
        static const char tail[] = " uefi";
        memcpy(cmdline_buf + cl, tail, sizeof tail);
    }

    /* graphics: read the monitor's preferred resolution (EDID) and take the largest BGR mode that fits in it
     * (1024x768 if the monitor doesn't say; never above 2560x1600) */
    u32 want_w = 1024, want_h = 768, mon_w = 0, mon_h = 0, mon_mhz = 0;
    {
        EDID_PROTO *ep = NULL;
        if (bs->LocateProtocol(&EDID_ACTIVE_GUID, NULL, (void **)&ep) != EFI_SUCCESS || !ep || !ep->size || !ep->edid)
            if (bs->LocateProtocol(&EDID_DISCOVERED_GUID, NULL, (void **)&ep) != EFI_SUCCESS || !ep || !ep->size || !ep->edid)
                ep = NULL;
        if (ep && ep->size >= 128) {
            const u8 *e = ep->edid;
            u32 clk = e[54] | (e[55] << 8);
            u32 hact = e[56] | ((e[58] & 0xF0) << 4), vact = e[59] | ((e[61] & 0xF0) << 4);
            u32 htot = hact + (e[57] | ((e[58] & 0x0F) << 8)), vtot = vact + (e[60] | ((e[61] & 0x0F) << 8));
            if (e[0] == 0 && e[1] == 0xFF && e[7] == 0 && clk && hact >= 640 && vact >= 480 && hact <= 8192 && vact <= 8192) {
                mon_w = hact;
                mon_h = vact;
                if (htot && vtot)
                    mon_mhz = (u32)((u64)clk * 10000000ull / ((u64)htot * vtot));
                want_w = hact > 2560 ? 2560 : hact;
                want_h = vact > 1600 ? 1600 : vact;
            }
        }
    }
    GOP *gop;
    u32 fb_w = 0, fb_h = 0, fb_pitch = 0;
    u64 fb_base = 0;
    if (bs->LocateProtocol(&GOP_GUID, NULL, (void **)&gop) == EFI_SUCCESS) {
        if (!mon_w) {                                       /* no EDID: the mode the firmware is already in is usually the panel's own */
            GOP_INFO *cur = gop->mode->info;
            if (cur->hres >= 1024 && cur->vres >= 768 && cur->hres <= 2560 && cur->vres <= 1600) {
                want_w = cur->hres;
                want_h = cur->vres;
            }
        }
        u32 best = 0, best_area = 0;
        for (u32 i = 0; i < gop->mode->max_mode; i++) {
            GOP_INFO *gi;
            UINTN isz;
            if (gop->QueryMode(gop, i, &isz, &gi) != EFI_SUCCESS)
                continue;
            u32 area = gi->hres * gi->vres;
            if (gi->format == 1 && gi->hres <= want_w && gi->vres <= want_h && area > best_area) {
                best = i;
                best_area = area;
            }
        }
        if (best_area && best != gop->mode->mode)
            gop->SetMode(gop, best);
        GOP_INFO *gi = gop->mode->info;
        if (gi->format <= 1) {
            fb_w = gi->hres;
            fb_h = gi->vres;
            fb_pitch = gi->pixels_per_line * 4;
            fb_base = gop->mode->fb_base;
        }
    }

    /* the ACPI tables, for power off */
    u64 rsdp = 0;
    for (UINTN i = 0; i < st->ntables; i++) {
        if (guid_eq(&st->tables[i].vendor, &ACPI20_GUID)) {
            rsdp = (u64)st->tables[i].table;
            break;
        }
        if (guid_eq(&st->tables[i].vendor, &ACPI10_GUID))
            rsdp = (u64)st->tables[i].table;
    }

    /* the information block lives below 640 KiB, where the kernel never allocates */
    u64 info_addr = 0x80000;
    if (bs->AllocatePages(1 /* AllocateMaxAddress */, 2, 1, &info_addr) != EFI_SUCCESS) {
        fail("can't allocate the boot information block");
        return ERR(6);
    }
    u32 *mbi = (u32 *)info_addr;
    memset(mbi, 0, 4096);
    char *cmdline = (char *)mbi + 512;
    memcpy(cmdline, cmdline_buf, 128);
    u32 *module = mbi + 512;                                  /* the Multiboot module list entry, at byte 2048 */
    if (kimg) {
        module[0] = (u32)kimg;
        module[1] = (u32)(kimg + kernel_file_size);
    }

    /* leave the firmware: the memory map must be current when ExitBootServices is called */
    UINTN map_size, key, dsize;
    u32 dver;
    for (int tries = 0; tries < 8; tries++) {
        map_size = sizeof memmap;
        if (bs->GetMemoryMap(&map_size, memmap, &key, &dsize, &dver) != EFI_SUCCESS) {
            fail("can't read the memory map");
            return ERR(7);
        }
        if (bs->ExitBootServices(image, key) == EFI_SUCCESS)
            goto exited;
    }
    fail("ExitBootServices failed");
    return ERR(8);

exited:
    __asm__ volatile("cli");
    u32 top = usable_ranges(map_size, dsize, mbi + 256, kimg, kimg ? kimg + ((kernel_file_size + 4095) & ~4095ull) : 0);      /* the memory ranges, at byte 1024 of the block */
    mbi[0] = 1 | 4 | (kimg ? 8 : 0) | (1u << 12) | 0x40000000u | (rsdp ? 0x80000000u : 0);
    mbi[5] = kimg ? 1 : 0;
    mbi[6] = kimg ? (u32)(u64)module : 0;
    mbi[1] = 640;
    mbi[2] = top > KERNEL_ADDR ? (top - KERNEL_ADDR) / 1024 : 31 * 1024;
    mbi[4] = (u32)(u64)cmdline;
    if (fb_base) {
        mbi[22] = (u32)fb_base;                               /* framebuffer_addr (low half) */
        mbi[24] = fb_pitch;
        mbi[25] = fb_w;
        mbi[26] = fb_h;
        ((u8 *)mbi)[108] = 32;
        ((u8 *)mbi)[109] = 1;                                 /* direct RGB */
    } else {
        mbi[0] &= ~(1u << 12);
    }
    mbi[30] = (u32)rsdp;
    if (mon_w) {
        mbi[32] = mon_w;
        mbi[33] = mon_h;
        mbi[34] = mon_mhz;
        mbi[35] = 0x44494445;                                   /* "EDID" */
    }

    ((void (*)(u64, u64))(u64)entry64)(MB_MAGIC_LOADER, (u64)mbi);
    for (;;)
        __asm__ volatile("hlt");
}
