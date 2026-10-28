/* Procedural icons: drawn on a 32x32 grid and scaled to any size. */
#include "desk.h"

static int S;
static int OX, OY;

static void box(int gx, int gy, int gw, int gh, u32 c)
{
    int x0 = OX + gx * S / 32, y0 = OY + gy * S / 32;
    int x1 = OX + (gx + gw) * S / 32, y1 = OY + (gy + gh) * S / 32;
    gfx_fill(x0, y0, x1 - x0 > 0 ? x1 - x0 : 1, y1 - y0 > 0 ? y1 - y0 : 1, c);
}

static void outline(int gx, int gy, int gw, int gh, u32 edge, u32 fill)
{
    box(gx, gy, gw, gh, edge);
    box(gx + 1, gy + 1, gw - 2, gh - 2, fill);
}

static void folder(void)
{
    box(2, 7, 12, 5, RGB(0xc9, 0x9a, 0x2e));
    outline(2, 10, 28, 19, RGB(0xa6, 0x7c, 0x1b), RGB(0xf0, 0xc2, 0x4d));
    box(3, 11, 26, 3, RGB(0xf8, 0xdc, 0x8a));
}

static void page(u32 accent)
{
    outline(7, 2, 19, 27, RGB(0x70, 0x78, 0x80), RGB(0xfa, 0xfa, 0xfa));
    box(20, 2, 6, 6, RGB(0xc8, 0xcc, 0xd0));
    for (int i = 0; i < 5; i++)
        box(10, 11 + i * 3, i == 4 ? 7 : 13, 1, accent);
}

void icon_draw(int kind, int x, int y, int size)
{
    S = size;
    OX = x;
    OY = y;
    switch (kind) {
    case ICON_FOLDER:
    case ICON_FILES:
    case ICON_HOME:
        folder();
        if (kind == ICON_HOME) {
            box(12, 15, 8, 8, RGB(0xfb, 0xfb, 0xfb));
            box(10, 14, 12, 2, RGB(0xc8, 0x40, 0x30));
        }
        break;
    case ICON_FILE:
        page(RGB(0xa0, 0xa8, 0xb0));
        break;
    case ICON_EDITOR:
        page(RGB(0x4a, 0x90, 0xd9));
        box(20, 18, 3, 11, RGB(0xe0, 0x9a, 0x2a));          /* pencil */
        box(20, 28, 3, 2, RGB(0x30, 0x30, 0x30));
        break;
    case ICON_TERM:
        outline(2, 5, 28, 22, RGB(0x10, 0x12, 0x14), RGB(0x24, 0x29, 0x30));
        box(2, 5, 28, 4, RGB(0x50, 0x58, 0x60));
        box(6, 13, 3, 2, RGB(0x6c, 0xe0, 0x6c));            /* ">" */
        box(8, 15, 3, 2, RGB(0x6c, 0xe0, 0x6c));
        box(6, 17, 3, 2, RGB(0x6c, 0xe0, 0x6c));
        box(13, 19, 8, 2, RGB(0xd3, 0xd7, 0xcf));           /* "_" */
        break;
    case ICON_MONITOR:
        outline(2, 4, 28, 20, RGB(0x30, 0x38, 0x40), RGB(0x1b, 0x1f, 0x24));
        box(5, 19, 4, 3, RGB(0x6c, 0xe0, 0x6c));            /* bars */
        box(11, 13, 4, 9, RGB(0x6c, 0xe0, 0x6c));
        box(17, 16, 4, 6, RGB(0xe0, 0xc0, 0x40));
        box(23, 9, 4, 13, RGB(0xe0, 0x60, 0x50));
        box(12, 25, 8, 3, RGB(0x70, 0x78, 0x80));
        box(8, 28, 16, 2, RGB(0x70, 0x78, 0x80));
        break;
    case ICON_ABOUT:
        outline(3, 3, 26, 26, RGB(0x2a, 0x6a, 0xb0), RGB(0x4a, 0x90, 0xd9));
        box(14, 8, 4, 4, C_WHITE);
        box(14, 14, 4, 11, C_WHITE);
        break;
    case ICON_DISK:
        outline(3, 6, 26, 20, RGB(0x60, 0x68, 0x70), RGB(0xc8, 0xcc, 0xd0));
        box(6, 20, 14, 2, RGB(0x50, 0x58, 0x60));
        box(24, 20, 3, 2, RGB(0x6c, 0xe0, 0x6c));
        break;
    case ICON_EXEC:
        page(RGB(0x40, 0xb0, 0x60));
        box(12, 20, 8, 6, RGB(0x40, 0xb0, 0x60));
        break;
    case ICON_APPS:
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                box(4 + c * 9, 4 + r * 9, 7, 7, C_WHITE);
        break;
    case ICON_SOFTWARE:
        outline(4, 10, 24, 18, RGB(0x9a, 0x6a, 0x30), RGB(0xd8, 0xa6, 0x5c));
        box(4, 10, 24, 5, RGB(0xe8, 0xc0, 0x80));
        box(14, 10, 4, 18, RGB(0xb0, 0x80, 0x40));
        break;
    case ICON_POWER:
        outline(5, 5, 22, 22, RGB(0xa0, 0x30, 0x30), RGB(0xe0, 0x50, 0x48));
        box(14, 8, 4, 10, C_WHITE);
        break;
    case ICON_SETTINGS: {                                   /* a gear */
        u32 c = RGB(0xb4, 0xbc, 0xc6), d = RGB(0x60, 0x6a, 0x74);
        box(11, 3, 10, 26, d);  box(3, 11, 26, 10, d);
        box(7, 7, 18, 18, d);
        box(12, 4, 8, 24, c);   box(4, 12, 24, 8, c);
        box(8, 8, 16, 16, c);
        box(12, 12, 8, 8, RGB(0x34, 0x3c, 0x44));          /* hub */
        break;
    }
    case ICON_INSTALL:
        outline(3, 14, 26, 14, RGB(0x60, 0x68, 0x70), RGB(0xc8, 0xcc, 0xd0));
        box(23, 22, 3, 2, RGB(0x6c, 0xe0, 0x6c));
        box(13, 2, 6, 9, RGB(0x4a, 0x90, 0xd9));           /* arrow pointing down into the disk */
        box(9, 9, 14, 2, RGB(0x4a, 0x90, 0xd9));
        box(11, 11, 10, 2, RGB(0x4a, 0x90, 0xd9));
        box(13, 13, 6, 1, RGB(0x4a, 0x90, 0xd9));
        break;
    case ICON_RESTART:
        outline(5, 5, 22, 22, RGB(0x30, 0x70, 0xb0), RGB(0x4a, 0x90, 0xd9));
        box(10, 10, 12, 2, C_WHITE); box(10, 10, 2, 12, C_WHITE);
        box(10, 20, 12, 2, C_WHITE); box(20, 14, 2, 8, C_WHITE);
        break;
    case ICON_PALETTE:
        outline(2, 4, 28, 24, RGB(0x70, 0x78, 0x80), RGB(0xf4, 0xf5, 0xf6));
        box(6, 8, 9, 7, RGB(0x4a, 0x90, 0xd9)); box(17, 8, 9, 7, RGB(0x3f, 0xae, 0x72));
        box(6, 17, 9, 7, RGB(0xe0, 0x8a, 0x3c)); box(17, 17, 9, 7, RGB(0x9a, 0x6a, 0xd8));
        break;
    case ICON_RUN:
        outline(2, 6, 28, 20, RGB(0x50, 0x58, 0x60), RGB(0xf0, 0xf2, 0xf4));
        box(5, 12, 4, 8, RGB(0x4a, 0x90, 0xd9));
        box(11, 15, 14, 2, RGB(0x90, 0x98, 0xa0));
        break;
    case ICON_LOGOUT:
        outline(4, 6, 16, 20, RGB(0x60, 0x68, 0x70), RGB(0xd0, 0xd4, 0xd8));
        box(16, 14, 12, 4, RGB(0x40, 0x80, 0xc0));
        box(24, 11, 4, 10, RGB(0x40, 0x80, 0xc0));
        break;
    }
}

int ui_button(int x, int y, int w, int h, const char *label, int pressed)
{
    gfx_vgrad(x, y, w, h, pressed ? RGB(0xc4, 0xc8, 0xcc) : RGB(0xfa, 0xfa, 0xfa),
              pressed ? RGB(0xe0, 0xe2, 0xe4) : RGB(0xd8, 0xdb, 0xde));
    gfx_frame(x, y, w, h, RGB(0x8a, 0x92, 0x9a));
    int tw = gfx_text_width(label);
    return gfx_text(x + (w - tw) / 2, y + (h - 16) / 2, label, C_TEXT);
}

void ui_panel_inset(int x, int y, int w, int h)
{
    gfx_fill(x, y, w, h, C_WHITE);
    gfx_frame(x, y, w, h, RGB(0x9a, 0xa2, 0xaa));
}
