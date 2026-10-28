/* PS/2 keyboard, scancode set 1, US layout. */
#include "kernel.h"

static const char normal[] =
    "\0\033" "1234567890-=\b\t" "qwertyuiop[]\n\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char shifted[] =
    "\0\033" "!@#$%^&*()_+\b\t" "QWERTYUIOP{}\n\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";

static int shift, ctrl, caps, ext;

void kbd_init(void)
{
    while (inb(0x64) & 1)          /* drain stale bytes */
        inb(0x60);
    irq_unmask(1);
}

void kbd_irq(void)
{
    u8 sc = inb(0x60);
    if (sc == 0xE0) {
        ext = 1;
        return;
    }
    int release = sc & 0x80;
    sc &= 0x7F;
    if (ext) {
        ext = 0;
        return;
    }
    switch (sc) {
    case 0x2A: case 0x36: shift = !release; return;
    case 0x1D: ctrl = !release; return;
    case 0x3A: if (!release) caps ^= 1; return;
    }
    if (release || sc >= sizeof normal - 1)
        return;

    char c = shift ? shifted[sc] : normal[sc];
    if (!c)
        return;
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    if (ctrl) {
        if (c >= 'a' && c <= 'z')
            c -= 'a' - 1;
        else if (c >= 'A' && c <= 'Z')
            c -= 'A' - 1;
        else
            return;
    }
    tty_input(c);
}
