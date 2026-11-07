/* USB 3 / USB 2 through xHCI host controllers (PCI class 0C03, prog-if 30).
 *
 * What it does: takes the controller from the firmware, resets it, enumerates the devices plugged into the
 * root ports at boot, and drives
 *   - HID boot-protocol keyboards and mice, fed into the same paths as the PS/2 ones,
 *   - mass-storage (flash drives, USB disks) using the Bulk-Only protocol with SCSI, registered as disks.
 * Everything is polled (no interrupts): commands and transfers wait by polling the event ring, and the timer
 * tick polls it for keyboard and mouse reports.  Not supported: hubs, hot-plugging, isochronous devices,
 * USB-attached network or Wi-Fi adapters, and non-boot HID devices (touchscreens, tablets, gamepads). */
#include "kernel.h"

#define NRING   256
#define NXHCI   3
#define NDEV    16                       /* device slots used per controller */
#define MAXFN   4
#define BOT_CHUNK 64                     /* sectors per transfer (32 KiB) */

struct trb {
    u32 lo, hi, st, ctl;
};

struct ring {
    volatile struct trb *t;
    u32 enq, cycle;
};

struct xdev;

enum { FN_NONE, FN_KBD, FN_MOUSE, FN_MSC, FN_HID };   /* FN_HID: report-protocol device, e.g. a tablet */

struct fn {
    int kind, iface;
    struct xdev *d;
    u8 in_ep, out_ep, in_ival, in_burst, out_burst;
    u16 in_mps, out_mps;
    int in_dci, out_dci;
    u8 *buf;                             /* HID: the report; MSC: CBW (32) + CSW (16) */
    u8 *dbuf;                            /* MSC: data bounce buffer */
    u32 tag;
    u8 prev[8];                          /* keyboard: the previous report */
    u16 rdesc_len;                       /* FN_HID: report descriptor length, then the layout found in it */
    struct { u8 id; u16 btn_off, btn_n, x_off, y_off, w_off; u8 x_size, y_size, w_size, absolute; i32 xmax, ymax; } lay;
    int errors;
};

struct xep {
    struct ring r;
    volatile int done, code;
    volatile u32 resid;
    struct fn *fn;                       /* HID endpoint polled in the background */
};

struct xhci;

struct xdev {
    struct xhci *x;
    int used, slot, port, speed;
    u8 *out_ctx, *in_ctx, *cbuf;
    struct xep ep[32];
    struct fn fn[MAXFN];
    int nfn;
};

struct xhci {
    volatile u8 *cap, *opr, *rt, *dbr;
    u32 cs;                              /* context size: 32 or 64 */
    int max_slots, max_ports;
    u64 *dcbaa;
    struct ring cmd, ev;
    u32 ev_deq, ev_cycle;
    volatile int cmd_done, cmd_code, cmd_slot;
    struct xdev dev[NDEV + 1];
    int busy;                            /* a synchronous operation is in progress: the timer tick keeps out */
    int live;
};

static struct xhci xs[NXHCI];
static int nx;

static inline u32 rd(volatile u8 *p, u32 off) { return *(volatile u32 *)(p + off); }
static inline void wr(volatile u8 *p, u32 off, u32 v) { *(volatile u32 *)(p + off) = v; }

static void udelay(u32 us)
{
    for (u32 i = 0; i < us; i++)
        inb(0x80);                       /* about a microsecond per read */
}

static inline u64 irq_save(void)
{
    u64 f;
    __asm__ volatile("pushfq; popq %0; cli" : "=r"(f) : : "memory");
    return f;
}

static inline void irq_restore(u64 f)
{
    if (f & 0x200)
        __asm__ volatile("sti" : : : "memory");
}

/* ---------------- rings ---------------- */

static int ring_init(struct ring *r)
{
    r->t = kalloc_aligned(NRING * sizeof(struct trb), 4096);
    if (!r->t)
        return -1;
    r->enq = 0;
    r->cycle = 1;
    volatile struct trb *l = &r->t[NRING - 1];             /* link back to the start */
    l->lo = (u32)(uptr)r->t;
    l->hi = 0;
    l->st = 0;
    l->ctl = (6 << 10) | 2;                                /* LINK, toggle cycle */
    return 0;
}

static void ring_push(struct ring *r, u32 lo, u32 hi, u32 st, u32 ctl)
{
    volatile struct trb *t = &r->t[r->enq];
    t->lo = lo;
    t->hi = hi;
    t->st = st;
    __asm__ volatile("" : : : "memory");
    t->ctl = (ctl & ~1u) | r->cycle;
    if (++r->enq == NRING - 1) {
        volatile struct trb *l = &r->t[NRING - 1];
        l->ctl = (l->ctl & ~1u) | r->cycle;
        r->enq = 0;
        r->cycle ^= 1;
    }
}


/* ---------------- events ---------------- */

static void kbd_report(struct fn *f, const u8 *r);
static void hid_report(struct fn *f, const u8 *r, int len);
static int parse_rdesc(struct fn *f, const u8 *d, int n);
static void mouse_report(const u8 *r, int len);

static void hid_arm(struct xdev *d, struct fn *f)
{
    struct xep *e = &d->ep[f->in_dci];
    u32 len = f->in_mps > 64 ? 64 : f->in_mps;
    ring_push(&e->r, (u32)(uptr)f->buf, 0, len, (1 << 10) | (1 << 5) | (1 << 2));   /* NORMAL, IOC, ISP */
    wr(d->x->dbr, d->slot * 4, f->in_dci);
}

static void on_transfer(struct xhci *x, u32 slot, u32 dci, u32 code, u32 resid)
{
    if (slot == 0 || slot > NDEV || !x->dev[slot].used || dci >= 32)
        return;
    struct xdev *d = &x->dev[slot];
    struct xep *e = &d->ep[dci];
    if (e->fn && e->fn->kind != FN_MSC) {
        struct fn *f = e->fn;
        if (code == 1 || code == 13) {
            int got = (f->in_mps > 64 ? 64 : f->in_mps) - (int)resid;
            if (f->kind == FN_KBD)
                kbd_report(f, f->buf);
            else if (f->kind == FN_HID)
                hid_report(f, f->buf, got);
            else
                mouse_report(f->buf, got);
            f->errors = 0;
        } else if (++f->errors > 50) {
            return;                                        /* the device is misbehaving: stop polling it */
        }
        hid_arm(d, f);
        return;
    }
    e->code = code;
    e->resid = resid;
    e->done = 1;
}

static void poll_events(struct xhci *x)
{
    u64 fl = irq_save();
    int any = 0;
    for (;;) {
        volatile struct trb *t = &x->ev.t[x->ev_deq];
        if ((t->ctl & 1) != x->ev_cycle)
            break;
        u32 lo = t->lo, st = t->st, ctl = t->ctl;
        (void)lo;
        if (++x->ev_deq == NRING) {
            x->ev_deq = 0;
            x->ev_cycle ^= 1;
        }
        any = 1;
        u32 type = (ctl >> 10) & 63;
        if (type == 33) {                                  /* command completion */
            x->cmd_code = st >> 24;
            x->cmd_slot = ctl >> 24;
            x->cmd_done = 1;
        } else if (type == 32) {                           /* transfer */
            on_transfer(x, ctl >> 24, (ctl >> 16) & 31, st >> 24, st & 0xFFFFFF);
        }
    }
    if (any) {
        u32 p = (u32)(uptr)&x->ev.t[x->ev_deq];
        wr(x->rt, 0x20 + 0x18, p | 8);                     /* ERDP, clearing "event handler busy" */
        wr(x->rt, 0x20 + 0x1C, 0);
    }
    irq_restore(fl);
}

/* Runs a command and waits for it.  Returns the completion code (1 = success) or -1 on timeout. */
static int command(struct xhci *x, u32 lo, u32 st, u32 ctl, int *slot_out)
{
    x->cmd_done = 0;
    ring_push(&x->cmd, lo, 0, st, ctl);
    wr(x->dbr, 0, 0);
    for (int i = 0; i < 100000 && !x->cmd_done; i++) {
        poll_events(x);
        if (!x->cmd_done)
            udelay(20);
    }
    if (!x->cmd_done)
        return -1;
    if (slot_out)
        *slot_out = x->cmd_slot;
    return x->cmd_code;
}

static int xfer_wait(struct xdev *d, int dci, int ms)
{
    struct xep *e = &d->ep[dci];
    for (int i = 0; i < ms * 20 && !e->done; i++) {
        poll_events(d->x);
        if (!e->done)
            udelay(50);
    }
    return e->done ? e->code : -1;
}

/* ---------------- control transfers ---------------- */

static int ctrl(struct xdev *d, u8 rt, u8 req, u16 val, u16 idx, u16 len, void *buf, int in)
{
    struct xep *e = &d->ep[1];
    u32 trt = len ? (in ? 3 : 2) : 0;
    e->done = 0;
    ring_push(&e->r, rt | (req << 8) | ((u32)val << 16), idx | ((u32)len << 16), 8, (2 << 10) | (1 << 6) | (trt << 16));
    if (len)
        ring_push(&e->r, (u32)(uptr)buf, 0, len, (3 << 10) | (in ? 1 << 16 : 0));
    ring_push(&e->r, 0, 0, 0, (4 << 10) | (1 << 5) | ((len && in) ? 0 : 1 << 16));
    wr(d->x->dbr, d->slot * 4, 1);
    int c = xfer_wait(d, 1, 1000);
    return c == 1 || c == 13 ? 0 : -EIO;
}

static int get_desc(struct xdev *d, u8 type, u8 index, u16 len)
{
    memset(d->cbuf, 0, len);
    return ctrl(d, 0x80, 6, (type << 8) | index, 0, len, d->cbuf, 1);
}

/* ---------------- device setup ---------------- */

static u32 *ictx_slot(struct xdev *d) { return (u32 *)(d->in_ctx + d->x->cs); }
static u32 *ictx_ep(struct xdev *d, int dci) { return (u32 *)(d->in_ctx + (1 + dci) * d->x->cs); }
static u32 *ictx_ctl(struct xdev *d) { return (u32 *)d->in_ctx; }

static void ep_context(struct xdev *d, int dci, int type, u32 mps, u32 interval, u32 burst, u32 avg)
{
    u32 *c = ictx_ep(d, dci);
    struct ring *r = &d->ep[dci].r;
    for (int i = 0; i < 8; i++)
        c[i] = 0;
    c[0] = interval << 16;
    c[1] = (3 << 1) | (type << 3) | (burst << 8) | (mps << 16);
    c[2] = ((u32)(uptr)r->t) | 1;                          /* dequeue pointer + cycle state */
    c[3] = 0;
    c[4] = avg | (type == 7 ? (mps * (burst + 1)) << 16 : 0);      /* average TRB length; max ESIT payload (interrupt only) */
}

static int parse_config(struct xdev *d, const u8 *p, int total)
{
    int off = 0, cur = -1;
    struct fn *f = NULL;
    int last_ep_is_in = 0;
    while (off + 2 <= total && p[off] >= 2) {
        u8 len = p[off], type = p[off + 1];
        if (off + len > total)
            break;
        if (type == 4 && len >= 9) {                        /* interface */
            u8 cls = p[off + 5], sub = p[off + 6], proto = p[off + 7];
            f = NULL;
            int kind = FN_NONE;
            if (cls == 3 && sub == 1 && proto == 1)
                kind = FN_KBD;
            else if (cls == 3 && sub == 1 && proto == 2)
                kind = FN_MOUSE;
            else if (cls == 8 && sub == 6 && proto == 0x50)
                kind = FN_MSC;
            else if (cls == 3)
                kind = FN_HID;                                  /* decided once its report descriptor is read */
            if (kind && p[off + 3] == 0 && d->nfn < MAXFN) {   /* alternate setting 0 only */
                f = &d->fn[d->nfn++];
                memset(f, 0, sizeof *f);
                f->kind = kind;
                f->iface = p[off + 2];
                f->d = d;
                cur = d->nfn - 1;
            }
        } else if (type == 5 && len >= 7 && f) {             /* endpoint */
            u8 addr = p[off + 2], attr = p[off + 3] & 3;
            u16 mps = p[off + 4] | (p[off + 5] << 8);
            last_ep_is_in = addr & 0x80;
            int dci = (addr & 15) * 2 + (addr & 0x80 ? 1 : 0);
            if (f->kind == FN_MSC && attr == 2) {
                if (addr & 0x80) { f->in_ep = addr; f->in_mps = mps & 0x7FF; f->in_dci = dci; }
                else { f->out_ep = addr; f->out_mps = mps & 0x7FF; f->out_dci = dci; }
            } else if (f->kind != FN_MSC && attr == 3 && (addr & 0x80) && !f->in_ep) {
                f->in_ep = addr; f->in_mps = mps & 0x7FF; f->in_ival = p[off + 6]; f->in_dci = dci;
            }
        } else if (type == 0x21 && len >= 9 && f && f->kind == FN_HID) {   /* HID descriptor */
            if (p[off + 6] == 0x22)
                f->rdesc_len = p[off + 7] | (p[off + 8] << 8);
        } else if (type == 0x30 && len >= 6 && f) {          /* SuperSpeed endpoint companion */
            if (last_ep_is_in) f->in_burst = p[off + 2]; else f->out_burst = p[off + 2];
        }
        off += len;
    }
    (void)cur;
    return d->nfn;
}

static u32 interval_for(int speed, u8 binterval)
{
    if (speed >= 3) {                                        /* high / super speed: already 2^(n-1) microframes */
        u32 v = binterval ? binterval - 1 : 0;
        return v > 15 ? 15 : v;
    }
    u32 n = 0;                                               /* low / full speed: milliseconds -> 125 us units */
    while ((1u << n) < binterval)
        n++;
    n += 3;
    return n > 15 ? 15 : n;
}

static int scsi_cmd(struct fn *f, const u8 *cdb, int cdblen, u8 *data, u32 len, int in);

static void msc_setup(struct xdev *d, struct fn *f);

static int enumerate(struct xhci *x, int port, int speed)
{
    int slot = 0;
    if (command(x, 0, 0, 9 << 10, &slot) != 1 || slot <= 0 || slot > NDEV)
        return -1;
    struct xdev *d = &x->dev[slot];
    memset(d, 0, sizeof *d);
    d->x = x;
    d->slot = slot;
    d->port = port;
    d->speed = speed;
    d->out_ctx = kalloc_aligned(x->cs * 32, 64);
    d->in_ctx = kalloc_aligned(x->cs * 33, 64);
    d->cbuf = kalloc_aligned(1024, 64);
    if (!d->out_ctx || !d->in_ctx || !d->cbuf || ring_init(&d->ep[1].r) < 0)
        return -1;
    x->dcbaa[slot] = (u64)(uptr)d->out_ctx;

    u32 mps0 = speed == 4 || speed == 5 ? 512 : speed == 3 ? 64 : 8;
    ictx_ctl(d)[1] = 3;                                      /* add slot + EP0 */
    u32 *sc = ictx_slot(d);
    sc[0] = ((u32)speed << 20) | (1u << 27);
    sc[1] = (u32)(port + 1) << 16;
    ep_context(d, 1, 4, mps0, 0, 0, 8);
    int c = command(x, (u32)(uptr)d->in_ctx, 0, (11 << 10) | ((u32)slot << 24), NULL);
    if (c != 1) {
        kprintf("[usb ] port %d: address device failed (%d)\n", port + 1, c);
        return -1;
    }
    d->used = 1;

    if (get_desc(d, 1, 0, 8) < 0) {
        kprintf("[usb ] port %d: no answer to GET_DESCRIPTOR\n", port + 1);
        return -1;
    }
    u32 real_mps = d->cbuf[7];
    if (speed >= 4)
        real_mps = real_mps < 16 ? 1u << real_mps : 512;
    if (real_mps && real_mps != mps0 && speed <= 2) {         /* full/low speed: now we know EP0's real packet size */
        ictx_ctl(d)[1] = 2;
        ep_context(d, 1, 4, real_mps, 0, 0, 8);
        d->ep[1].r.enq = d->ep[1].r.enq;                     /* ring is unchanged; Evaluate Context only edits MPS */
        command(x, (u32)(uptr)d->in_ctx, 0, (13 << 10) | ((u32)slot << 24), NULL);
    }
    if (get_desc(d, 1, 0, 18) < 0)
        return -1;
    u8 dclass = d->cbuf[4];
    if (dclass == 9) {
        kprintf("[usb ] port %d: a hub; hubs are not supported, devices behind it are ignored\n", port + 1);
        return -1;
    }
    if (get_desc(d, 2, 0, 9) < 0)
        return -1;
    u32 total = d->cbuf[2] | (d->cbuf[3] << 8), cfgval = d->cbuf[5];
    if (total > 1000)
        total = 1000;
    if (get_desc(d, 2, 0, total) < 0)
        return -1;
    parse_config(d, d->cbuf, total);
    if (!d->nfn) {
        kprintf("[usb ] port %d: device class %02x is not supported (only HID keyboards/mice and mass storage)\n",
                port + 1, dclass);
        return -1;
    }
    if (ctrl(d, 0x00, 9, cfgval, 0, 0, NULL, 0) < 0)         /* SET_CONFIGURATION */
        return -1;

    /* configure the endpoints we use */
    u32 add = 1, maxdci = 1;
    for (int i = 0; i < d->nfn; i++) {
        struct fn *f = &d->fn[i];
        int dcis[2] = {f->in_dci, f->kind == FN_MSC ? f->out_dci : 0};
        if (!f->in_dci || (f->kind == FN_MSC && !f->out_dci)) {
            f->kind = FN_NONE;
            continue;
        }
        for (int k = 0; k < 2; k++) {
            int dci = dcis[k];
            if (!dci)
                continue;
            if (ring_init(&d->ep[dci].r) < 0)
                return -1;
            add |= 1u << dci;
            if (dci > (int)maxdci)
                maxdci = dci;
            if (f->kind == FN_MSC)
                ep_context(d, dci, k == 0 ? 6 : 2, k == 0 ? f->in_mps : f->out_mps, 0, k == 0 ? f->in_burst : f->out_burst, 1024);
            else
                ep_context(d, dci, 7, f->in_mps, interval_for(speed, f->in_ival), f->in_burst, f->in_mps);
        }
        f->buf = kalloc_aligned(64, 64);
        if (!f->buf)
            return -1;
        d->ep[f->in_dci].fn = f;
        if (f->kind == FN_MSC)
            d->ep[f->out_dci].fn = f;
    }
    ictx_ctl(d)[0] = 0;
    ictx_ctl(d)[1] = add;
    sc[0] = (sc[0] & 0x07FFFFFF) | (maxdci << 27);
    c = command(x, (u32)(uptr)d->in_ctx, 0, (12 << 10) | ((u32)slot << 24), NULL);
    if (c != 1) {
        kprintf("[usb ] port %d: configure endpoints failed (%d)\n", port + 1, c);
        return -1;
    }
    for (int i = 0; i < d->nfn; i++) {
        struct fn *f = &d->fn[i];
        if (f->kind == FN_KBD || f->kind == FN_MOUSE) {
            ctrl(d, 0x21, 0x0B, 0, f->iface, 0, NULL, 0);    /* SET_PROTOCOL: boot */
            ctrl(d, 0x21, 0x0A, 0, f->iface, 0, NULL, 0);    /* SET_IDLE: report only on change */
            hid_arm(d, f);
            kprintf("[usb ] port %d: %s\n", port + 1, f->kind == FN_KBD ? "keyboard" : "mouse");
            x->live = 1;
        } else if (f->kind == FN_HID && f->in_dci) {
            u16 n = f->rdesc_len && f->rdesc_len <= 1000 ? f->rdesc_len : 512;
            memset(d->cbuf, 0, n);
            if (ctrl(d, 0x81, 6, 0x2200, f->iface, n, d->cbuf, 1) < 0 || parse_rdesc(f, d->cbuf, n) < 0)
                continue;                                    /* not a pointer (media keys, gamepad...): ignored */
            ctrl(d, 0x21, 0x0A, 0, f->iface, 0, NULL, 0);    /* SET_IDLE */
            hid_arm(d, f);
            kprintf("[usb ] port %d: %s pointer\n", port + 1, f->lay.absolute ? "absolute (tablet)" : "report-protocol");
            x->live = 1;
        } else if (f->kind == FN_MSC) {
            msc_setup(d, f);
        }
    }
    return 0;
}

/* ---------------- mass storage (Bulk-Only Transport, SCSI) ---------------- */

static int bulk(struct xdev *d, int dci, u32 phys, u32 len)
{
    struct xep *e = &d->ep[dci];
    e->done = 0;
    ring_push(&e->r, phys, 0, len, (1 << 10) | (1 << 5));    /* NORMAL, IOC */
    wr(d->x->dbr, d->slot * 4, dci);
    return xfer_wait(d, dci, 3000);
}

static void ep_recover(struct xdev *d, struct fn *f, int dci)
{
    struct xhci *x = d->x;
    struct ring *r = &d->ep[dci].r;
    command(x, 0, 0, (14 << 10) | ((u32)d->slot << 24) | ((u32)dci << 16), NULL);                 /* reset endpoint */
    command(x, ((u32)(uptr)&r->t[r->enq]) | r->cycle, 0, (16 << 10) | ((u32)d->slot << 24) | ((u32)dci << 16), NULL);
    ctrl(d, 0x02, 1, 0, (dci & 1) ? (0x80 | (dci >> 1)) : (dci >> 1), 0, NULL, 0);                  /* CLEAR_FEATURE(HALT) */
    (void)f;
}

static int scsi_cmd(struct fn *f, const u8 *cdb, int cdblen, u8 *data, u32 len, int in)
{
    struct xdev *d = f->d;
    struct xhci *x = d->x;
    u8 *cbw = f->buf, *csw = f->buf + 32;
    x->busy++;
    int ret = -EIO;
    memset(cbw, 0, 31);
    *(u32 *)cbw = 0x43425355;                                /* "USBC" */
    *(u32 *)(cbw + 4) = ++f->tag;
    *(u32 *)(cbw + 8) = len;
    cbw[12] = in ? 0x80 : 0;
    cbw[14] = cdblen;
    memcpy(cbw + 15, cdb, cdblen);
    if (bulk(d, f->out_dci, (u32)(uptr)cbw, 31) != 1) {
        ep_recover(d, f, f->out_dci);
        goto out;
    }
    if (len) {
        int dci = in ? f->in_dci : f->out_dci;
        int c = bulk(d, dci, (u32)(uptr)f->dbuf, len);
        if (c == 6)
            ep_recover(d, f, dci);
        else if (c != 1 && c != 13)
            goto out;
    }
    for (int attempt = 0; attempt < 2; attempt++) {
        memset(csw, 0, 13);
        int c = bulk(d, f->in_dci, (u32)(uptr)csw, 13);
        if (c == 6) {
            ep_recover(d, f, f->in_dci);
            continue;
        }
        if ((c == 1 || c == 13) && *(u32 *)csw == 0x53425355 && *(u32 *)(csw + 4) == f->tag)
            ret = csw[12] == 0 ? 0 : -EIO;
        break;
    }
out:
    x->busy--;
    (void)data;
    return ret;
}

static u32 be32(const u8 *p) { return ((u32)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int msc_rw(void *priv, u32 lba, u32 n, void *buf, int write)
{
    struct fn *f = priv;
    while (n) {
        u32 c = n > BOT_CHUNK ? BOT_CHUNK : n;
        u8 cdb[10] = {write ? 0x2A : 0x28, 0, lba >> 24, lba >> 16, lba >> 8, lba, 0, c >> 8, c, 0};
        if (write)
            memcpy(f->dbuf, buf, c * 512);
        if (scsi_cmd(f, cdb, 10, f->dbuf, c * 512, !write) < 0)
            return -EIO;
        if (!write)
            memcpy(buf, f->dbuf, c * 512);
        lba += c;
        n -= c;
        buf = (u8 *)buf + c * 512;
    }
    return 0;
}

static void msc_setup(struct xdev *d, struct fn *f)
{
    f->dbuf = kalloc_aligned(BOT_CHUNK * 512, 65536);
    if (!f->dbuf)
        return;
    u8 tur[6] = {0}, sense[6] = {0x03, 0, 0, 0, 18, 0}, inq[6] = {0x12, 0, 0, 0, 36, 0}, cap[10] = {0x25};
    int ready = 0;
    for (int i = 0; i < 20 && !ready; i++) {
        ready = scsi_cmd(f, tur, 6, f->dbuf, 0, 0) == 0;
        if (!ready) {
            scsi_cmd(f, sense, 6, f->dbuf, 18, 1);           /* clears "unit attention" */
            udelay(100000);
        }
    }
    if (!ready) {
        kprintf("[usb ] port %d: storage device is not ready\n", d->port + 1);
        return;
    }
    char model[41] = "USB storage";
    if (scsi_cmd(f, inq, 6, f->dbuf, 36, 1) == 0) {
        u8 *p = f->dbuf;
        if ((p[0] & 0x1F) != 0) {
            kprintf("[usb ] port %d: SCSI device type %d is not a disk\n", d->port + 1, p[0] & 0x1F);
            return;
        }
        int n = 0;
        for (int i = 8; i < 32 && n < 40; i++)
            model[n++] = p[i] >= 32 && p[i] < 127 ? p[i] : ' ';
        while (n > 0 && model[n - 1] == ' ')
            n--;
        model[n] = 0;
        char tmp[41];                                        /* squeeze runs of blanks left by the padding */
        int o = 0;
        for (int i = 0; model[i]; i++)
            if (!(model[i] == ' ' && o && tmp[o - 1] == ' '))
                tmp[o++] = model[i];
        tmp[o] = 0;
        safestrcpy(model, tmp, sizeof model);
    }
    if (scsi_cmd(f, cap, 10, f->dbuf, 8, 1) != 0) {
        kprintf("[usb ] port %d: READ CAPACITY failed\n", d->port + 1);
        return;
    }
    u32 last = be32(f->dbuf), bsz = be32(f->dbuf + 4);
    if (bsz != 512) {
        kprintf("[usb ] port %d: %u-byte sectors are not supported\n", d->port + 1, bsz);
        return;
    }
    u32 sectors = last == 0xFFFFFFFFu ? last : last + 1;
    blk_register(model, sectors, BUS_USB, msc_rw, f);
    kprintf("[usb ] port %d: disk \"%s\", %u MiB\n", d->port + 1, model, sectors >> 11);
}

/* ---------------- HID report descriptors (pointers only) ---------------- */

/* Finds the buttons, X, Y and wheel of the first pointer in a report descriptor.  Returns 0 if it has X and Y. */
static int parse_rdesc(struct fn *f, const u8 *d, int n)
{
    u32 page = 0, rsize = 0, rcount = 0, id = 0, usages[16], nus = 0, umin = 0, umax = 0, bit = 0;
    i32 lmax = 0;
    int got_x = 0;
    memset(&f->lay, 0, sizeof f->lay);
    for (int i = 0; i < n;) {
        u8 b = d[i];
        if (b == 0xFE) {                                       /* long item: skip */
            if (i + 1 >= n) break;
            i += 3 + d[i + 1];
            continue;
        }
        int sz = (b & 3) == 3 ? 4 : (b & 3);
        if (i + 1 + sz > n)
            break;
        u32 v = 0;
        for (int k = 0; k < sz; k++)
            v |= (u32)d[i + 1 + k] << (8 * k);
        i32 sv = sz == 1 ? (i8)v : sz == 2 ? (i16)v : (i32)v;
        u8 tag = b & 0xFC;
        i += 1 + sz;
        switch (tag) {
        case 0x04: page = v; break;                           /* usage page */
        case 0x74: rsize = v; break;
        case 0x94: rcount = v; break;
        case 0x84:                                            /* report ID */
            if (got_x)
                goto done;                                    /* the pointer's report is complete */
            id = v;
            bit = 0;
            break;
        case 0x24: lmax = sv; break;                          /* logical maximum */
        case 0x08: if (nus < 16) usages[nus++] = sz == 4 ? v & 0xFFFF : v; break;
        case 0x18: umin = v; break;
        case 0x28: umax = v; break;
        case 0x80: {                                          /* input */
            int constant = v & 1, relative = v & 4;
            for (u32 k = 0; k < rcount && !constant; k++) {
                u32 u = k < nus ? usages[k] : (umin + k <= umax ? umin + k : 0);
                u32 off = bit + k * rsize;
                if (page == 9 && !f->lay.btn_n) {
                    f->lay.btn_off = off;
                    f->lay.btn_n = rcount > 8 ? 8 : rcount;
                    f->lay.id = id;
                } else if (page == 1 && u == 0x30) {
                    f->lay.x_off = off; f->lay.x_size = rsize; f->lay.xmax = lmax; f->lay.absolute = !relative;
                    f->lay.id = id;
                    got_x = 1;
                } else if (page == 1 && u == 0x31) {
                    f->lay.y_off = off; f->lay.y_size = rsize; f->lay.ymax = lmax;
                } else if (page == 1 && u == 0x38) {
                    f->lay.w_off = off; f->lay.w_size = rsize;
                }
            }
            bit += rsize * rcount;
            nus = umin = umax = 0;
            break;
        }
        case 0x90: case 0xB0: nus = umin = umax = 0; break;  /* output, feature */
        case 0xA0: case 0xC0: nus = 0; break;                 /* collection */
        }
    }
done:
    return got_x && f->lay.y_size && f->lay.x_size <= 16 && f->lay.y_size <= 16 ? 0 : -1;
}

static i32 get_bits(const u8 *r, int len, u32 off, u32 size, int is_signed)
{
    u32 v = 0;
    for (u32 k = 0; k < size; k++) {
        u32 b = off + k;
        if (b / 8 < (u32)len && (r[b / 8] >> (b % 8) & 1))
            v |= 1u << k;
    }
    if (is_signed && size < 32 && (v & (1u << (size - 1))))
        v |= ~0u << size;
    return (i32)v;
}

static void hid_report(struct fn *f, const u8 *r, int len)
{
    static int last_buttons;
    if (f->lay.id) {
        if (len < 1 || r[0] != f->lay.id)
            return;
        r++;
        len--;
    }
    if (!input_active())
        return;
    int b = f->lay.btn_n ? get_bits(r, len, f->lay.btn_off, f->lay.btn_n, 0) & 7 : 0;
    i32 x = get_bits(r, len, f->lay.x_off, f->lay.x_size, !f->lay.absolute);
    i32 y = get_bits(r, len, f->lay.y_off, f->lay.y_size, !f->lay.absolute);
    if (f->lay.absolute) {
        i32 xm = f->lay.xmax > 0 ? f->lay.xmax : 32767, ym = f->lay.ymax > 0 ? f->lay.ymax : 32767;
        input_push(INPUT_MOUSE, IEF_ABS, b, (i16)((long)x * 32767 / xm), (i16)((long)y * 32767 / ym));
    } else if (x || y || b != last_buttons) {
        input_push(INPUT_MOUSE, 0, b, x, y);
    }
    if (f->lay.w_size) {
        i32 w = get_bits(r, len, f->lay.w_off, f->lay.w_size, 1);
        if (w)
            input_push(INPUT_WHEEL, 0, 0, 0, w > 0 ? -1 : 1);
    }
    last_buttons = b;
}

/* ---------------- HID ---------------- */

/* USB usage ID -> PC scancode (set 1); bit 7 marks an E0-prefixed key. */
static const u8 usage_sc[0x66] = {
    [0x04] = 0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32, 0x31, 0x18, 0x19,
    0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,                       /* a-z */
    0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B,                       /* 1-9, 0 */
    0x1C, 0x01, 0x0E, 0x0F, 0x39, 0x0C, 0x0D, 0x1A, 0x1B, 0x2B, 0x2B, 0x27, 0x28, 0x29, 0x33, 0x34, 0x35, 0x3A,
    0x3B, 0x3C, 0x3D, 0x3E, 0x3F, 0x40, 0x41, 0x42, 0x43, 0x44, 0x57, 0x58,           /* F1-F12 */
    [0x46] = 0xB7, 0x46,
    [0x49] = 0xD2, 0xC7, 0xC9, 0xD3, 0xCF, 0xD1, 0xCD, 0xCB, 0xD0, 0xC8,              /* ins home pgup del end pgdn right left down up */
    [0x54] = 0xB5, 0x37, 0x4A, 0x4E, 0x9C, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49, 0x52, 0x53,
};

static int rep_sc, rep_wait, rep_ctr;

static void emit_key(int sc, int release)
{
    if (sc & 0x80)
        kbd_scancode(0xE0);
    kbd_scancode((sc & 0x7F) | (release ? 0x80 : 0));
}

static void kbd_report(struct fn *f, const u8 *r)
{
    static const u8 mod_sc[8] = {0x1D, 0x2A, 0x38, 0, 0x9D, 0x36, 0xB8, 0};   /* LCtrl LShift LAlt - RCtrl RShift RAlt - */
    u8 diff = r[0] ^ f->prev[0];
    for (int i = 0; i < 8; i++)
        if ((diff & (1 << i)) && mod_sc[i])
            emit_key(mod_sc[i], !(r[0] & (1 << i)));
    for (int i = 2; i < 8; i++) {                                              /* released keys */
        u8 k = f->prev[i];
        if (k < 4 || k >= sizeof usage_sc)
            continue;
        int still = 0;
        for (int j = 2; j < 8; j++)
            still |= r[j] == k;
        if (!still && usage_sc[k]) {
            emit_key(usage_sc[k], 1);
            if (rep_sc == usage_sc[k])
                rep_sc = 0;
        }
    }
    for (int i = 2; i < 8; i++) {                                              /* pressed keys */
        u8 k = r[i];
        if (k < 4 || k >= sizeof usage_sc)
            continue;
        int had = 0;
        for (int j = 2; j < 8; j++)
            had |= f->prev[j] == k;
        if (!had && usage_sc[k]) {
            emit_key(usage_sc[k], 0);
            rep_sc = usage_sc[k];                                              /* typematic repeat: USB keyboards don't */
            rep_wait = 40;
            rep_ctr = 0;
        }
    }
    memcpy(f->prev, r, 8);
}

static void mouse_report(const u8 *r, int len)
{
    static int last_buttons;
    if (len < 3 || !input_active())
        return;
    int dx = (i8)r[1], dy = (i8)r[2], b = r[0] & 7;
    if (dx || dy || b != last_buttons)
        input_push(INPUT_MOUSE, 0, b, dx, dy);
    if (len >= 4 && (i8)r[3])
        input_push(INPUT_WHEEL, 0, 0, 0, (i8)r[3] > 0 ? -1 : 1);
    last_buttons = b;
}

/* Called every timer tick: collects HID reports and generates key repeats. */
void usb_tick(void)
{
    for (int i = 0; i < nx; i++)
        if (xs[i].live && !xs[i].busy)
            poll_events(&xs[i]);
    if (rep_sc) {
        if (rep_wait > 0)
            rep_wait--;
        else if (--rep_ctr <= 0) {
            emit_key(rep_sc, 0);
            rep_ctr = 3;
        }
    }
}

/* ---------------- controller bring-up ---------------- */

static int port_reset(struct xhci *x, int p)
{
    volatile u32 *sc = (volatile u32 *)(x->opr + 0x400 + p * 0x10);
    const u32 keep = 0x0E00C3E0;                             /* the writable bits that must not change */
    u32 v = *sc;
    if (!(v & 1))
        return 0;
    if (!(v & (1 << 9))) {
        *sc = (v & keep) | (1 << 9);                         /* power the port */
        udelay(20000);
        v = *sc;
        if (!(v & 1))
            return 0;
    }
    *sc = (v & keep) | 0x00FE0000;                           /* acknowledge old change bits */
    *sc = (*sc & keep) | (1 << 4);                           /* reset */
    for (int i = 0; i < 500; i++) {
        udelay(1000);
        v = *sc;
        if (v & (1 << 21))
            break;
    }
    if (!(v & (1 << 21)))
        return 0;
    *sc = (v & keep) | 0x00FE0000;
    udelay(10000);
    v = *sc;
    if (!(v & 2))
        return 0;
    return (v >> 10) & 15;
}

static int handoff(struct xhci *x, u32 cap_off)
{
    u32 hcc = rd(x->cap, 0x10);
    u32 xecp = (hcc >> 16) << 2;
    for (int guard = 0; xecp && guard < 64; guard++) {
        volatile u32 *c = (volatile u32 *)(x->cap + xecp);
        if ((c[0] & 0xFF) == 1) {                            /* USB Legacy Support: the firmware may own the controller */
            c[0] |= 1u << 24;                                /* ask for it */
            for (int i = 0; i < 1000 && (c[0] & (1u << 16)); i++)
                udelay(1000);
            u32 v = c[1];
            v &= ~((1u << 0) | (1u << 4) | (1u << 13) | (1u << 14) | (1u << 15));
            c[1] = v | 0xE0000000u;                          /* no more SMIs; clear their status */
        }
        u32 next = (c[0] >> 8) & 0xFF;
        if (!next)
            break;
        xecp += next << 2;
    }
    (void)cap_off;
    return 0;
}

static int setup(struct xhci *x, struct pcidev *pd)
{
    u32 bar = pci_cfg_read(pd, 0x10);
    u64 base = bar & ~0xFu;
    if ((bar & 6) == 4)
        base |= (u64)pci_cfg_read(pd, 0x14) << 32;           /* 64-bit BAR: UEFI firmware often puts it above 4 GiB */
    pci_enable(pd);
    x->cap = mmio_map(base, 0x10000);
    if (!x->cap)
        return -1;
    u32 caplen = x->cap[0];
    x->opr = x->cap + caplen;
    x->rt = x->cap + (rd(x->cap, 0x18) & ~0x1Fu);
    x->dbr = x->cap + (rd(x->cap, 0x14) & ~3u);
    u32 hcs1 = rd(x->cap, 4), hcs2 = rd(x->cap, 8), hcc = rd(x->cap, 0x10);
    x->max_slots = hcs1 & 0xFF;
    x->max_ports = hcs1 >> 24;
    x->cs = (hcc & 4) ? 64 : 32;
    if (x->max_slots > NDEV)
        x->max_slots = NDEV;
    if (x->max_ports > 32)
        x->max_ports = 32;
    handoff(x, 0);

    wr(x->opr, 0, rd(x->opr, 0) & ~1u);                      /* stop */
    for (int i = 0; i < 200 && !(rd(x->opr, 4) & 1); i++)
        udelay(1000);
    wr(x->opr, 0, rd(x->opr, 0) | 2);                        /* reset */
    for (int i = 0; i < 500 && (rd(x->opr, 0) & 2); i++)
        udelay(1000);
    for (int i = 0; i < 500 && (rd(x->opr, 4) & (1 << 11)); i++)
        udelay(1000);
    if ((rd(x->opr, 0) & 2) || (rd(x->opr, 4) & (1 << 11)))
        return -1;

    x->dcbaa = kalloc_aligned(256 * 8, 64);
    if (!x->dcbaa || ring_init(&x->cmd) < 0 || ring_init(&x->ev) < 0)
        return -1;
    u32 nscratch = ((hcs2 >> 27) & 31) | (((hcs2 >> 21) & 31) << 5);
    if (nscratch) {
        u64 *sp = kalloc_aligned(nscratch * 8, 64);
        if (!sp)
            return -1;
        for (u32 i = 0; i < nscratch; i++) {
            u8 *pg = kalloc_aligned(4096, 4096);
            if (!pg)
                return -1;
            sp[i] = (u64)(uptr)pg;
        }
        x->dcbaa[0] = (u64)(uptr)sp;
    }
    wr(x->opr, 0x38, x->max_slots);
    wr(x->opr, 0x30, (u32)(uptr)x->dcbaa);
    wr(x->opr, 0x34, 0);
    wr(x->opr, 0x18, ((u32)(uptr)x->cmd.t) | 1);
    wr(x->opr, 0x1C, 0);

    u32 *erst = kalloc_aligned(16, 64);
    if (!erst)
        return -1;
    erst[0] = (u32)(uptr)x->ev.t;
    erst[1] = 0;
    erst[2] = NRING;
    erst[3] = 0;
    x->ev_deq = 0;
    x->ev_cycle = 1;
    wr(x->rt, 0x20 + 0x08, 1);                               /* ERSTSZ */
    wr(x->rt, 0x20 + 0x18, (u32)(uptr)x->ev.t);              /* ERDP */
    wr(x->rt, 0x20 + 0x1C, 0);
    wr(x->rt, 0x20 + 0x10, (u32)(uptr)erst);                 /* ERSTBA (last: it starts the ring) */
    wr(x->rt, 0x20 + 0x14, 0);

    wr(x->opr, 0, rd(x->opr, 0) | 1);                        /* run */
    for (int i = 0; i < 200 && (rd(x->opr, 4) & 1); i++)
        udelay(1000);
    if (rd(x->opr, 4) & 1)
        return -1;
    return 0;
}

static void probe_controller(struct pcidev *pd)
{
    if (nx == NXHCI)
        return;
    struct xhci *x = &xs[nx];
    memset(x, 0, sizeof *x);
    if (setup(x, pd) < 0) {
        kprintf("[usb ] xHCI controller at %d:%d.%d would not start\n", pd->bus, pd->slot, pd->fn);
        return;
    }
    nx++;
    kprintf("[usb ] xHCI controller %d:%d.%d, %d ports\n", pd->bus, pd->slot, pd->fn, x->max_ports);
    udelay(100000);                                          /* let devices that were just powered up settle */
    x->busy++;
    for (int p = 0; p < x->max_ports; p++) {
        int speed = port_reset(x, p);
        if (speed)
            enumerate(x, p, speed);
    }
    x->busy--;
}

int usb_init(void)
{
    struct pcidev dev;
    for (int i = 0; pci_find_class(0x0C0330, i, &dev) == 0; i++)
        probe_controller(&dev);
    return nx;
}
