/* PS/2 keyboard, scancode set 1, US layout. */
#include "kernel.h"

static const char normal[] =
    "\0\033" "1234567890-=\b\t" "qwertyuiop[]\n\0" "asdfghjkl;'`\0\\" "zxcvbnm,./\0*\0 ";
static const char shifted[] =
    "\0\033" "!@#$%^&*()_+\b\t" "QWERTYUIOP{}\n\0" "ASDFGHJKL:\"~\0|" "ZXCVBNM<>?\0*\0 ";

static int shift, ctrl, alt, caps, ext;

void kbd_init(void)
{
    while (inb(0x64) & 1)          /* drain stale bytes */
        inb(0x60);
    irq_unmask(1);
}

/* Translates a scancode to a key code for /dev/input; 0 if it has none. */
static u16 raw_code(u8 sc, int was_ext)
{
    if (was_ext) {
        switch (sc) {
        case 0x48: return KEY_UP;
        case 0x50: return KEY_DOWN;
        case 0x4B: return KEY_LEFT;
        case 0x4D: return KEY_RIGHT;
        case 0x47: return KEY_HOME;
        case 0x4F: return KEY_END;
        case 0x49: return KEY_PGUP;
        case 0x51: return KEY_PGDN;
        case 0x53: return KEY_DELETE;
        case 0x52: return KEY_INSERT;
        case 0x1C: return '\n';
        case 0x35: return '/';
        }
        return 0;
    }
    if (sc >= 0x3B && sc <= 0x44)
        return KEY_F1 + (sc - 0x3B);
    if (sc >= sizeof normal - 1)
        return 0;
    char c = shift ? shifted[sc] : normal[sc];
    if (caps && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')))
        c ^= 0x20;
    return (u8)c;
}

void kbd_irq(void)
{
    u8 sc = inb(0x60);
    random_event(sc);
    if (sc == 0xE0) {
        ext = 1;
        return;
    }
    int release = sc & 0x80;
    sc &= 0x7F;
    int was_ext = ext;
    ext = 0;

    switch (sc) {
    case 0x2A: case 0x36: if (!was_ext) { shift = !release; return; } break;
    case 0x1D: ctrl = !release; return;
    case 0x38: alt = !release; return;
    case 0x3A: if (!release) caps ^= 1; return;
    }

    if (input_active()) {                       /* a GUI owns the keyboard: send raw events */
        u16 code = raw_code(sc, was_ext);
        if (code)
            input_push(INPUT_KEY,
                       (release ? 0 : IEF_DOWN) | (shift ? IEF_SHIFT : 0) | (ctrl ? IEF_CTRL : 0) |
                           (alt ? IEF_ALT : 0),
                       code, 0, 0);
        return;
    }

    if (was_ext || release || sc >= sizeof normal - 1)
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
