/* Input events: raw keyboard and PS/2 mouse, delivered through /dev/input.
 * While nobody has /dev/input open the keyboard feeds the tty instead. */
#include "kernel.h"

#define QN 256

static struct input_event q[QN];
static u32 qhead, qtail;
static int refs;

int input_active(void) { return refs > 0; }
int input_ready(void) { return qhead != qtail; }

void input_open(void)
{
    refs++;
}

void input_close(void)
{
    if (--refs <= 0) {
        refs = 0;
        qhead = qtail = 0;
    }
}

void input_push(u8 type, u8 flags, u16 code, i16 dx, i16 dy)
{
    /* Merge into the newest unread motion event so fast mice can't flood the queue. */
    if (type == INPUT_MOUSE && qhead != qtail) {
        struct input_event *last = &q[(qtail - 1) % QN];
        if (last->type == INPUT_MOUSE && last->code == code) {
            last->dx += dx;
            last->dy += dy;
            wakeup(q);
            return;
        }
    }
    if (qtail - qhead == QN)
        qhead++;                                /* overflow: drop the oldest */
    struct input_event *e = &q[qtail++ % QN];
    e->type = type;
    e->flags = flags;
    e->code = code;
    e->dx = dx;
    e->dy = dy;
    wakeup(q);
}

/* Blocks until at least one event is available; returns whole events only. */
int input_read(void *dst, u32 n)
{
    u32 max = n / sizeof(struct input_event);
    if (!max)
        return -EINVAL;
    while (qhead == qtail) {
        if (cur->killed)
            return -EINTR;
        sleep_on(q);
    }
    struct input_event *out = dst;
    u32 got = 0;
    while (got < max && qhead != qtail)
        out[got++] = q[qhead++ % QN];
    return got * sizeof(struct input_event);
}

/* ---------------- PS/2 mouse (second port of the 8042) ---------------- */

static void kbc_wait_write(void)
{
    for (int i = 0; i < 100000 && (inb(0x64) & 2); i++)
        ;
}

static int kbc_wait_read(void)
{
    for (int i = 0; i < 100000; i++)
        if (inb(0x64) & 1)
            return 1;
    return 0;
}

static void mouse_cmd(u8 v)
{
    kbc_wait_write();
    outb(0x64, 0xD4);                           /* next byte goes to the mouse */
    kbc_wait_write();
    outb(0x60, v);
    if (kbc_wait_read())
        inb(0x60);                              /* ACK (0xFA) */
}

void mouse_init(void)
{
    kbc_wait_write();
    outb(0x64, 0xA8);                           /* enable the auxiliary port */
    kbc_wait_write();
    outb(0x64, 0x20);                           /* read the controller config byte */
    kbc_wait_read();
    u8 cfg = inb(0x60);
    cfg |= 0x02;                                /* IRQ12 on */
    cfg &= ~0x20;                               /* mouse clock on */
    kbc_wait_write();
    outb(0x64, 0x60);
    kbc_wait_write();
    outb(0x60, cfg);
    mouse_cmd(0xF6);                            /* defaults */
    mouse_cmd(0xF4);                            /* start streaming */
    irq_unmask(12);
}

void mouse_irq(void)
{
    static u8 pkt[3];
    static int n;
    u8 b = inb(0x60);
    if (n == 0 && !(b & 0x08))
        return;                                 /* out of sync: wait for a header byte */
    pkt[n++] = b;
    if (n < 3)
        return;
    n = 0;
    if (pkt[0] & 0xC0)
        return;                                 /* overflow */
    int dx = pkt[1] - ((pkt[0] << 4) & 0x100);
    int dy = pkt[2] - ((pkt[0] << 3) & 0x100);
    if (input_active())
        input_push(INPUT_MOUSE, 0, pkt[0] & 7, dx, -dy);   /* PS/2 y grows upward */
}
