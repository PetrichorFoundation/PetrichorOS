/* Painting the display list, glyph rendering at any size, hit testing. */
#include "web.h"
#include "font8x16.h"

void glyph_info(int g, int *units, int *left);
int web_glyph(int cp);
int utf8_next(const char **ps, const char *e);

/* ---- glyph cache: the 8x16 bitmap font, box-filtered to any pixel size, with proportional advances ---- */

struct glyph {
    u32 key;
    u8 *a;
    short w, h;
};

#define NGLYPH 2048
static struct glyph gcache[NGLYPH];
static int gcount;

void glyph_flush(void)
{
    for (int i = 0; i < NGLYPH; i++) {
        free(gcache[i].a);
        gcache[i].a = NULL;
        gcache[i].key = 0;
    }
    gcount = 0;
}

static struct glyph *glyph_get(int g, int px, int flags)
{
    u32 key = (u32)g << 16 | (u32)px << 4 | (u32)(flags & (CF_BOLD | CF_ITALIC | CF_MONO)) | 0x80000000u;
    u32 h = (key * 2654435761u) >> 21;
    for (int probe = 0; probe < NGLYPH; probe++, h = (h + 1) % NGLYPH) {
        struct glyph *e = &gcache[h];
        if (e->key == key) return e;
        if (e->key) continue;
        if (gcount > NGLYPH * 3 / 4) { glyph_flush(); return glyph_get(g, px, flags); }
        int units, left;
        glyph_info(g, &units, &left);
        if (flags & CF_MONO) { units = 8; left = 0; }
        int ow = (units * px + 8) / 16;
        if (ow < 1) ow = 1;
        int oh = px < 1 ? 1 : px;
        int extra = (flags & CF_BOLD ? 1 : 0) + (flags & CF_ITALIC ? oh / 4 : 0);
        int W = ow + extra;
        u8 *bm = calloc(W * oh, 1);
        if (!bm) return NULL;
        const unsigned char *src = font8x16[g - 32];
        for (int oy = 0; oy < oh; oy++) {
            int y0 = oy * 16 * 16 / oh, y1 = (oy + 1) * 16 * 16 / oh;                 /* 1/16 source rows */
            if (y1 <= y0) y1 = y0 + 1;
            for (int ox = 0; ox < ow; ox++) {
                int x0 = left * 16 + ox * units * 16 / ow, x1 = left * 16 + (ox + 1) * units * 16 / ow;
                if (x1 <= x0) x1 = x0 + 1;
                int sum = 0, area = 0;
                for (int sy = y0 / 16; sy <= (y1 - 1) / 16 && sy < 16; sy++) {
                    int wy = (y1 < (sy + 1) * 16 ? y1 : (sy + 1) * 16) - (y0 > sy * 16 ? y0 : sy * 16);
                    for (int sx = x0 >= 0 ? x0 / 16 : (x0 - 15) / 16; sx <= (x1 - 1) / 16; sx++) {
                        int wx = (x1 < (sx + 1) * 16 ? x1 : (sx + 1) * 16) - (x0 > sx * 16 ? x0 : sx * 16);
                        int on = sx >= 0 && sx < 8 && (src[sy] & (0x80 >> sx));
                        area += wx * wy;
                        if (on) sum += wx * wy;
                    }
                }
                int a = area ? sum * 255 / area : 0;
                a = a * 5 / 4;
                bm[oy * W + ox] = a > 255 ? 255 : a;
            }
        }
        if (flags & CF_BOLD) {
            for (int oy = 0; oy < oh; oy++)
                for (int ox = W - 1; ox > 0; ox--)
                    if (bm[oy * W + ox - 1] > bm[oy * W + ox]) bm[oy * W + ox] = bm[oy * W + ox - 1];
        }
        if (flags & CF_ITALIC) {
            for (int oy = 0; oy < oh; oy++) {
                int sh = (oh - 1 - oy) * (oh / 4) / oh;
                if (sh > 0) {
                    memmove(bm + oy * W + sh, bm + oy * W, W - sh);
                    memset(bm + oy * W, 0, sh);
                }
            }
        }
        e->key = key;
        e->a = bm;
        e->w = W;
        e->h = oh;
        gcount++;
        return e;
    }
    return NULL;
}

int glyph_adv(int ch, int px, int flags);

static void draw_text(int x, int y, const char *s, int n, int px, u32 color, int flags)
{
    const char *e = s + n;
    int x16 = 0;
    while (s < e) {
        int g = web_glyph(utf8_next(&s, e));
        if (g <= 0) continue;
        int gx = x + (x16 + 8) / 16;
        if (g != ' ') {
            struct glyph *gl = glyph_get(g, px, flags);
            if (gl && gl->a) gfx_blit_a8(gl->a, gx, y, gl->w, gl->h, color);
        }
        x16 += glyph_adv(g, px, flags);
    }
}

/* ---- images ---- */

static void draw_image(const struct cmd *c, int x, int y, struct page *p)
{
    struct image *im = p->res[c->img - 1].img;
    if (!im) return;
    if (!(c->flags & 0x40)) {
        const u32 *px = img_scaled(im, c->w, c->h);
        if (px) gfx_blit_argb(px, x, y, c->w, c->h);
        return;
    }
    struct rect old = gfx_get_clip();
    struct rect box = rect_isect((struct rect){x, y, c->w, c->h}, old);
    if (!box.w) return;
    gfx_set_clip(box.x, box.y, box.w, box.h);
    int mode = c->flags >> 1 & 3, dw = im->w, dh = im->h;
    if (mode == 1 || mode == 2) {
        int sw = c->w * 1000 / im->w, sh = c->h * 1000 / im->h;
        int sc = mode == 1 ? (sw > sh ? sw : sh) : (sw < sh ? sw : sh);
        dw = im->w * sc / 1000; dh = im->h * sc / 1000;
    }
    if (dw < 1) dw = 1;
    if (dh < 1) dh = 1;
    int ox = c->flags & 0x10 ? (c->w - dw) / 2 : 0, oy = c->flags & 0x10 ? (c->h - dh) / 2 : 0;
    if (mode == 0) {                                      /* natural size, tiled across the box */
        const u32 *px = img_scaled(im, dw, dh);
        if (px)
            for (int ty = 0; ty < c->h; ty += dh)
                for (int tx = 0; tx < c->w; tx += dw)
                    gfx_blit_argb(px, x + tx, y + ty, dw, dh);
    } else {
        const u32 *px = img_scaled(im, dw, dh);
        if (px) gfx_blit_argb(px, x + ox, y + oy, dw, dh);
    }
    gfx_set_clip(old.x, old.y, old.w, old.h);
}

static void fill_argb(int x, int y, int w, int h, u32 c)
{
    u32 a = c >> 24;
    if (a == 255) gfx_fill(x, y, w, h, c & 0xFFFFFF);
    else if (a) gfx_blend(x, y, w, h, c & 0xFFFFFF, a);
}

void page_draw(struct page *p, struct rect view, int scroll, int hover)
{
    struct rect old = gfx_get_clip();
    struct rect clip = rect_isect(view, old);
    if (!clip.w) return;
    gfx_set_clip(clip.x, clip.y, clip.w, clip.h);
    gfx_fill(view.x, view.y, view.w, view.h, (p->styled ? p->page_bg : 0xFFFFFFFF) & 0xFFFFFF);
    int top = scroll, bot = scroll + view.h;
    for (int i = 0; i < p->ncmds; i++) {
        const struct cmd *c = &p->cmds[i];
        if (c->y > bot || c->y + (c->h > c->fsize ? c->h : c->fsize) + 4 < top) continue;
        int x = view.x + c->x, y = view.y + c->y - scroll;
        switch (c->kind) {
        case K_RECT: fill_argb(x, y, c->w, c->h, c->color); break;
        case K_ROUND:
            gfx_round_rect(x, y, c->w, c->h, c->radius, c->color, c->flags & 2 ? c->flags >> 4 : 0);
            break;
        case K_GRAD:
            if (c->flags & 1) gfx_hgrad(x, y, c->w, c->h, c->color, c->color2);
            else gfx_vgrad(x, y, c->w, c->h, c->color, c->color2);
            break;
        case K_IMAGE: draw_image(c, x, y, p); break;
        case K_TEXT: {
            u32 col = c->color;
            int fl = c->flags;
            if (c->link && c->link - 1 == hover) fl |= CF_UNDER;
            draw_text(x, y, c->text, c->len, c->fsize, col & 0xFFFFFF, fl);
            if (fl & CF_UNDER) {
                int th = c->fsize >= 28 ? 2 : 1;
                fill_argb(x, y + c->fsize * 9 / 10 + 1, c->w, th, col);
            }
            if (fl & CF_STRIKE) fill_argb(x, y + c->fsize * 2 / 5, c->w, 1, col);
            break;
        }
        }
    }
    gfx_set_clip(old.x, old.y, old.w, old.h);
}

int page_link_at(struct page *p, int x, int y)
{
    for (int i = p->ncmds - 1; i >= 0; i--) {
        const struct cmd *c = &p->cmds[i];
        if (!c->link || (c->kind != K_TEXT && c->kind != K_IMAGE)) continue;
        int h = c->kind == K_TEXT ? c->fsize + 3 : c->h;
        int cy = c->kind == K_TEXT ? c->y - 1 : c->y;
        if (x >= c->x && x < c->x + c->w && y >= cy && y < cy + h)
            return c->link - 1;
    }
    return -1;
}

/* The element (or text node) drawn at a page position: the topmost display-list item there. */
struct node *page_node_at(struct page *p, int x, int y)
{
    for (int i = p->ncmds - 1; i >= 0; i--) {
        const struct cmd *c = &p->cmds[i];
        if (!c->node) continue;
        int h = c->kind == K_TEXT ? c->fsize + 3 : c->h;
        int cy = c->kind == K_TEXT ? c->y - 1 : c->y;
        if (x >= c->x && x < c->x + c->w && y >= cy && y < cy + h)
            return c->node;
    }
    return p->body;
}
