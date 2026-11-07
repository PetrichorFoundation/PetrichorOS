/* GrapeJuice: GDI.  Device contexts draw into plain 0x00RRGGBB pixel buffers (a window's, or a bitmap's). */
#include "font8x16.h"

typedef struct { i32 left, top, right, bottom; } RECT;
typedef struct { i32 x, y; } POINT;
typedef struct { i32 cx, cy; } SIZE;
typedef u32 COLORREF;
typedef u32 UINT;
typedef i64 LRESULT;
typedef u64 WPARAM;
typedef i64 LPARAM;

/* COLORREF is 0x00BBGGRR; our pixels are 0x00RRGGBB */
static u32 cref(COLORREF c) { return (c & 0xFF) << 16 | (c & 0xFF00) | (c >> 16 & 0xFF); }

enum { GO_PEN = 1, GO_BRUSH, GO_FONT, GO_BITMAP };

struct gdiobj {
    int type;
    u32 color;                       /* pens, brushes: 0x00RRGGBB */
    int width, style;                /* pens; style 5 (PS_NULL) draws nothing; brushes: 1 = hollow */
    int w, h, flip;                  /* bitmaps: flip = rows stored bottom-up, as a DIB section's are */
    u32 *bits;
    int scale;                       /* fonts: integer scale of the 8x16 face */
};

struct window;

struct dc {
    u32 *px;
    int w, h, flip;
    int ox, oy;                      /* origin of this DC inside the pixel buffer */
    RECT clip;                       /* in buffer coordinates */
    struct gdiobj *pen, *brush, *font, *bitmap;
    u32 textcolor, bkcolor;
    int bkmode, curx, cury;
    struct window *win;              /* a window's DC: drawing marks the window dirty */
};

static struct gdiobj stock_objs[20];
static int stock_ready;

static struct gdiobj *new_obj(int type)
{
    struct gdiobj *o = calloc(1, sizeof *o);
    if (o)
        o->type = type;
    return o;
}

static void init_stock(void)
{
    if (stock_ready)
        return;
    stock_ready = 1;
    static const u32 shades[5] = {0xFFFFFF, 0xC0C0C0, 0x808080, 0x404040, 0x000000};
    for (int i = 0; i < 5; i++) { stock_objs[i].type = GO_BRUSH; stock_objs[i].color = shades[i]; }
    stock_objs[5].type = GO_BRUSH; stock_objs[5].style = 1;                       /* NULL_BRUSH */
    stock_objs[6].type = GO_PEN; stock_objs[6].color = 0xFFFFFF; stock_objs[6].width = 1;
    stock_objs[7].type = GO_PEN; stock_objs[7].color = 0x000000; stock_objs[7].width = 1;
    stock_objs[8].type = GO_PEN; stock_objs[8].style = 5;                         /* NULL_PEN */
    for (int i = 9; i < 20; i++) { stock_objs[i].type = GO_FONT; stock_objs[i].scale = 1; }
}

static void WINAPI g_dummy(void) {}

/* ---- pixel access ---- */

static u32 *dc_row(struct dc *d, int y) { return d->px + (d->flip ? d->h - 1 - y : y) * d->w; }

static int inside(const struct dc *d, int x, int y) { return x >= d->clip.left && x < d->clip.right && y >= d->clip.top && y < d->clip.bottom; }

static void mark_dirty(struct dc *d, int l, int t, int r, int b);

static void dc_pset(struct dc *d, int x, int y, u32 rgb)
{
    x += d->ox; y += d->oy;
    if (inside(d, x, y))
        dc_row(d, y)[x] = rgb;
}

static void dc_fill(struct dc *d, int l, int t, int r, int b, u32 rgb)
{
    l += d->ox; r += d->ox; t += d->oy; b += d->oy;
    if (l < d->clip.left) l = d->clip.left;
    if (t < d->clip.top) t = d->clip.top;
    if (r > d->clip.right) r = d->clip.right;
    if (b > d->clip.bottom) b = d->clip.bottom;
    for (int y = t; y < b; y++) {
        u32 *row = dc_row(d, y);
        for (int x = l; x < r; x++)
            row[x] = rgb;
    }
}

static int abs_i32(int v) { return v < 0 ? -v : v; }

static void dc_line(struct dc *d, int x0, int y0, int x1, int y1, u32 rgb, int width)
{
    int dx = abs_i32(x1 - x0), dy = -abs_i32(y1 - y0), sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
    int hw = width > 1 ? width / 2 : 0;
    for (;;) {
        if (hw) dc_fill(d, x0 - hw, y0 - hw, x0 - hw + width, y0 - hw + width, rgb);
        else dc_pset(d, x0, y0, rgb);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static int isqrt_i(int v)
{
    int r = 0, b = 1 << 28;
    while (b > v) b >>= 2;
    while (b) { if (v >= r + b) { v -= r + b; r = (r >> 1) + b; } else r >>= 1; b >>= 2; }
    return r;
}

static void dc_ellipse(struct dc *d, int l, int t, int r, int b, int fill, u32 fillc, int outline, u32 linec)
{
    int W = r - l, H = b - t;
    if (W <= 0 || H <= 0) return;
    int cx2 = l + r;                                          /* twice the centre x */
    int prev_l = 0, prev_r = 0, have = 0;
    for (int y = t; y < b; y++) {
        int dy = 2 * (y - t) + 1 - H;
        int num = H * H - dy * dy;
        if (num < 0) continue;
        int hw2 = W * isqrt_i(num) / H;                       /* the full chord width */
        int xl = (cx2 - hw2 + 1) / 2, xr = (cx2 + hw2 + 1) / 2;
        if (fill) dc_fill(d, xl, y, xr, y + 1, fillc);
        if (outline) {
            if (y == t || y == b - 1 || !have) dc_fill(d, xl, y, xr, y + 1, linec);
            else {
                dc_fill(d, xl, y, prev_l > xl ? prev_l : xl + 1, y + 1, linec);
                dc_fill(d, prev_r < xr ? prev_r : xr - 1, y, xr, y + 1, linec);
            }
        }
        prev_l = xl; prev_r = xr; have = 1;
    }
}

/* ---- text ---- */

static int text_scale(struct dc *d) { return d->font && d->font->scale > 1 ? d->font->scale : 1; }

static void dc_glyph(struct dc *d, int x, int y, int ch, u32 rgb, int scale)
{
    if (ch < 32 || ch > 126) ch = '?';
    const unsigned char *g = font8x16[ch - 32];
    for (int row = 0; row < 16; row++)
        for (int col = 0; col < 8; col++)
            if (g[row] & (0x80 >> col)) {
                if (scale == 1) dc_pset(d, x + col, y + row, rgb);
                else dc_fill(d, x + col * scale, y + row * scale, x + col * scale + scale, y + row * scale + scale, rgb);
            }
}

static int dc_text(struct dc *d, int x, int y, const char *s, int n)
{
    int sc = text_scale(d);
    if (d->bkmode == 2)
        dc_fill(d, x, y, x + n * 8 * sc, y + 16 * sc, cref(d->bkcolor));
    for (int i = 0; i < n; i++)
        dc_glyph(d, x + i * 8 * sc, y, (u8)s[i], cref(d->textcolor), sc);
    mark_dirty(d, x, y, x + n * 8 * sc, y + 16 * sc);
    return n * 8 * sc;
}

/* ---- creating DCs ---- */

static struct dc *new_dc(void)
{
    init_stock();
    struct dc *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->pen = &stock_objs[7];
    d->brush = &stock_objs[0];
    d->font = &stock_objs[13];
    d->textcolor = 0x000000;
    d->bkcolor = 0xFFFFFF;
    d->bkmode = 2;
    return d;
}

static void dc_set_target(struct dc *d, u32 *px, int w, int h, int flip)
{
    d->px = px; d->w = w; d->h = h; d->flip = flip;
    d->clip = (RECT){0, 0, w, h};
    d->ox = d->oy = 0;
}

/* ---- the GDI entry points ---- */

static struct dc *D(HANDLE h) { return (struct dc *)h; }

static HANDLE WINAPI gdi_GetStockObject(int i) { init_stock(); return i >= 0 && i < 20 ? &stock_objs[i] : &stock_objs[0]; }
static HANDLE WINAPI gdi_CreateSolidBrush(COLORREF c) { struct gdiobj *o = new_obj(GO_BRUSH); if (o) o->color = cref(c); return o; }
static HANDLE WINAPI gdi_CreatePen(int style, int width, COLORREF c) { struct gdiobj *o = new_obj(GO_PEN); if (o) { o->color = cref(c); o->width = width < 1 ? 1 : width; o->style = style; } return o; }
static HANDLE WINAPI gdi_CreateFontA(int h, int w, int esc, int ori, int weight, DWORD it, DWORD ul, DWORD so, DWORD cs, DWORD op, DWORD cp, DWORD q, DWORD pf, const char *face)
{
    struct gdiobj *o = new_obj(GO_FONT);
    if (o) o->scale = abs_i32(h) >= 32 ? 2 : 1;
    return o;
}
static HANDLE WINAPI gdi_CreateFontIndirectA(const u8 *lf) { return gdi_CreateFontA((i32)rd32(lf), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, NULL); }

static HANDLE WINAPI gdi_SelectObject(HANDLE hdc, HANDLE obj)
{
    struct dc *d = D(hdc);
    struct gdiobj *o = obj;
    if (!d || !o) return NULL;
    struct gdiobj *old = NULL;
    switch (o->type) {
    case GO_PEN: old = d->pen; d->pen = o; break;
    case GO_BRUSH: old = d->brush; d->brush = o; break;
    case GO_FONT: old = d->font; d->font = o; break;
    case GO_BITMAP:
        old = d->bitmap;
        d->bitmap = o;
        dc_set_target(d, o->bits, o->w, o->h, o->flip);
        break;
    }
    return old;
}

static BOOL WINAPI gdi_DeleteObject(HANDLE obj)
{
    struct gdiobj *o = obj;
    if (!o || (o >= stock_objs && o < stock_objs + 20)) return 1;
    if (o->type == GO_BITMAP) free(o->bits);
    free(o);
    return 1;
}

static HANDLE WINAPI gdi_CreateCompatibleDC(HANDLE hdc)
{
    struct dc *d = new_dc();
    if (!d) return NULL;
    struct gdiobj *bm = new_obj(GO_BITMAP);
    bm->w = bm->h = 1;
    bm->bits = calloc(1, 4);
    d->bitmap = bm;
    dc_set_target(d, bm->bits, 1, 1, 0);
    return d;
}

static BOOL WINAPI gdi_DeleteDC(HANDLE hdc) { struct dc *d = D(hdc); if (d && d->win == NULL) free(d); return 1; }

static HANDLE WINAPI gdi_CreateCompatibleBitmap(HANDLE hdc, int w, int h)
{
    if (w <= 0 || h <= 0 || (long)w * h > 16 * 1024 * 1024) return NULL;
    struct gdiobj *o = new_obj(GO_BITMAP);
    if (!o) return NULL;
    o->w = w; o->h = h;
    o->bits = calloc((size_t)w * h, 4);
    return o;
}

static HANDLE WINAPI gdi_CreateDIBSection(HANDLE hdc, const u8 *bmi, DWORD usage, void **bits, HANDLE section, DWORD off)
{
    i32 w = (i32)rd32(bmi + 4), h = (i32)rd32(bmi + 8);
    int flip = h > 0;
    if (h < 0) h = -h;
    if (w <= 0 || h <= 0 || (long)w * h > 16 * 1024 * 1024) return NULL;
    struct gdiobj *o = new_obj(GO_BITMAP);
    if (!o) return NULL;
    o->w = w; o->h = h; o->flip = flip;
    o->bits = calloc((size_t)w * h, 4);
    if (bits) *bits = o->bits;
    return o;
}

static COLORREF WINAPI gdi_SetTextColor(HANDLE hdc, COLORREF c) { struct dc *d = D(hdc); COLORREF old = (cref(d->textcolor) ); d->textcolor = cref(c); return old; }
static COLORREF WINAPI gdi_SetBkColor(HANDLE hdc, COLORREF c) { struct dc *d = D(hdc); COLORREF old = cref(d->bkcolor); d->bkcolor = cref(c); return old; }
static int WINAPI gdi_SetBkMode(HANDLE hdc, int m) { struct dc *d = D(hdc); int old = d->bkmode; d->bkmode = m; return old; }
static int WINAPI gdi_GetDeviceCaps(HANDLE hdc, int idx)
{
    switch (idx) { case 8: return 1024; case 10: return 768; case 12: return 32; case 88: case 90: return 96; case 24: return -1; default: return 0; }
}

static COLORREF WINAPI gdi_SetPixel(HANDLE hdc, int x, int y, COLORREF c) { struct dc *d = D(hdc); dc_pset(d, x, y, cref(c)); mark_dirty(d, x, y, x + 1, y + 1); return c; }
static COLORREF WINAPI gdi_GetPixel(HANDLE hdc, int x, int y) { struct dc *d = D(hdc); x += d->ox; y += d->oy; return inside(d, x, y) ? cref(dc_row(d, y)[x]) : (COLORREF)-1; }

static BOOL WINAPI gdi_MoveToEx(HANDLE hdc, int x, int y, POINT *old) { struct dc *d = D(hdc); if (old) { old->x = d->curx; old->y = d->cury; } d->curx = x; d->cury = y; return 1; }
static BOOL WINAPI gdi_LineTo(HANDLE hdc, int x, int y)
{
    struct dc *d = D(hdc);
    if (d->pen->style != 5) dc_line(d, d->curx, d->cury, x, y, d->pen->color, d->pen->width);
    int l = d->curx < x ? d->curx : x, r = d->curx > x ? d->curx : x, t = d->cury < y ? d->cury : y, b = d->cury > y ? d->cury : y;
    mark_dirty(d, l - d->pen->width, t - d->pen->width, r + d->pen->width + 1, b + d->pen->width + 1);
    d->curx = x; d->cury = y;
    return 1;
}

static void dc_frame_rect(struct dc *d, int l, int t, int r, int b, u32 rgb, int w)
{
    dc_fill(d, l, t, r, t + w, rgb); dc_fill(d, l, b - w, r, b, rgb);
    dc_fill(d, l, t, l + w, b, rgb); dc_fill(d, r - w, t, r, b, rgb);
}

static BOOL WINAPI gdi_Rectangle(HANDLE hdc, int l, int t, int r, int b)
{
    struct dc *d = D(hdc);
    if (d->brush->style != 1) dc_fill(d, l, t, r, b, d->brush->color);
    if (d->pen->style != 5) dc_frame_rect(d, l, t, r, b, d->pen->color, d->pen->width);
    mark_dirty(d, l, t, r, b);
    return 1;
}

static BOOL WINAPI gdi_RoundRect(HANDLE hdc, int l, int t, int r, int b, int w, int h) { return gdi_Rectangle(hdc, l, t, r, b); }

static BOOL WINAPI gdi_Ellipse(HANDLE hdc, int l, int t, int r, int b)
{
    struct dc *d = D(hdc);
    dc_ellipse(d, l, t, r, b, d->brush->style != 1, d->brush->color, d->pen->style != 5, d->pen->color);
    mark_dirty(d, l, t, r, b);
    return 1;
}

static int WINAPI gdi_FillRect(HANDLE hdc, const RECT *rc, HANDLE brush)
{
    struct dc *d = D(hdc);
    struct gdiobj *b = brush;
    u32 c = b && (u64)b < 64 ? cref(0xC0C0C0) : b ? b->color : 0xFFFFFF;       /* small numbers are COLOR_xxx + 1 */
    if (b && (u64)b < 64) c = 0xD4D0C8;
    dc_fill(d, rc->left, rc->top, rc->right, rc->bottom, c);
    mark_dirty(d, rc->left, rc->top, rc->right, rc->bottom);
    return 1;
}

static int WINAPI gdi_FrameRect(HANDLE hdc, const RECT *rc, HANDLE brush)
{
    struct dc *d = D(hdc);
    struct gdiobj *b = brush;
    dc_frame_rect(d, rc->left, rc->top, rc->right, rc->bottom, b ? b->color : 0, 1);
    mark_dirty(d, rc->left, rc->top, rc->right, rc->bottom);
    return 1;
}

static BOOL WINAPI gdi_Polygon(HANDLE hdc, const POINT *pts, int n)
{
    struct dc *d = D(hdc);
    if (n < 2) return 0;
    int miny = pts[0].y, maxy = pts[0].y;
    for (int i = 1; i < n; i++) { if (pts[i].y < miny) miny = pts[i].y; if (pts[i].y > maxy) maxy = pts[i].y; }
    if (d->brush->style != 1)
        for (int y = miny; y < maxy; y++) {                  /* even-odd scanline fill */
            int xs[64], k = 0;
            for (int i = 0; i < n && k < 62; i++) {
                POINT a = pts[i], b = pts[(i + 1) % n];
                if ((a.y <= y && b.y > y) || (b.y <= y && a.y > y))
                    xs[k++] = a.x + (y - a.y) * (b.x - a.x) / (b.y - a.y);
            }
            for (int i = 1; i < k; i++) { int v = xs[i], j = i - 1; while (j >= 0 && xs[j] > v) { xs[j + 1] = xs[j]; j--; } xs[j + 1] = v; }
            for (int i = 0; i + 1 < k; i += 2)
                dc_fill(d, xs[i], y, xs[i + 1], y + 1, d->brush->color);
        }
    if (d->pen->style != 5)
        for (int i = 0; i < n; i++)
            dc_line(d, pts[i].x, pts[i].y, pts[(i + 1) % n].x, pts[(i + 1) % n].y, d->pen->color, d->pen->width);
    mark_dirty(d, 0, miny, d->w, maxy + 1);
    return 1;
}

static int wlen(const u16 *s, int n) { if (n >= 0) return n; int k = 0; while (s[k]) k++; return k; }

static BOOL WINAPI gdi_TextOutA(HANDLE hdc, int x, int y, const char *s, int n) { dc_text(D(hdc), x, y, s, n < 0 ? (int)strlen(s) : n); return 1; }
static BOOL WINAPI gdi_TextOutW(HANDLE hdc, int x, int y, const u16 *s, int n)
{
    n = wlen(s, n);
    char tmp[256];
    if (n > 255) n = 255;
    for (int i = 0; i < n; i++) tmp[i] = s[i] < 128 ? (char)s[i] : '?';
    dc_text(D(hdc), x, y, tmp, n);
    return 1;
}
static BOOL WINAPI gdi_ExtTextOutA(HANDLE hdc, int x, int y, UINT opts, const RECT *rc, const char *s, UINT n, const int *dx) { return gdi_TextOutA(hdc, x, y, s, n); }
static BOOL WINAPI gdi_GetTextExtentPoint32A(HANDLE hdc, const char *s, int n, SIZE *sz) { int sc = text_scale(D(hdc)); sz->cx = n * 8 * sc; sz->cy = 16 * sc; return 1; }
static BOOL WINAPI gdi_GetTextExtentPoint32W(HANDLE hdc, const u16 *s, int n, SIZE *sz) { int sc = text_scale(D(hdc)); sz->cx = wlen(s, n) * 8 * sc; sz->cy = 16 * sc; return 1; }
static BOOL WINAPI gdi_GetTextMetricsA(HANDLE hdc, u8 *tm)
{
    int sc = text_scale(D(hdc));
    memset(tm, 0, 56);
    *(i32 *)(tm + 0) = 16 * sc; *(i32 *)(tm + 4) = 13 * sc; *(i32 *)(tm + 8) = 3 * sc; *(i32 *)(tm + 20) = 8 * sc; *(i32 *)(tm + 24) = 8 * sc;
    return 1;
}

/* DrawText: left/centre/right, single line or word-wrapped. */
static int WINAPI gdi_DrawTextA(HANDLE hdc, const char *s, int n, RECT *rc, UINT fmt)
{
    struct dc *d = D(hdc);
    if (n < 0) n = strlen(s);
    int sc = text_scale(d), cw = 8 * sc, lh = 16 * sc, width = rc->right - rc->left;
    int lines = 0, maxw = 0;
    const char *ls[64]; int ln[64];
    int i = 0;
    while (i <= n && lines < 64) {
        int j = i;
        while (j < n && s[j] != '\n') j++;
        int start = i;
        while (1) {
            int len = j - start;
            if ((fmt & 0x10) && len * cw > width && width > 0) {              /* DT_WORDBREAK */
                int fit = width / cw, k = start + fit;
                while (k > start && s[k] != ' ') k--;
                if (k == start) k = start + (fit ? fit : 1);
                ls[lines] = s + start; ln[lines++] = k - start;
                start = k;
                while (start < j && s[start] == ' ') start++;
                if (lines >= 63) break;
                continue;
            }
            ls[lines] = s + start; ln[lines++] = len;
            break;
        }
        i = j + 1;
        if (fmt & 0x20) break;                                                /* DT_SINGLELINE */
    }
    for (int k = 0; k < lines; k++) if (ln[k] * cw > maxw) maxw = ln[k] * cw;
    int height = lines * lh;
    if (fmt & 0x400) { rc->right = rc->left + maxw; rc->bottom = rc->top + height; return height; }     /* DT_CALCRECT */
    int y = rc->top;
    if (fmt & 4) y = rc->top + (rc->bottom - rc->top - height) / 2;                                       /* DT_VCENTER */
    if (fmt & 8) y = rc->bottom - height;                                                                  /* DT_BOTTOM */
    int oldmode = d->bkmode;
    for (int k = 0; k < lines; k++) {
        int x = rc->left, w = ln[k] * cw;
        if ((fmt & 3) == 1) x = rc->left + (width - w) / 2;
        else if ((fmt & 3) == 2) x = rc->right - w;
        dc_text(d, x, y + k * lh, ls[k], ln[k]);
    }
    d->bkmode = oldmode;
    return height;
}

static int WINAPI gdi_DrawTextW(HANDLE hdc, const u16 *s, int n, RECT *rc, UINT fmt)
{
    n = wlen(s, n);
    char tmp[512];
    if (n > 511) n = 511;
    for (int i = 0; i < n; i++) tmp[i] = s[i] < 128 ? (char)s[i] : '?';
    return gdi_DrawTextA(hdc, tmp, n, rc, fmt);
}

/* Copies w*h pixels from src to dst (clipped on the destination). */
static void blit(struct dc *dst, int dx, int dy, int w, int h, struct dc *src, int sx, int sy)
{
    for (int y = 0; y < h; y++) {
        int syy = sy + y + src->oy, dyy = dy + y + dst->oy;
        if (syy < 0 || syy >= src->h || dyy < dst->clip.top || dyy >= dst->clip.bottom) continue;
        u32 *srow = dc_row(src, syy), *drow = dc_row(dst, dyy);
        for (int x = 0; x < w; x++) {
            int sxx = sx + x + src->ox, dxx = dx + x + dst->ox;
            if (sxx < 0 || sxx >= src->w || dxx < dst->clip.left || dxx >= dst->clip.right) continue;
            drow[dxx] = srow[sxx];
        }
    }
    mark_dirty(dst, dx, dy, dx + w, dy + h);
}

static BOOL WINAPI gdi_BitBlt(HANDLE hdc, int x, int y, int w, int h, HANDLE hsrc, int sx, int sy, DWORD rop)
{
    struct dc *d = D(hdc);
    if (rop == 0x00000042) { dc_fill(d, x, y, x + w, y + h, 0x000000); mark_dirty(d, x, y, x + w, y + h); return 1; }       /* BLACKNESS */
    if (rop == 0x00FF0062) { dc_fill(d, x, y, x + w, y + h, 0xFFFFFF); mark_dirty(d, x, y, x + w, y + h); return 1; }       /* WHITENESS */
    if (rop == 0x00F00021) { dc_fill(d, x, y, x + w, y + h, d->brush->color); mark_dirty(d, x, y, x + w, y + h); return 1; } /* PATCOPY */
    if (!hsrc) return 0;
    blit(d, x, y, w, h, D(hsrc), sx, sy);
    return 1;
}

static BOOL WINAPI gdi_StretchBlt(HANDLE hdc, int x, int y, int w, int h, HANDLE hsrc, int sx, int sy, int sw, int sh, DWORD rop)
{
    struct dc *d = D(hdc), *s = D(hsrc);
    if (!s || !w || !h || !sw || !sh) return 0;
    int aw = abs_i32(w), ah = abs_i32(h);
    for (int yy = 0; yy < ah; yy++)
        for (int xx = 0; xx < aw; xx++) {
            int px = sx + (long)xx * abs_i32(sw) / aw, py = sy + (long)yy * abs_i32(sh) / ah;
            px += s->ox; py += s->oy;
            if (px < 0 || py < 0 || px >= s->w || py >= s->h) continue;
            dc_pset(d, (w < 0 ? x + aw - 1 - xx : x + xx), (h < 0 ? y + ah - 1 - yy : y + yy), dc_row(s, py)[px]);
        }
    mark_dirty(d, x, y, x + aw, y + ah);
    return 1;
}

/* A DIB (BITMAPINFO + pixel data) drawn scaled; 24 and 32 bits per pixel. */
static int WINAPI gdi_StretchDIBits(HANDLE hdc, int x, int y, int w, int h, int sx, int sy, int sw, int sh, const u8 *bits, const u8 *bmi, UINT usage, DWORD rop)
{
    struct dc *d = D(hdc);
    i32 bw = (i32)rd32(bmi + 4), bh = (i32)rd32(bmi + 8);
    int bpp = rd16(bmi + 14), up = bh > 0;
    if (bh < 0) bh = -bh;
    if ((bpp != 32 && bpp != 24) || bw <= 0 || bh <= 0 || sw <= 0 || sh <= 0 || w <= 0 || h <= 0) return 0;
    int stride = ((bw * bpp + 31) / 32) * 4;
    for (int yy = 0; yy < h; yy++) {
        int srow = sy + (long)yy * sh / h;
        if (srow < 0 || srow >= bh) continue;
        const u8 *row = bits + (long)(up ? bh - 1 - srow : srow) * stride;
        for (int xx = 0; xx < w; xx++) {
            int scol = sx + (long)xx * sw / w;
            if (scol < 0 || scol >= bw) continue;
            const u8 *p = row + scol * (bpp / 8);
            dc_pset(d, x + xx, y + yy, p[2] << 16 | p[1] << 8 | p[0]);
        }
    }
    mark_dirty(d, x, y, x + w, y + h);
    return h;
}

static int WINAPI gdi_SetDIBitsToDevice(HANDLE hdc, int x, int y, DWORD w, DWORD h, int sx, int sy, UINT start, UINT lines, const u8 *bits, const u8 *bmi, UINT usage)
{
    return gdi_StretchDIBits(hdc, x, y, w, h, sx, sy, w, h, bits, bmi, usage, 0);
}

static int WINAPI gdi_SaveDC(HANDLE hdc) { return 1; }
static BOOL WINAPI gdi_RestoreDC(HANDLE hdc, int n) { return 1; }
static int WINAPI gdi_SetROP2(HANDLE hdc, int m) { return 13; }
static int WINAPI gdi_SetMapMode(HANDLE hdc, int m) { return 1; }
static int WINAPI gdi_SetStretchBltMode(HANDLE hdc, int m) { return 1; }
static int WINAPI gdi_IntersectClipRect(HANDLE hdc, int l, int t, int r, int b)
{
    struct dc *d = D(hdc);
    if (l + d->ox > d->clip.left) d->clip.left = l + d->ox;
    if (t + d->oy > d->clip.top) d->clip.top = t + d->oy;
    if (r + d->ox < d->clip.right) d->clip.right = r + d->ox;
    if (b + d->oy < d->clip.bottom) d->clip.bottom = b + d->oy;
    return 2;
}
