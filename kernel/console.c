/* Text console: VGA 80x25 with a small ANSI parser, mirrored to COM1. */
#include "kernel.h"

#define COM1  0x3F8
#define VGA_W 80
#define VGA_H 25

static volatile u16 *const vga = (volatile u16 *)0xB8000;
static int cx, cy;
static int fg = 7, bg = 0, bold;
static int esc_state;              /* 0 = text, 1 = saw ESC, 2 = inside CSI */
static int params[4], np;
static int serial_ok;

static const u8 ansi2vga[8] = {0, 4, 2, 6, 1, 5, 3, 7};

/* ---- serial ---- */
static void serial_init(void)
{
    outb(COM1 + 1, 0x00);          /* no interrupts yet */
    outb(COM1 + 3, 0x80);          /* DLAB */
    outb(COM1 + 0, 0x01);          /* 115200 baud */
    outb(COM1 + 1, 0x00);
    outb(COM1 + 3, 0x03);          /* 8N1 */
    outb(COM1 + 2, 0xC7);          /* FIFO */
    outb(COM1 + 4, 0x0B);          /* DTR, RTS, OUT2 */
    serial_ok = inb(COM1 + 5) != 0xFF;
}

static void serial_putc(char c)
{
    if (!serial_ok)
        return;
    if (c == '\n')
        serial_putc('\r');
    for (int spin = 0; spin < 100000 && !(inb(COM1 + 5) & 0x20); spin++)
        ;
    outb(COM1, (u8)c);
}

void serial_irq_enable(void)
{
    if (!serial_ok)
        return;
    outb(COM1 + 1, 0x01);          /* interrupt on received data */
    irq_unmask(4);
}

void serial_irq(void)
{
    while (inb(COM1 + 5) & 1) {
        char c = (char)inb(COM1);
        if (c == '\r')
            c = '\n';
        tty_input(c);
    }
}

/* ---- VGA ---- */
static u16 cell(char c)
{
    int f = fg | (bold ? 8 : 0);
    return (u16)(((bg << 4) | f) << 8) | (u8)c;
}

static void vga_cursor(void)
{
    u16 pos = cy * VGA_W + cx;
    outb(0x3D4, 14);
    outb(0x3D5, pos >> 8);
    outb(0x3D4, 15);
    outb(0x3D5, pos & 0xFF);
}

static void vga_clear_range(int from, int to)
{
    for (int i = from; i < to; i++)
        vga[i] = cell(' ');
}

static void vga_scroll(void)
{
    for (int i = 0; i < VGA_W * (VGA_H - 1); i++)
        vga[i] = vga[i + VGA_W];
    vga_clear_range(VGA_W * (VGA_H - 1), VGA_W * VGA_H);
}

static void vga_put(char c)
{
    switch (c) {
    case '\n': cx = 0; cy++; break;
    case '\r': cx = 0; break;
    case '\b': if (cx > 0) cx--; break;
    case '\t': cx = (cx + 8) & ~7; break;
    default:
        if ((u8)c >= ' ')
            vga[cy * VGA_W + cx++] = cell(c);
        break;
    }
    if (cx >= VGA_W) {
        cx = 0;
        cy++;
    }
    while (cy >= VGA_H) {
        vga_scroll();
        cy--;
    }
}

static void sgr(int code)
{
    if (code == 0) { fg = 7; bg = 0; bold = 0; }
    else if (code == 1) bold = 1;
    else if (code == 22) bold = 0;
    else if (code >= 30 && code <= 37) fg = ansi2vga[code - 30];
    else if (code == 39) fg = 7;
    else if (code >= 40 && code <= 47) bg = ansi2vga[code - 40];
    else if (code == 49) bg = 0;
    else if (code >= 90 && code <= 97) { fg = ansi2vga[code - 90]; bold = 1; }
}

static void csi(char final)
{
    int n = np + 1;
    int p0 = params[0] ? params[0] : 1;
    switch (final) {
    case 'm':
        for (int i = 0; i < n; i++)
            sgr(params[i]);
        break;
    case 'H':
    case 'f':
        cy = (params[0] ? params[0] : 1) - 1;
        cx = (params[1] ? params[1] : 1) - 1;
        if (cy >= VGA_H) cy = VGA_H - 1;
        if (cx >= VGA_W) cx = VGA_W - 1;
        break;
    case 'J':
        if (params[0] == 2 || params[0] == 3) {
            vga_clear_range(0, VGA_W * VGA_H);
            cx = cy = 0;
        } else if (params[0] == 0) {
            vga_clear_range(cy * VGA_W + cx, VGA_W * VGA_H);
        }
        break;
    case 'K':
        if (params[0] == 2)
            vga_clear_range(cy * VGA_W, (cy + 1) * VGA_W);
        else
            vga_clear_range(cy * VGA_W + cx, (cy + 1) * VGA_W);
        break;
    case 'A': cy -= p0; if (cy < 0) cy = 0; break;
    case 'B': cy += p0; if (cy >= VGA_H) cy = VGA_H - 1; break;
    case 'C': cx += p0; if (cx >= VGA_W) cx = VGA_W - 1; break;
    case 'D': cx -= p0; if (cx < 0) cx = 0; break;
    }
}

static void put_nocursor(char c)
{
    serial_putc(c);
    if (esc_state == 0) {
        if (c == 033)
            esc_state = 1;
        else
            vga_put(c);
    } else if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            np = 0;
            memset(params, 0, sizeof params);
        } else {
            esc_state = 0;
        }
    } else {
        if (c >= '0' && c <= '9') {
            params[np] = params[np] * 10 + (c - '0');
        } else if (c == ';') {
            if (np < 3)
                np++;
        } else {
            csi(c);
            esc_state = 0;
        }
    }
}

void console_putc(char c)
{
    put_nocursor(c);
    vga_cursor();
}

void console_write(const char *s, u32 n)
{
    for (u32 i = 0; i < n; i++)
        put_nocursor(s[i]);
    vga_cursor();
}

void console_init(void)
{
    serial_init();
    vga_clear_range(0, VGA_W * VGA_H);
    cx = cy = 0;
    vga_cursor();
}

static void kputc(char c, void *ctx) { (void)ctx; put_nocursor(c); }

int kprintf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vformat(kputc, NULL, fmt, ap);
    va_end(ap);
    vga_cursor();
    return n;
}

void panic(const char *fmt, ...)
{
    cli();
    fg = 7; bg = 4; bold = 1;
    esc_state = 0;
    kprintf("\n*** KERNEL PANIC: ");
    va_list ap;
    va_start(ap, fmt);
    vformat(kputc, NULL, fmt, ap);
    va_end(ap);
    kprintf(" ***\n");
    for (;;)
        __asm__ volatile("hlt");
}
