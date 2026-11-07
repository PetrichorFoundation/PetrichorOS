#include "gfx.h"
#include "font8x16.h"

static u32 *tgt;
static int tw, th;
static struct rect clip;

void gfx_set_target(u32 *px, int w, int h)
{
    tgt = px;
    tw = w;
    th = h;
    clip = (struct rect){0, 0, w, h};
}

void gfx_set_clip(int x, int y, int w, int h)
{
    clip = rect_isect((struct rect){x, y, w, h}, (struct rect){0, 0, tw, th});
}

struct rect gfx_get_clip(void)
{
    return clip;
}

struct rect rect_isect(struct rect a, struct rect b)
{
    int x0 = a.x > b.x ? a.x : b.x, y0 = a.y > b.y ? a.y : b.y;
    int x1 = a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h;
    if (x1 <= x0 || y1 <= y0)
        return (struct rect){0, 0, 0, 0};
    return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

struct rect rect_union(struct rect a, struct rect b)
{
    if (a.w <= 0 || a.h <= 0)
        return b;
    if (b.w <= 0 || b.h <= 0)
        return a;
    int x0 = a.x < b.x ? a.x : b.x, y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return (struct rect){x0, y0, x1 - x0, y1 - y0};
}

int rect_hit(struct rect r, int x, int y)
{
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

u32 gfx_mix(u32 a, u32 b, int t)
{
    u32 rb = (((a & 0xFF00FF) * (256 - t) + (b & 0xFF00FF) * t) >> 8) & 0xFF00FF;
    u32 g = (((a & 0x00FF00) * (256 - t) + (b & 0x00FF00) * t) >> 8) & 0x00FF00;
    return rb | g;
}

void gfx_fill(int x, int y, int w, int h, u32 c)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, clip);
    for (int row = 0; row < r.h; row++) {
        u32 *p = tgt + (r.y + row) * tw + r.x;
        u32 n = r.w;
        __asm__ volatile("cld; rep stosl" : "+D"(p), "+c"(n) : "a"(c) : "memory");
    }
}

void gfx_hline(int x, int y, int w, u32 c) { gfx_fill(x, y, w, 1, c); }
void gfx_vline(int x, int y, int h, u32 c) { gfx_fill(x, y, 1, h, c); }

void gfx_frame(int x, int y, int w, int h, u32 c)
{
    gfx_hline(x, y, w, c);
    gfx_hline(x, y + h - 1, w, c);
    gfx_vline(x, y, h, c);
    gfx_vline(x + w - 1, y, h, c);
}

void gfx_vgrad(int x, int y, int w, int h, u32 top, u32 bottom)
{
    for (int i = 0; i < h; i++)
        gfx_fill(x, y + i, w, 1, gfx_mix(top, bottom, h > 1 ? i * 256 / (h - 1) : 0));
}

void gfx_blend(int x, int y, int w, int h, u32 c, int alpha)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, clip);
    for (int row = 0; row < r.h; row++) {
        u32 *p = tgt + (r.y + row) * tw + r.x;
        for (int i = 0; i < r.w; i++)
            p[i] = gfx_mix(p[i], c, alpha);
    }
}

void gfx_copy(const u32 *src, int x, int y, int w, int h)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, clip);
    for (int row = 0; row < r.h; row++) {
        int off = (r.y + row) * tw + r.x;
        memcpy(tgt + off, src + off, r.w * 4);
    }
}

void gfx_blit_argb(const u32 *src, int x, int y, int w, int h)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, clip);
    for (int row = 0; row < r.h; row++) {
        const u32 *s = src + (r.y - y + row) * w + (r.x - x);
        u32 *d = tgt + (r.y + row) * tw + r.x;
        for (int i = 0; i < r.w; i++) {
            u32 p = s[i], a = p >> 24;
            if (a == 0)
                continue;
            if (a == 255) {
                d[i] = p & 0xFFFFFF;
                continue;
            }
            u32 inv = 255 - a, o = d[i];
            u32 rr = ((p >> 16) & 255) + (((o >> 16) & 255) * inv + 127) / 255;
            u32 gg = ((p >> 8) & 255) + (((o >> 8) & 255) * inv + 127) / 255;
            u32 bb = (p & 255) + ((o & 255) * inv + 127) / 255;
            d[i] = (rr > 255 ? 255 : rr) << 16 | (gg > 255 ? 255 : gg) << 8 | (bb > 255 ? 255 : bb);
        }
    }
}

void gfx_char(int x, int y, int ch, u32 fg)
{
    if (ch < 32 || ch > 126)
        return;
    struct rect r = rect_isect((struct rect){x, y, 8, 16}, clip);
    if (r.w == 0)
        return;
    const unsigned char *g = font8x16[ch - 32];
    for (int row = r.y - y; row < r.y - y + r.h; row++) {
        unsigned char bits = g[row];
        if (!bits)
            continue;
        u32 *p = tgt + (y + row) * tw;
        for (int col = r.x - x; col < r.x - x + r.w; col++)
            if (bits & (0x80 >> col))
                p[x + col] = fg;
    }
}

int gfx_text_n(int x, int y, const char *s, int n, u32 fg)
{
    for (int i = 0; i < n && s[i]; i++, x += 8)
        gfx_char(x, y, s[i], fg);
    return x;
}

int gfx_text(int x, int y, const char *s, u32 fg)
{
    return gfx_text_n(x, y, s, 1 << 20, fg);
}

int gfx_text_width(const char *s)
{
    return strlen(s) * 8;
}

void gfx_text_scaled(int x, int y, const char *s, u32 fg, int scale)
{
    for (; *s; s++, x += 8 * scale) {
        if (*s < 32 || *s > 126)
            continue;
        const unsigned char *g = font8x16[*s - 32];
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (g[row] & (0x80 >> col))
                    gfx_fill(x + col * scale, y + row * scale, scale, scale, fg);
    }
}

/* Blends `color` through an 8-bit coverage mask (w*h bytes). */
void gfx_blit_a8(const u8 *a, int x, int y, int w, int h, u32 color)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, clip);
    u32 cr = (color >> 16) & 255, cg = (color >> 8) & 255, cb = color & 255;
    for (int row = 0; row < r.h; row++) {
        const u8 *s = a + (r.y - y + row) * w + (r.x - x);
        u32 *d = tgt + (r.y + row) * tw + r.x;
        for (int i = 0; i < r.w; i++) {
            u32 m = s[i];
            if (!m)
                continue;
            if (m == 255) {
                d[i] = color & 0xFFFFFF;
                continue;
            }
            u32 o = d[i], inv = 255 - m;
            u32 rr = (cr * m + ((o >> 16) & 255) * inv + 127) / 255;
            u32 gg = (cg * m + ((o >> 8) & 255) * inv + 127) / 255;
            u32 bb = (cb * m + (o & 255) * inv + 127) / 255;
            d[i] = rr << 16 | gg << 8 | bb;
        }
    }
}

void gfx_hgrad(int x, int y, int w, int h, u32 left, u32 right)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, clip);
    for (int i = 0; i < r.w; i++)
        gfx_vline(r.x + i, r.y, r.h, gfx_mix(left, right, w > 1 ? (r.x + i - x) * 256 / (w - 1) : 0));
}

static int isqrt(int v)
{
    int r = 0, b = 1 << 24;
    while (b > v)
        b >>= 2;
    while (b) {
        if (v >= r + b) {
            v -= r + b;
            r = (r >> 1) + b;
        } else
            r >>= 1;
        b >>= 2;
    }
    return r;
}

/* Coverage 0..255 of pixel (i, j) in a w*h rounded rectangle with corner radius r. */
static int rr_cov(int i, int j, int w, int h, int r)
{
    int cx = i < r ? r : i >= w - r ? w - 1 - r : -1, cy = j < r ? r : j >= h - r ? h - 1 - r : -1;
    if (cx < 0 || cy < 0)
        return 255;
    int dx = (i - cx) * 16, dy = (j - cy) * 16;
    int d = isqrt(dx * dx + dy * dy);                    /* 1/16 px */
    int c = (r * 16 + 8 - d) * 255 / 16;
    return c < 0 ? 0 : c > 255 ? 255 : c;
}

/* Rounded rectangle, anti-aliased. ring > 0 draws only a border of that width. alpha comes from the color. */
void gfx_round_rect(int x, int y, int w, int h, int r, u32 argb, int ring)
{
    if (w <= 0 || h <= 0)
        return;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    u32 a = argb >> 24 ? argb >> 24 : 255;
    struct rect cl = rect_isect((struct rect){x, y, w, h}, clip);
    for (int j = cl.y - y; j < cl.y - y + cl.h; j++) {
        u32 *d = tgt + (y + j) * tw;
        for (int i = cl.x - x; i < cl.x - x + cl.w; i++) {
            int c = rr_cov(i, j, w, h, r);
            if (!c)
                continue;
            if (ring > 0) {
                int iw = w - 2 * ring, ih = h - 2 * ring, ir = r > ring ? r - ring : 0;
                int ii = i - ring, jj = j - ring;
                if (ii >= 0 && jj >= 0 && ii < iw && jj < ih) {
                    int in = rr_cov(ii, jj, iw, ih, ir);
                    c = c - in < 0 ? 0 : c - in;
                    if (!c)
                        continue;
                }
            }
            u32 m = c * a / 255, o = d[x + i], inv = 255 - m;
            u32 rr = (((argb >> 16) & 255) * m + ((o >> 16) & 255) * inv + 127) / 255;
            u32 gg = (((argb >> 8) & 255) * m + ((o >> 8) & 255) * inv + 127) / 255;
            u32 bb = ((argb & 255) * m + (o & 255) * inv + 127) / 255;
            d[x + i] = rr << 16 | gg << 8 | bb;
        }
    }
}

void gfx_blit_rgb(const u32 *src, int x, int y, int w, int h)
{
    struct rect r = rect_isect((struct rect){x, y, w, h}, clip);
    for (int row = 0; row < r.h; row++)
        memcpy(tgt + (r.y + row) * tw + r.x, src + (r.y - y + row) * w + (r.x - x), r.w * 4);
}
