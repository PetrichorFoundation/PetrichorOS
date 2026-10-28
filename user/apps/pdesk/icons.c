/* Icons: drawn procedurally on a 64x64 design grid with an anti-aliased rasterizer
 * (rounded rectangles, circles, ellipses, thick lines, polygons, gradients, soft shadows),
 * then scaled down with smoothing to the requested size and cached.
 *
 * All arithmetic is integer: shapes are tested at 4x4 sub-pixel samples on an 8x-scaled grid. */
#include "desk.h"

#define M 64

/* ---------------- rasterizer ---------------- */

static u32 mst[M * M];                 /* master being built: premultiplied 0xAARRGGBB */
static int g_erase;                    /* when set, shapes remove ink instead of adding it */

#define K(rgb)       (0xFF000000u | (rgb))
#define KA(a, rgb)   (((u32)(a) << 24) | (rgb))

static u32 lerp_col(u32 a, u32 b, int t)     /* t: 0..256, straight ARGB */
{
    u32 out = 0;
    for (int sh = 0; sh <= 24; sh += 8) {
        int ca = (a >> sh) & 255, cb = (b >> sh) & 255;
        out |= (u32)((ca * (256 - t) + cb * t) >> 8) << sh;
    }
    return out;
}

static void composite(int x, int y, u32 c, int cov)
{
    if (x < 0 || y < 0 || x >= M || y >= M || cov <= 0)
        return;
    u32 *d = &mst[y * M + x];
    if (g_erase) {
        u32 keep = 255 - cov, o = 0;
        for (int sh = 0; sh <= 24; sh += 8)
            o |= (((*d >> sh) & 255) * keep / 255) << sh;
        *d = o;
        return;
    }
    u32 a = ((c >> 24) * cov) / 255;
    if (a == 0)
        return;
    u32 sr = ((c >> 16) & 255) * a / 255, sg = ((c >> 8) & 255) * a / 255, sb = (c & 255) * a / 255;
    u32 inv = 255 - a, o = *d;
    u32 oa = a + (o >> 24) * inv / 255;
    u32 or_ = sr + ((o >> 16) & 255) * inv / 255;
    u32 og = sg + ((o >> 8) & 255) * inv / 255;
    u32 ob = sb + (o & 255) * inv / 255;
    *d = (oa > 255 ? 255 : oa) << 24 | (or_ > 255 ? 255 : or_) << 16 | (og > 255 ? 255 : og) << 8 | (ob > 255 ? 255 : ob);
}

/* For each pixel in the box, count the covered sub-samples of TEST (X, Y are in 1/8 pixel). */
#define RASTER(bx0, by0, bx1, by1, gy0, gy1, c1, c2, TEST)                                            \
    do {                                                                                              \
        int _y0 = (by0) < 0 ? 0 : (by0), _y1 = (by1) > M ? M : (by1);                                 \
        int _x0 = (bx0) < 0 ? 0 : (bx0), _x1 = (bx1) > M ? M : (bx1);                                 \
        for (int y = _y0; y < _y1; y++) {                                                             \
            int _t = (gy1) > (gy0) ? ((y - (gy0)) * 256) / ((gy1) - (gy0)) : 0;                       \
            _t = _t < 0 ? 0 : _t > 256 ? 256 : _t;                                                    \
            u32 _c = lerp_col(c1, c2, _t);                                                            \
            for (int x = _x0; x < _x1; x++) {                                                         \
                int n = 0;                                                                            \
                for (int sy = 0; sy < 4; sy++)                                                        \
                    for (int sx = 0; sx < 4; sx++) {                                                  \
                        int X = x * 8 + sx * 2 + 1, Y = y * 8 + sy * 2 + 1;                           \
                        if (TEST)                                                                     \
                            n++;                                                                      \
                    }                                                                                 \
                if (n)                                                                                \
                    composite(x, y, _c, n * 255 / 16);                                                \
            }                                                                                         \
        }                                                                                             \
    } while (0)

static int sq(int v) { return v * v; }

/* Rounded rectangle [x0,x1) x [y0,y1) with corner radius r, vertical gradient c1 -> c2. */
static void rr(int x0, int y0, int x1, int y1, int r, u32 c1, u32 c2)
{
    int L = x0 * 8, T = y0 * 8, R = x1 * 8, B = y1 * 8, rad = r * 8;
    RASTER(x0 - 1, y0 - 1, x1 + 1, y1 + 1, y0, y1, c1, c2,
           X >= L && X < R && Y >= T && Y < B &&
           !(X < L + rad && Y < T + rad && sq(X - L - rad) + sq(Y - T - rad) > sq(rad)) &&
           !(X >= R - rad && Y < T + rad && sq(X - R + rad) + sq(Y - T - rad) > sq(rad)) &&
           !(X < L + rad && Y >= B - rad && sq(X - L - rad) + sq(Y - B + rad) > sq(rad)) &&
           !(X >= R - rad && Y >= B - rad && sq(X - R + rad) + sq(Y - B + rad) > sq(rad)));
}

static void ci(int cx, int cy, int r, u32 c1, u32 c2)
{
    int ox = cx * 8, oy = cy * 8, rad = r * 8;
    RASTER(cx - r - 1, cy - r - 1, cx + r + 1, cy + r + 1, cy - r, cy + r, c1, c2,
           sq(X - ox) + sq(Y - oy) <= sq(rad));
}

static void el(int cx, int cy, int rx, int ry, u32 c1, u32 c2)
{
    int ox = cx * 8, oy = cy * 8, a = rx * 8, b = ry * 8;
    RASTER(cx - rx - 1, cy - ry - 1, cx + rx + 1, cy + ry + 1, cy - ry, cy + ry, c1, c2,
           sq((X - ox) * b / 8) + sq((Y - oy) * a / 8) <= sq(a * b / 8));
}

static void rg(int cx, int cy, int ro, int ri, u32 c)
{
    int ox = cx * 8, oy = cy * 8;
    RASTER(cx - ro - 1, cy - ro - 1, cx + ro + 1, cy + ro + 1, 0, 0, c, c,
           sq(X - ox) + sq(Y - oy) <= sq(ro * 8) && sq(X - ox) + sq(Y - oy) >= sq(ri * 8));
}

/* Thick line with round caps; wq is the width in half pixels. */
static void sg(int x0, int y0, int x1, int y1, int wq, u32 c)
{
    int ax = x0 * 8, ay = y0 * 8, bx = x1 * 8, by = y1 * 8, hw = wq * 2;
    int dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
    int mnx = (x0 < x1 ? x0 : x1) - wq / 2 - 2, mxx = (x0 > x1 ? x0 : x1) + wq / 2 + 3;
    int mny = (y0 < y1 ? y0 : y1) - wq / 2 - 2, mxy = (y0 > y1 ? y0 : y1) + wq / 2 + 3;
    RASTER(mnx, mny, mxx, mxy, 0, 0, c, c, ({
        int px = X - ax, py = Y - ay, t = px * dx + py * dy;
        if (t < 0) t = 0;
        if (t > len2) t = len2;
        int qx = len2 ? dx * t / len2 : 0, qy = len2 ? dy * t / len2 : 0;
        sq(px - qx) + sq(py - qy) <= sq(hw);
    }));
}

/* Polygon (even-odd), vertical gradient over its own height. pts = x0,y0,x1,y1,... */
static void pl(int cnt, const int *pts, u32 c1, u32 c2)
{
    int mnx = 999, mxx = -1, mny = 999, mxy = -1;
    for (int i = 0; i < cnt; i++) {
        if (pts[2 * i] < mnx) mnx = pts[2 * i];
        if (pts[2 * i] > mxx) mxx = pts[2 * i];
        if (pts[2 * i + 1] < mny) mny = pts[2 * i + 1];
        if (pts[2 * i + 1] > mxy) mxy = pts[2 * i + 1];
    }
    RASTER(mnx - 1, mny - 1, mxx + 2, mxy + 2, mny, mxy, c1, c2, ({
        int inside = 0;
        for (int i = 0, j = cnt - 1; i < cnt; j = i++) {
            int xi = pts[2 * i] * 8, yi = pts[2 * i + 1] * 8, xj = pts[2 * j] * 8, yj = pts[2 * j + 1] * 8;
            if ((yi > Y) != (yj > Y) && X < (xj - xi) * (Y - yi) / (yj - yi) + xi)
                inside = !inside;
        }
        inside;
    }));
}

static void shadow(int cx, int cy, int rx, int ry)
{
    el(cx, cy, rx + 3, ry + 2, KA(26, 0x000000), KA(26, 0x000000));
    el(cx, cy, rx, ry, KA(52, 0x000000), KA(52, 0x000000));
}

/* ---------------- the icons ---------------- */

static void page_base(void)
{
    shadow(32, 58, 18, 3);
    rr(12, 3, 52, 61, 4, K(0x8996a8), K(0x7a8799));              /* outline */
    rr(13, 4, 51, 60, 3, K(0xffffff), K(0xe6ebf2));              /* paper */
    g_erase = 1;                                                 /* cut the folded corner */
    { static const int c[] = {35, 3, 53, 3, 53, 21}; pl(3, c, 0, 0); }
    g_erase = 0;
    { static const int f[] = {35, 4, 51, 20, 35, 20}; pl(3, f, K(0xdde3ec), K(0xb4bfcd)); }
    sg(35, 4, 35, 20, 2, K(0x8996a8));
    sg(35, 20, 51, 20, 2, K(0x8996a8));
    sg(35, 4, 51, 20, 2, K(0x9ba7b8));
}

static void text_lines(int n, u32 c)
{
    for (int i = 0; i < n; i++)
        sg(19, 28 + i * 6, i == n - 1 ? 34 : 45, 28 + i * 6, 3, c);
}

static void folder(int tint)
{
    u32 b1 = tint ? K(0x4a82c8) : K(0xd9a229), b2 = tint ? K(0x3a68a8) : K(0xc08818);
    u32 f1 = tint ? K(0x8ec4ff) : K(0xffdb6e), f2 = tint ? K(0x5b9ae6) : K(0xf0b236);
    shadow(32, 58, 24, 3);
    rr(5, 12, 29, 26, 4, b1, b2);
    rr(4, 17, 60, 54, 6, b1, b2);
    rr(9, 16, 53, 42, 2, K(0xfbfbf7), K(0xdcdcd2));              /* a sheet peeking out */
    sg(14, 22, 44, 22, 3, K(0xb5bdc8));
    sg(14, 28, 36, 28, 3, K(0xb5bdc8));
    rr(3, 26, 61, 57, 6, f1, f2);
    sg(9, 29, 55, 29, 3, KA(130, 0xffffff));
    sg(7, 55, 57, 55, 3, KA(50, 0x000000));
}

static void terminal(void)
{
    shadow(32, 58, 24, 3);
    rr(3, 7, 61, 55, 7, K(0x59656f), K(0x2b323b));
    rr(3, 7, 61, 21, 7, K(0x7b8794), K(0x56626f));
    rr(3, 15, 61, 22, 0, K(0x6a7683), K(0x56626f));
    ci(11, 14, 2, K(0xff6a62), K(0xe0443c));
    ci(18, 14, 2, K(0xffc83d), K(0xe0a21a));
    ci(25, 14, 2, K(0x35d04c), K(0x1faa38));
    rr(7, 24, 57, 51, 3, K(0x0d1116), K(0x1a2129));
    sg(12, 31, 19, 36, 3, K(0x5ff58a));
    sg(19, 36, 12, 41, 3, K(0x5ff58a));
    sg(23, 42, 34, 42, 3, K(0xe2e6ea));
    sg(25, 31, 47, 31, 3, K(0x8da0b6));
    sg(25, 36, 39, 36, 3, K(0x6b7f96));
    sg(8, 25, 56, 25, 2, KA(40, 0xffffff));
}

static void editor(void)
{
    shadow(32, 58, 20, 3);
    rr(10, 6, 46, 59, 4, K(0x8f8a78), K(0x77725f));
    rr(11, 7, 45, 58, 3, K(0xfffef8), K(0xf0ecdc));
    rr(11, 7, 45, 17, 3, K(0xee6a5a), K(0xc9473a));
    rr(11, 12, 45, 18, 0, K(0xe05a4b), K(0xc9473a));
    for (int i = 0; i < 4; i++)
        ci(17 + i * 8, 9, 2, K(0x55493f), K(0x2f2822));
    for (int i = 0; i < 6; i++)
        sg(16, 25 + i * 5, 40, 25 + i * 5, 2, K(0xb9cdee));
    sg(16, 25, 16, 55, 2, K(0xf29a92));
    sg(22, 26, 36, 26, 3, K(0x56667e));
    sg(22, 31, 32, 31, 3, K(0x56667e));
    { static const int pb[] = {38, 50, 55, 24, 61, 28, 44, 54}; pl(4, pb, K(0xffd23c), K(0xe6a416)); }
    { static const int pt[] = {38, 50, 44, 54, 36, 58}; pl(3, pt, K(0xf3d3a2), K(0xd9ae72)); }
    { static const int ln[] = {36, 58, 37, 55, 39, 56}; pl(3, ln, K(0x3a3a3a), K(0x1a1a1a)); }
    { static const int er[] = {55, 24, 57, 20, 63, 24, 61, 28}; pl(4, er, K(0xff8fa0), K(0xe05a74)); }
    sg(41, 52, 57, 26, 2, KA(110, 0xffffff));
}

static void monitor(void)
{
    shadow(32, 59, 22, 3);
    rr(3, 6, 61, 46, 6, K(0x66727f), K(0x333b45));
    rr(7, 10, 57, 42, 3, K(0x0c1116), K(0x171e26));
    for (int i = 0; i < 3; i++)
        sg(9, 18 + i * 8, 55, 18 + i * 8, 1, KA(36, 0x7fdcff));
    for (int i = 0; i < 5; i++)
        sg(15 + i * 10, 11, 15 + i * 10, 41, 1, KA(24, 0x7fdcff));
    static const int hs[] = {10, 16, 8, 20, 13};
    for (int i = 0; i < 5; i++)
        rr(11 + i * 9, 40 - hs[i], 17 + i * 9, 40, 1, K(0x7af09a), K(0x2fb35a));
    sg(10, 33, 22, 22, 3, K(0xffd23c));
    sg(22, 22, 32, 28, 3, K(0xffd23c));
    sg(32, 28, 44, 15, 3, K(0xffd23c));
    sg(44, 15, 55, 20, 3, K(0xffd23c));
}

static void files_icon(void)
{
    folder(1);
    ci(44, 44, 10, K(0xf4f9ff), K(0xc8d8ee));                    /* magnifier */
    rg(44, 44, 10, 7, K(0x4a5a72));
    sg(51, 51, 59, 59, 7, K(0x4a5a72));
    el(40, 40, 4, 2, KA(120, 0xffffff), KA(120, 0xffffff));
}

static void about(void)
{
    shadow(32, 59, 20, 3);
    ci(32, 31, 27, K(0x6db6ff), K(0x2a62c4));
    rg(32, 31, 27, 25, KA(90, 0x163f86));
    el(24, 17, 15, 8, KA(120, 0xffffff), KA(10, 0xffffff));
    ci(32, 19, 4, K(0xffffff), K(0xdce8fb));
    rr(28, 27, 36, 49, 3, K(0xffffff), K(0xdce8fb));
}

static void disk(void)
{
    shadow(32, 57, 26, 3);
    rr(4, 20, 60, 52, 6, K(0xd8dee6), K(0x9099a6));
    rr(4, 20, 60, 28, 6, KA(255, 0xeef2f7), K(0xc2cad4));
    rr(8, 29, 56, 47, 3, K(0x808a97), K(0x59626f));
    for (int i = 0; i < 5; i++)
        sg(12, 33 + i * 3, 38, 33 + i * 3, 1, K(0x3d444d));
    ci(48, 38, 3, K(0x6dff8a), K(0x1fc04a));
    rg(48, 38, 6, 4, KA(60, 0x4dff77));
    rr(42, 43, 53, 45, 1, K(0x39414b), K(0x39414b));
    sg(7, 22, 57, 22, 2, KA(160, 0xffffff));
}

static void exec_file(void)
{
    page_base();
    text_lines(2, K(0xaab6c6));
    ci(43, 44, 13, K(0x58dd7a), K(0x239443));
    rg(43, 44, 13, 11, KA(80, 0x0c5a24));
    { static const int tr[] = {39, 37, 39, 51, 52, 44}; pl(3, tr, K(0xffffff), K(0xdaf5e2)); }
}

static void apps(void)
{
    static const u32 c1[9] = {K(0x6db6ff), K(0x7bd88f), K(0xffb35c), K(0xc89cff), K(0xff8a7a), K(0x5fd8d0), K(0xffe066), K(0xff9ec8), K(0xbac4d0)};
    static const u32 c2[9] = {K(0x2f6fd0), K(0x2f9a52), K(0xe0781a), K(0x7d4bc8), K(0xd64538), K(0x1fa39a), K(0xe0b81a), K(0xd8508c), K(0x808a98)};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            int x = 5 + c * 19, y = 5 + r * 19;
            rr(x, y, x + 16, y + 16, 4, c1[r * 3 + c], c2[r * 3 + c]);
            sg(x + 3, y + 3, x + 13, y + 3, 2, KA(110, 0xffffff));
        }
}

static void power(void)
{
    shadow(32, 59, 20, 3);
    ci(32, 31, 27, K(0xff8a78), K(0xc4302a));
    rg(32, 31, 27, 25, KA(70, 0x7a1410));
    el(24, 17, 15, 8, KA(110, 0xffffff), KA(10, 0xffffff));
    rg(32, 34, 13, 9, K(0xffffff));
    g_erase = 1;
    rr(28, 16, 36, 30, 0, 0, 0);
    g_erase = 0;
    sg(32, 17, 32, 33, 7, K(0xffffff));
}

static void package(void)
{
    shadow(32, 59, 24, 3);
    rr(8, 26, 56, 58, 4, K(0xe0af68), K(0xb98438));
    rr(4, 14, 60, 30, 4, K(0xf5cd8c), K(0xdba755));
    rr(27, 14, 37, 58, 0, K(0xf6ead0), K(0xdccaa0));
    rr(27, 14, 37, 30, 0, K(0xfbf3df), K(0xe8d7ae));
    rr(40, 40, 52, 50, 2, K(0xffffff), K(0xe3e8ef));
    sg(42, 44, 50, 44, 2, K(0x9aa6b6));
    sg(42, 47, 47, 47, 2, K(0x9aa6b6));
    sg(7, 29, 57, 29, 2, KA(60, 0x000000));
    sg(8, 16, 56, 16, 2, KA(130, 0xffffff));
}

static void logout(void)
{
    shadow(32, 58, 22, 3);
    rr(8, 6, 38, 58, 3, K(0xb98a5c), K(0x84603a));
    rr(11, 9, 35, 55, 2, K(0xd9ab76), K(0xa8794a));
    ci(31, 33, 2, K(0xffe08a), K(0xd9a830));
    sg(26, 32, 50, 32, 7, K(0x3d8bf0));
    { static const int h[] = {46, 20, 62, 32, 46, 44}; pl(3, h, K(0x6db0ff), K(0x2f74dc)); }
}

static void home(void)
{
    shadow(32, 58, 24, 3);
    rr(10, 28, 54, 57, 2, K(0xf7ecd8), K(0xdcc8a4));
    rr(40, 10, 48, 28, 1, K(0xb5523f), K(0x8a3a2c));
    { static const int roof[] = {3, 31, 32, 7, 61, 31}; pl(3, roof, K(0xe8604a), K(0xb23a2c)); }
    sg(5, 31, 32, 9, 2, KA(120, 0xffffff));
    rr(26, 38, 38, 57, 2, K(0x9a6a44), K(0x6e4528));
    ci(35, 48, 1, K(0xffe08a), K(0xffe08a));
    rr(14, 35, 24, 45, 1, K(0xaee0ff), K(0x6aaee6));
    sg(19, 35, 19, 45, 1, K(0xffffff));
    sg(14, 40, 24, 40, 1, K(0xffffff));
    rr(42, 35, 52, 45, 1, K(0xaee0ff), K(0x6aaee6));
    sg(47, 35, 47, 45, 1, K(0xffffff));
    sg(42, 40, 52, 40, 1, K(0xffffff));
}

static void settings(void)
{
    static const int tip[8][2] = {{32, 5}, {51, 13}, {59, 32}, {51, 51}, {32, 59}, {13, 51}, {5, 32}, {13, 13}};
    shadow(32, 59, 22, 3);
    for (int i = 0; i < 8; i++)
        sg(32, 32, tip[i][0], tip[i][1], 14, K(0x8b97a6));
    ci(32, 32, 21, K(0xd2dae4), K(0x8b97a6));
    rg(32, 32, 21, 19, KA(110, 0x4a5666));
    ci(32, 32, 9, K(0x39424e), K(0x1e242c));
    rg(32, 32, 9, 7, KA(120, 0xffffff));
    el(26, 22, 11, 5, KA(120, 0xffffff), KA(10, 0xffffff));
}

static void install(void)
{
    shadow(32, 59, 24, 3);
    rr(4, 32, 60, 56, 6, K(0xd8dee6), K(0x9099a6));
    rr(8, 36, 56, 52, 3, K(0x808a97), K(0x59626f));
    ci(48, 44, 3, K(0x6dff8a), K(0x1fc04a));
    { static const int a[] = {24, 3, 40, 3, 40, 18, 52, 18, 32, 34, 12, 18, 24, 18}; pl(7, a, K(0x7dc0ff), K(0x2a74e0)); }
    sg(24, 5, 24, 17, 2, KA(120, 0xffffff));
}

static void restart(void)
{
    shadow(32, 59, 20, 3);
    ci(32, 31, 27, K(0x6db6ff), K(0x2a62c4));
    el(24, 17, 15, 8, KA(110, 0xffffff), KA(10, 0xffffff));
    rg(32, 32, 14, 9, K(0xffffff));
    g_erase = 1;
    { static const int w[] = {32, 32, 50, 14, 50, 32}; pl(3, w, 0, 0); }
    g_erase = 0;
    { static const int h[] = {34, 18, 52, 22, 40, 35}; pl(3, h, K(0xffffff), K(0xdce8fb)); }
}

static void run(void)
{
    shadow(32, 58, 24, 3);
    rr(3, 10, 61, 54, 6, K(0x6a7684), K(0x3d4652));
    rr(5, 12, 59, 22, 4, K(0x5aa0f0), K(0x2f6fd0));
    rr(5, 17, 59, 22, 0, K(0x4a90e0), K(0x2f6fd0));
    rr(5, 23, 59, 52, 3, K(0xfbfcfe), K(0xe3e8ef));
    sg(11, 31, 17, 36, 3, K(0x2f6fd0));
    sg(17, 36, 11, 41, 3, K(0x2f6fd0));
    sg(22, 42, 36, 42, 3, K(0x3a4656));
    ci(48, 40, 7, K(0x58dd7a), K(0x239443));
    { static const int t[] = {45, 36, 45, 44, 52, 40}; pl(3, t, K(0xffffff), K(0xdaf5e2)); }
}

static void palette(void)
{
    shadow(32, 58, 26, 3);
    el(31, 32, 29, 24, K(0xf0cf9c), K(0xc89a5a));
    g_erase = 1;
    ci(45, 42, 6, 0, 0);
    g_erase = 0;
    ci(16, 30, 5, K(0xff8a7a), K(0xd43a2c));
    ci(25, 18, 5, K(0xffe066), K(0xe0b81a));
    ci(38, 16, 5, K(0x7bd88f), K(0x2f9a52));
    ci(50, 24, 5, K(0x6db6ff), K(0x2f6fd0));
    ci(14, 42, 4, K(0xc89cff), K(0x7d4bc8));
    sg(10, 24, 28, 12, 2, KA(100, 0xffffff));
}

static void web(void)
{
    shadow(32, 59, 20, 3);
    ci(32, 31, 27, K(0x7cc6ff), K(0x1f5fbe));
    static const int land1[] = {14, 22, 22, 14, 30, 16, 28, 24, 34, 28, 30, 36, 22, 34, 16, 30};
    static const int land2[] = {38, 14, 48, 18, 52, 28, 46, 36, 50, 44, 42, 50, 40, 40, 34, 32, 40, 22};
    pl(8, land1, K(0x6ee08a), K(0x2fa052));
    pl(9, land2, K(0x6ee08a), K(0x2fa052));
    sg(6, 31, 58, 31, 1, KA(70, 0xffffff));
    sg(32, 4, 32, 58, 1, KA(70, 0xffffff));
    sg(10, 18, 54, 18, 1, KA(50, 0xffffff));
    sg(10, 44, 54, 44, 1, KA(50, 0xffffff));
    rg(32, 31, 27, 25, KA(90, 0x123a80));
    el(24, 16, 15, 8, KA(120, 0xffffff), KA(10, 0xffffff));
}

static void file_text(void) { page_base(); text_lines(5, K(0xaab6c6)); }

static void file_code(void)
{
    page_base();
    sg(19, 28, 30, 28, 3, K(0x6a9be0));
    sg(19, 34, 40, 34, 3, K(0xaab6c6));
    sg(19, 40, 28, 40, 3, K(0xe09a5a));
    rr(28, 36, 62, 58, 5, K(0x4a8be8), K(0x2a5cb8));
    sg(40, 42, 35, 47, 3, K(0xffffff));
    sg(35, 47, 40, 52, 3, K(0xffffff));
    sg(51, 42, 56, 47, 3, K(0xffffff));
    sg(56, 47, 51, 52, 3, K(0xffffff));
    sg(48, 41, 43, 53, 3, K(0xffffff));
}

static void file_web(void)
{
    page_base();
    text_lines(2, K(0xaab6c6));
    ci(43, 44, 14, K(0x7cc6ff), K(0x1f5fbe));
    sg(30, 44, 56, 44, 1, KA(150, 0xffffff));
    sg(43, 31, 43, 57, 1, KA(150, 0xffffff));
    rg(43, 44, 14, 12, KA(150, 0xffffff));
    el(38, 38, 6, 3, KA(120, 0xffffff), KA(10, 0xffffff));
}

static void file_archive(void)
{
    page_base();
    for (int i = 0; i < 9; i++) {
        int y = 6 + i * 5;
        rr(28 + (i & 1) * 4, y, 32 + (i & 1) * 4, y + 3, 0, K(0x6a7686), K(0x3d4652));
    }
    rr(25, 44, 39, 55, 2, K(0xe6b455), K(0xc08a24));
    sg(30, 50, 34, 50, 3, K(0x7a5a18));
}

/* ---------------- cache and drawing ---------------- */

#define NKINDS 30
#define NSIZES 8

static u32 *masters[NKINDS];
static int sizes[NSIZES];
static u32 *scaled[NKINDS][NSIZES];

static void build(int kind)
{
    memset(mst, 0, sizeof mst);
    g_erase = 0;
    switch (kind) {
    case ICON_FOLDER:   folder(0); break;
    case ICON_FILE:     page_base(); text_lines(3, K(0xb7c1ce)); break;
    case ICON_TERM:     terminal(); break;
    case ICON_EDITOR:   editor(); break;
    case ICON_MONITOR:  monitor(); break;
    case ICON_FILES:    files_icon(); break;
    case ICON_ABOUT:    about(); break;
    case ICON_DISK:     disk(); break;
    case ICON_EXEC:     exec_file(); break;
    case ICON_APPS:     apps(); break;
    case ICON_POWER:    power(); break;
    case ICON_SOFTWARE: package(); break;
    case ICON_LOGOUT:   logout(); break;
    case ICON_HOME:     home(); break;
    case ICON_SETTINGS: settings(); break;
    case ICON_INSTALL:  install(); break;
    case ICON_RESTART:  restart(); break;
    case ICON_RUN:      run(); break;
    case ICON_PALETTE:  palette(); break;
    case ICON_WEB:      web(); break;
    case ICON_FILE_TEXT:    file_text(); break;
    case ICON_FILE_CODE:    file_code(); break;
    case ICON_FILE_WEB:     file_web(); break;
    case ICON_FILE_ARCHIVE: file_archive(); break;
    }
    masters[kind] = malloc(sizeof mst);
    if (masters[kind])
        memcpy(masters[kind], mst, sizeof mst);
}

/* Box-filters the 64x64 master down to size x size (premultiplied, so averaging is exact). */
static u32 *scale_master(const u32 *src, int size)
{
    u32 *out = malloc(size * size * 4);
    if (!out)
        return NULL;
    int k = (M + size - 1) / size;
    if (k > 4) k = 4;
    for (int y = 0; y < size; y++) {
        for (int x = 0; x < size; x++) {
            u32 a = 0, r = 0, g = 0, b = 0;
            for (int j = 0; j < k; j++)
                for (int i = 0; i < k; i++) {
                    int sx = (2 * (x * k + i) + 1) * M / (2 * size * k);
                    int sy = (2 * (y * k + j) + 1) * M / (2 * size * k);
                    u32 p = src[sy * M + sx];
                    a += p >> 24; r += (p >> 16) & 255; g += (p >> 8) & 255; b += p & 255;
                }
            int n = k * k;
            out[y * size + x] = (a / n) << 24 | (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
    return out;
}

void icon_draw(int kind, int x, int y, int size)
{
    if (kind < 0 || kind >= NKINDS)
        return;
    if (!masters[kind])
        build(kind);
    if (!masters[kind])
        return;
    int si = -1;
    for (int i = 0; i < NSIZES; i++)
        if (sizes[i] == size) {
            si = i;
            break;
        }
    if (si < 0) {
        for (int i = 0; i < NSIZES; i++)
            if (!sizes[i]) {
                sizes[i] = size;
                si = i;
                break;
            }
        if (si < 0)
            return;
    }
    if (!scaled[kind][si])
        scaled[kind][si] = size == M ? masters[kind] : scale_master(masters[kind], size);
    if (scaled[kind][si])
        gfx_blit_argb(scaled[kind][si], x, y, size, size);
}

/* ---------------- small widgets ---------------- */

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
