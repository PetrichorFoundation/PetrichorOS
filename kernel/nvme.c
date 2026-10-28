/* NVMe driver: one admin queue and one I/O queue (16 entries each), polled.
 * Transfers go through an 8 KiB bounce buffer described by PRP1/PRP2. */
#include "kernel.h"

#define QSIZE 16

struct queue {
    u32 *sq, *cq;
    volatile u32 *sq_db, *cq_db;
    u32 sqt, cqh, phase;
    u16 cid;
};

struct nvme {
    volatile u32 *regs;
    struct queue admin, io;
    u8 *pg_a, *pg_b;                 /* bounce pages */
    u32 nsid;
};

static int submit(struct queue *q, u32 cmd[16])
{
    cmd[0] = (cmd[0] & 0xFFFF) | ((u32)++q->cid << 16);
    memcpy(q->sq + q->sqt * 16, cmd, 64);
    q->sqt = (q->sqt + 1) % QSIZE;
    *q->sq_db = q->sqt;
    for (int i = 0; i < 5000000; i++) {
        volatile u32 *e = q->cq + q->cqh * 4;
        if (((e[3] >> 16) & 1) == q->phase) {
            u32 status = e[3] >> 17;
            if (++q->cqh == QSIZE) {
                q->cqh = 0;
                q->phase ^= 1;
            }
            *q->cq_db = q->cqh;
            return status ? -EIO : 0;
        }
    }
    return -EIO;
}

static int nvme_rw(void *priv, u32 lba, u32 n, void *buf, int write)
{
    struct nvme *d = priv;
    u8 *b = buf;
    while (n) {
        u32 chunk = n > 16 ? 16 : n, bytes = chunk * 512;
        if (write) {
            memcpy(d->pg_a, b, bytes > 4096 ? 4096 : bytes);
            if (bytes > 4096)
                memcpy(d->pg_b, b + 4096, bytes - 4096);
        }
        u32 cmd[16] = {0};
        cmd[0] = write ? 0x01 : 0x02;
        cmd[1] = d->nsid;
        cmd[6] = (u32)d->pg_a;
        cmd[8] = bytes > 4096 ? (u32)d->pg_b : 0;
        cmd[10] = lba;
        cmd[11] = 0;
        cmd[12] = chunk - 1;
        if (submit(&d->io, cmd) < 0)
            return -EIO;
        if (!write) {
            memcpy(b, d->pg_a, bytes > 4096 ? 4096 : bytes);
            if (bytes > 4096)
                memcpy(b + 4096, d->pg_b, bytes - 4096);
        }
        lba += chunk;
        b += bytes;
        n -= chunk;
    }
    return 0;
}

static int wait_ready(volatile u32 *regs, u32 want)
{
    for (int i = 0; i < 2000000; i++) {
        u32 csts = regs[0x1C / 4];
        if (csts & 2)                                           /* fatal status */
            return -1;
        if ((csts & 1) == want)
            return 0;
    }
    return -1;
}

static int attach(struct pcidev *dev)
{
    pci_enable(dev);
    u32 bar0 = pci_cfg_read(dev, 0x10);
    if ((bar0 & 6) == 4 && pci_cfg_read(dev, 0x14))
        return 0;                                               /* 64-bit BAR above 4 GiB */
    volatile u32 *regs = mmio_map(bar0 & ~0xFu, 0x2000);
    if (!regs)
        return 0;
    u32 stride = 4u << (regs[1] & 0xF);                         /* CAP.DSTRD */

    struct nvme *d = kzalloc(sizeof *d);
    if (!d)
        return 0;
    d->regs = regs;
    d->nsid = 1;

    if (regs[0x14 / 4] & 1) {                                   /* reset the controller */
        regs[0x14 / 4] = 0;
        if (wait_ready(regs, 0) < 0)
            return 0;
    }
    u32 asq = pmm_alloc(), acq = pmm_alloc(), isq = pmm_alloc(), icq = pmm_alloc();
    u32 idp = pmm_alloc();
    d->pg_a = (u8 *)pmm_alloc();
    d->pg_b = (u8 *)pmm_alloc();
    if (!asq || !acq || !isq || !icq || !idp || !d->pg_a || !d->pg_b)
        return 0;

    regs[0x24 / 4] = ((QSIZE - 1) << 16) | (QSIZE - 1);         /* AQA */
    regs[0x28 / 4] = asq;
    regs[0x2C / 4] = 0;
    regs[0x30 / 4] = acq;
    regs[0x34 / 4] = 0;
    regs[0x14 / 4] = (6u << 16) | (4u << 20) | 1;               /* 64-byte SQ entries, 16-byte CQ entries, enable */
    if (wait_ready(regs, 1) < 0)
        return 0;

    d->admin = (struct queue){.sq = (u32 *)asq, .cq = (u32 *)acq, .phase = 1,
                              .sq_db = (volatile u32 *)((u8 *)regs + 0x1000),
                              .cq_db = (volatile u32 *)((u8 *)regs + 0x1000 + stride)};
    d->io = (struct queue){.sq = (u32 *)isq, .cq = (u32 *)icq, .phase = 1,
                           .sq_db = (volatile u32 *)((u8 *)regs + 0x1000 + 2 * stride),
                           .cq_db = (volatile u32 *)((u8 *)regs + 0x1000 + 3 * stride)};

    u32 cmd[16] = {0};
    cmd[0] = 0x05;                                              /* create I/O completion queue 1 */
    cmd[6] = icq;
    cmd[10] = ((QSIZE - 1) << 16) | 1;
    cmd[11] = 1;                                                /* physically contiguous */
    if (submit(&d->admin, cmd) < 0)
        return 0;
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x01;                                              /* create I/O submission queue 1 */
    cmd[6] = isq;
    cmd[10] = ((QSIZE - 1) << 16) | 1;
    cmd[11] = (1u << 16) | 1;                                   /* completes on CQ 1 */
    if (submit(&d->admin, cmd) < 0)
        return 0;

    u8 *id = (u8 *)idp;
    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x06;                                              /* identify controller (for the model name) */
    cmd[6] = idp;
    cmd[10] = 1;
    if (submit(&d->admin, cmd) < 0)
        return 0;
    char model[41];
    memcpy(model, id + 24, 40);
    model[40] = 0;
    for (int i = 39; i >= 0 && (model[i] == ' ' || !model[i]); i--)
        model[i] = 0;

    memset(cmd, 0, sizeof cmd);
    cmd[0] = 0x06;                                              /* identify namespace 1 */
    cmd[1] = 1;
    cmd[6] = idp;
    cmd[10] = 0;
    if (submit(&d->admin, cmd) < 0)
        return 0;
    u32 nsze_lo = *(u32 *)id, nsze_hi = *(u32 *)(id + 4);
    u8 flbas = id[26] & 0xF;
    u32 lbaf = *(u32 *)(id + 128 + flbas * 4);
    if (((lbaf >> 16) & 0xFF) != 9 || !nsze_lo)                 /* only 512-byte sectors */
        return 0;
    return blk_register(model, nsze_hi ? 0xFFFFFFFFu : nsze_lo, BUS_NVME, nvme_rw, d) >= 0;
}

int nvme_probe(void)
{
    int found = 0;
    struct pcidev dev;
    for (int i = 0; pci_find_class(0x010802, i, &dev) == 0; i++)
        found += attach(&dev);
    return found;
}
