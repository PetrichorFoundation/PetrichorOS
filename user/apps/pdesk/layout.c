/* Layout: text metrics, block / inline / float / flex / grid / table flows, producing a display list. */
#include "web.h"
#include "font8x16.h"

static struct page *P;

/* ============================== text metrics ============================== */

static u8 adv_tab[96], left_tab[96];
static int adv_ready;

static void init_adv(void)
{
    for (int ch = 0; ch < 95; ch++) {
        int minc = 8, maxc = -1;
        for (int row = 0; row < 16; row++)
            for (int col = 0; col < 8; col++)
                if (font8x16[ch][row] & (0x80 >> col)) {
                    if (col < minc) minc = col;
                    if (col > maxc) maxc = col;
                }
        if (maxc < 0) {
            adv_tab[ch] = 4;
            left_tab[ch] = 0;
        } else {
            int u = maxc - minc + 3;
            adv_tab[ch] = u < 4 ? 4 : u > 10 ? 10 : u;
            left_tab[ch] = minc - 1 + 8;                    /* biased by 8 so it stays unsigned */
        }
    }
    adv_ready = 1;
}

void glyph_info(int g, int *units, int *left)
{
    if (!adv_ready) init_adv();
    *units = adv_tab[g - 32];
    *left = (int)left_tab[g - 32] - 8;
}

/* Maps a code point to an ASCII glyph (32..126), or -1 for nothing at all. */
int web_glyph(int cp)
{
    if (cp >= 32 && cp < 127) return cp;
    switch (cp) {
    case 0xA0: case 0x2002: case 0x2003: case 0x2009: return ' ';
    case 0xAD: case 0x200B: case 0x200C: case 0x200D: case 0xFEFF: return -1;
    case 0x2018: case 0x2019: case 0x2032: return '\'';
    case 0x201C: case 0x201D: case 0xAB: case 0xBB: return '"';
    case 0x2013: case 0x2014: case 0x2212: case 0x2010: case 0x2011: return '-';
    case 0x2022: case 0xB7: case 0x25CF: return '*';
    case 0x2026: return '.';
    case 0xA9: return 'c';
    case 0xAE: return 'R';
    case 0x2122: return 'T';
    case 0xD7: return 'x';
    case 0x2190: return '<';
    case 0x2192: case 0x203A: return '>';
    case 0xE0: case 0xE1: case 0xE2: case 0xE4: return 'a';
    case 0xE8: case 0xE9: case 0xEA: case 0xEB: return 'e';
    case 0xEC: case 0xED: case 0xEE: return 'i';
    case 0xF2: case 0xF3: case 0xF4: case 0xF6: return 'o';
    case 0xF9: case 0xFA: case 0xFB: case 0xFC: return 'u';
    case 0xE7: return 'c';
    case 0xF1: return 'n';
    case 0xDF: return 's';
    }
    return '?';
}

int utf8_next(const char **ps, const char *e)
{
    const u8 *s = (const u8 *)*ps;
    int cp = *s++, n = 0;
    if (cp >= 0xF0) { cp &= 7; n = 3; }
    else if (cp >= 0xE0) { cp &= 15; n = 2; }
    else if (cp >= 0xC0) { cp &= 31; n = 1; }
    else if (cp >= 0x80) { *ps += 1; return '?'; }
    while (n-- > 0 && (const char *)s < e && (*s & 0xC0) == 0x80)
        cp = cp << 6 | (*s++ & 63);
    *ps = (const char *)s;
    return cp;
}

int glyph_adv(int ch, int px, int flags)
{
    if (!adv_ready) init_adv();
    int u = flags & CF_MONO ? 8 : adv_tab[ch - 32] + (flags & CF_BOLD ? 1 : 0);
    return u * px;
}

int text_width(const char *s, int n, int px, int flags)
{
    const char *e = s + n;
    int sum = 0;
    while (s < e) {
        int g = web_glyph(utf8_next(&s, e));
        if (g > 0)
            sum += glyph_adv(g, px, flags);
    }
    return (sum + 8) / 16;
}

/* ============================== display list ============================== */

static int cmd_new(int kind)
{
    if (P->ncmds == P->cap_cmds) {
        int nc = P->cap_cmds ? P->cap_cmds * 2 : 1024;
        struct cmd *c = realloc(P->cmds, nc * sizeof *c);
        if (!c)
            return -1;
        P->cmds = c;
        P->cap_cmds = nc;
    }
    struct cmd *c = &P->cmds[P->ncmds];
    memset(c, 0, sizeof *c);
    c->kind = kind;
    return P->ncmds++;
}

static void shift_cmds(int from, int to, int dx, int dy)
{
    if (!dx && !dy) return;
    for (int i = from; i < to; i++) {
        P->cmds[i].x += dx;
        P->cmds[i].y += dy;
    }
}

/* Transient storage (case-converted text and merged runs): cleared at the start of every layout. */
static void *lay_alloc(int n)
{
    n = (n + 7) & ~7;
    struct arena *a = P->lay;
    if (!a || a->used + n > a->cap) {
        int cap = n > 60000 ? n : 64000;
        struct arena *na = malloc(sizeof *na + cap);
        if (!na)
            return NULL;
        na->next = a;
        na->used = 0;
        na->cap = cap;
        P->lay = a = na;
    }
    void *r = a->data + a->used;
    a->used += n;
    return r;
}

static void lay_reset(void)
{
    for (struct arena *a = P->lay; a;) {
        struct arena *n = a->next;
        free(a);
        a = n;
    }
    P->lay = NULL;
}

/* ============================== boxes ============================== */

struct box {
    int ml, mr, mt, mb, pl, pr, pt, pb, bl, br, bt, bb;
    int ml_auto, mr_auto, mt_auto, mb_auto;
};

struct lres {
    int h, mb, bg, bgimg, bord[4], cc0, cc1;       /* cc: the content's command range (decor excluded) */
};

#define MAXFLOATS 32
struct bfc {
    struct { int x, y, w, h, side; } fl[MAXFLOATS];
    int n;
};

static int len_px(struct len l, int base, int dflt)
{
    return l.unit == L_PX ? l.v : l.unit == L_PCT ? base * l.v / 1000 : dflt;
}

static void get_box(const struct style *s, int cw, struct box *b)
{
    b->ml = len_px(s->margin[3], cw, 0); b->mr = len_px(s->margin[1], cw, 0);
    b->mt = len_px(s->margin[0], cw, 0); b->mb = len_px(s->margin[2], cw, 0);
    b->ml_auto = s->margin[3].unit == L_AUTO; b->mr_auto = s->margin[1].unit == L_AUTO;
    b->mt_auto = s->margin[0].unit == L_AUTO; b->mb_auto = s->margin[2].unit == L_AUTO;
    b->pt = len_px(s->padding[0], cw, 0); b->pr = len_px(s->padding[1], cw, 0);
    b->pb = len_px(s->padding[2], cw, 0); b->pl = len_px(s->padding[3], cw, 0);
    if (b->pt < 0) b->pt = 0; if (b->pr < 0) b->pr = 0; if (b->pb < 0) b->pb = 0; if (b->pl < 0) b->pl = 0;
    b->bt = s->border_w[0]; b->br = s->border_w[1]; b->bb = s->border_w[2]; b->bl = s->border_w[3];
}

static int line_height(const struct style *s)
{
    if (s->line_height > 0) return s->line_height;
    if (s->line_height < 0) return s->font_size * -s->line_height / 1000;
    return s->font_size * 6 / 5 + 1;
}

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static int blank_text(const struct node *n)
{
    for (int i = 0; i < n->tlen; i++)
        if (!is_ws(n->text[i]) && (u8)n->text[i] != 0xC2)
            return 0;
    return 1;
}

static int is_replaced(const struct node *n)
{
    switch (n->tag) {
    case TG_IMG: case TG_INPUT: case TG_SELECT: case TG_TEXTAREA: case TG_SVG: case TG_CANVAS: case TG_VIDEO:
        return 1;
    }
    return 0;
}

static int is_block_display(int d)
{
    return d == D_BLOCK || d == D_LIST_ITEM || d == D_TABLE || d == D_FLEX || d == D_GRID || d == D_ROW_GROUP ||
           d == D_ROW || d == D_CELL || d == D_CAPTION;
}

static int skipped(const struct node *par, const struct node *c)
{
    if (par->tag == TG_DETAILS && !attr_val(par, "open") && c->type == 0 && c->tag != TG_SUMMARY) return 1;
    if (par->tag == TG_DETAILS && !attr_val(par, "open") && c->type == 1) return 1;
    return 0;
}

static int block_level(struct node *n);

static int has_block_child(struct node *n)
{
    for (struct node *c = n->first; c; c = c->next)
        if (c->type == 0 && c->st && c->st->display != D_NONE && c->st->float_ == FL_NONE && block_level(c))
            return 1;
    return 0;
}

static int block_level(struct node *n)
{
    if (n->type != 0 || !n->st) return 0;
    if (n->bk) return n->bk == 1;
    const struct style *s = n->st;
    int r = is_block_display(s->display) || (s->display == D_INLINE && n->tag != TG_BR && has_block_child(n));
    n->bk = r ? 1 : 2;
    return r;
}

/* ============================== floats ============================== */

static void line_bounds(struct bfc *f, int y, int h, int x, int w, int *lx, int *rx)
{
    *lx = x;
    *rx = x + w;
    if (!f) return;
    for (int i = 0; i < f->n; i++) {
        if (f->fl[i].y >= y + (h > 0 ? h : 1) || f->fl[i].y + f->fl[i].h <= y) continue;
        if (f->fl[i].side == FL_LEFT) { if (f->fl[i].x + f->fl[i].w > *lx) *lx = f->fl[i].x + f->fl[i].w; }
        else if (f->fl[i].x < *rx) *rx = f->fl[i].x;
    }
}

static int float_bottom(struct bfc *f, int side_mask)
{
    int b = 0;
    if (f)
        for (int i = 0; i < f->n; i++)
            if ((side_mask & (f->fl[i].side == FL_LEFT ? 1 : 2)) && f->fl[i].y + f->fl[i].h > b) b = f->fl[i].y + f->fl[i].h;
    return b;
}

static int layout_block(struct node *n, int x, int y, int cw, int force_w, struct bfc *f, struct lres *res);
static int intr_outer(struct node *n, int maxmode);
static int layout_flow(struct node *par, int x, int y0, int w, struct bfc *f, int collapse_top, int *trail_mb);

static int place_float(struct node *n, int y, int cx, int cw, struct bfc *f)
{
    const struct style *s = n->st;
    struct box b;
    get_box(s, cw, &b);
    int fw;
    if (s->width.unit != L_AUTO) {
        fw = len_px(s->width, cw, 0) + (s->box_sizing ? 0 : b.pl + b.pr + b.bl + b.br);
    } else {
        int mx = intr_outer(n, 1) - b.ml - b.mr, mn = intr_outer(n, 0) - b.ml - b.mr;
        fw = mx < cw - b.ml - b.mr ? mx : cw - b.ml - b.mr;
        if (fw < mn) fw = mn;
    }
    struct lres r;
    int c0 = P->ncmds;
    struct bfc own = {0};
    int h = layout_block(n, 0, b.mt, cw, fw, &own, &r) + b.mt + b.mb;
    int ow = fw + b.ml + b.mr;
    int cl = s->clear;
    int yt = y;
    if (cl && f) { int fb = float_bottom(f, cl); if (fb > yt) yt = fb; }
    for (int guard = 0; guard < 64; guard++) {
        int lx, rx;
        line_bounds(f, yt, h, cx, cw, &lx, &rx);
        if (rx - lx >= ow || !f || f->n == 0) break;
        int next = 1 << 30;
        for (int i = 0; i < f->n; i++) {
            int bot = f->fl[i].y + f->fl[i].h;
            if (bot > yt && bot < next) next = bot;
        }
        if (next == 1 << 30) break;
        yt = next;
    }
    int lx, rx;
    line_bounds(f, yt, h, cx, cw, &lx, &rx);
    int px = s->float_ == FL_LEFT ? lx : rx - ow;
    if (px < cx) px = cx;
    shift_cmds(c0, P->ncmds, px, yt);
    if (f && f->n < MAXFLOATS) {
        f->fl[f->n].x = px; f->fl[f->n].y = yt; f->fl[f->n].w = ow; f->fl[f->n].h = h; f->fl[f->n].side = s->float_;
        f->n++;
    }
    return yt + h;
}

/* ============================== intrinsic widths ============================== */

static int image_size(struct node *n, int *w, int *h);

struct mstate {
    int cur, best;
};

static int measure_word(const char *s, int n, const struct style *st)
{
    int fl = (st->bold ? CF_BOLD : 0) | (st->mono ? CF_MONO : 0);
    return text_width(s, n, st->font_size, fl);
}

static void measure_text(struct mstate *m, struct node *t, int maxmode)
{
    const struct style *st = t->st;
    int space = measure_word(" ", 1, st);
    int pre = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP;
    const char *s = t->text, *e = s + t->tlen;
    while (s < e) {
        if (pre) {
            const char *l = s;
            while (l < e && *l != '\n') l++;
            int w = measure_word(s, l - s, st);
            if (maxmode) { m->cur += w; if (m->cur > m->best) m->best = m->cur; if (l < e) m->cur = 0; }
            else if (w > m->best) m->best = w;
            s = l < e ? l + 1 : l;
            continue;
        }
        while (s < e && is_ws(*s)) s++;
        if (s >= e) break;
        const char *w0 = s;
        while (s < e && !is_ws(*s)) s++;
        int w = measure_word(w0, s - w0, st);
        if (maxmode) {
            if (m->cur > 0 && w0 > t->text && is_ws(w0[-1])) m->cur += space;
            m->cur += w;
            if (m->cur > m->best) m->best = m->cur;
        } else if (w > m->best) {
            m->best = w;
        }
    }
    if (maxmode && t->tlen && is_ws(t->text[t->tlen - 1]) && m->cur > 0) m->cur += space;
}

static void measure_inline(struct mstate *m, struct node *c, int maxmode)
{
    if (c->type == 1) { measure_text(m, c, maxmode); return; }
    const struct style *s = c->st;
    if (!s || s->display == D_NONE) return;
    if (c->tag == TG_BR) { if (maxmode) { if (m->cur > m->best) m->best = m->cur; m->cur = 0; } return; }
    if (block_level(c) || s->float_ != FL_NONE || s->display == D_INLINE_BLOCK || s->display == D_INLINE_FLEX || is_replaced(c)) {
        int w = intr_outer(c, maxmode);
        if (maxmode) { m->cur += w; if (m->cur > m->best) m->best = m->cur; }
        else if (w > m->best) m->best = w;
        return;
    }
    struct box b;
    get_box(s, 0, &b);
    int extra = b.ml + b.mr + b.pl + b.pr + b.bl + b.br;
    if (maxmode) m->cur += extra;
    for (struct node *k = c->first; k; k = k->next)
        measure_inline(m, k, maxmode);
}

static int intr_flow(struct node *par, int maxmode)
{
    int best = 0;
    struct mstate m = {0, 0};
    for (struct node *c = par->first; c; c = c->next) {
        if (skipped(par, c)) continue;
        if (c->type == 0 && c->st && c->st->display != D_NONE && (block_level(c) || c->st->float_ != FL_NONE)) {
            if (m.best > best) best = m.best;
            m.cur = m.best = 0;
            int w = intr_outer(c, maxmode);
            if (w > best) best = w;
        } else {
            measure_inline(&m, c, maxmode);
        }
    }
    if (m.best > best) best = m.best;
    return best;
}

static int natural_content(struct node *n, int maxmode)
{
    const struct style *s = n->st;
    int iw, ih;
    switch (n->tag) {
    case TG_IMG:
        if (image_size(n, &iw, &ih)) return iw;
        return 0;
    case TG_INPUT: {
        const char *t = attr_val(n, "type");
        if (t && (!strcmp(t, "checkbox") || !strcmp(t, "radio"))) return 13;
        if (t && !strcmp(t, "hidden")) return 0;
        if (t && (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "reset"))) {
            const char *v = attr_val(n, "value");
            return text_width(v ? v : t, v ? strlen(v) : strlen(t), s->font_size, 0);
        }
        const char *sz = attr_val(n, "size");
        return (sz ? atoi(sz) : 20) * s->font_size / 2;
    }
    case TG_TEXTAREA: { const char *c = attr_val(n, "cols"); return (c ? atoi(c) : 20) * s->font_size / 2; }
    case TG_SELECT: return 120;
    case TG_SVG: case TG_CANVAS: case TG_VIDEO: {
        const char *w = attr_val(n, "width");
        return w ? atoi(w) : n->tag == TG_SVG ? 0 : 300;
    }
    }
    return 0;
}

/* Border-box + margin width of n when sized by its content. */
static int intr_outer(struct node *n, int maxmode)
{
    if (n->type == 1) { struct mstate m = {0, 0}; measure_text(&m, n, maxmode); return m.best; }
    int *cache = maxmode ? &n->maxcw : &n->mincw;
    if (*cache >= 0) return *cache;
    const struct style *s = n->st;
    struct box b;
    get_box(s, 0, &b);
    int extra = b.ml + b.mr + b.pl + b.pr + b.bl + b.br, content;
    if (s->width.unit == L_PX) {
        content = s->width.v - (s->box_sizing ? b.pl + b.pr + b.bl + b.br : 0);
    } else if (is_replaced(n) && n->tag != TG_BUTTON) {
        content = natural_content(n, maxmode);
        if (n->tag == TG_INPUT && !attr_val(n, "type")) content += 0;
        const char *wa = attr_val(n, "width");
        if (wa && (n->tag == TG_IMG)) content = atoi(wa);
    } else if (s->display == D_FLEX || s->display == D_INLINE_FLEX || s->display == D_GRID) {
        int sum = 0, mx = 0, cnt = 0;
        for (struct node *c = n->first; c; c = c->next) {
            if (c->type != 0 || !c->st || c->st->display == D_NONE) continue;
            int w = intr_outer(c, maxmode);
            sum += w;
            if (w > mx) mx = w;
            cnt++;
        }
        int col = s->display == D_FLEX && (s->flex_dir == FD_COL || s->flex_dir == FD_COL_REV);
        content = col || (!maxmode && s->flex_wrap) ? mx : sum + (cnt > 1 ? (cnt - 1) * s->gap_c : 0);
        if (s->display == D_GRID) content = maxmode ? sum : mx;
    } else if (s->display == D_TABLE || s->display == D_ROW_GROUP || s->display == D_ROW) {
        int best = 0;
        for (struct node *r = n->first; r; r = r->next) {
            if (r->type != 0 || !r->st || r->st->display == D_NONE) continue;
            int w = intr_outer(r, maxmode);
            if (s->display == D_ROW) best += w; else if (w > best) best = w;
        }
        content = best;
    } else {
        content = intr_flow(n, maxmode);
    }
    if (s->max_w.unit == L_PX && content > s->max_w.v) content = s->max_w.v;
    if (s->min_w.unit == L_PX && content < s->min_w.v) content = s->min_w.v;
    *cache = content + extra;
    return *cache;
}

/* ============================== images and replaced elements ============================== */

static int image_size(struct node *n, int *w, int *h)
{
    const struct style *s = n->st;
    int aw = 0, ah = 0;
    const char *wa = attr_val(n, "width"), *ha = attr_val(n, "height");
    if (wa) aw = atoi(wa);
    if (ha) ah = atoi(ha);
    int nw = 0, nh = 0;
    struct image *im = n->img ? P->res[n->img - 1].img : NULL;
    if (im) { nw = im->w; nh = im->h; }
    int sw = s->width.unit == L_PX ? s->width.v : -1, sh = s->height.unit == L_PX ? s->height.v : -1;
    int rw = sw >= 0 ? sw : aw, rh = sh >= 0 ? sh : ah;
    if (rw > 0 && rh <= 0) rh = nw ? rw * nh / nw : (ah && aw ? rw * ah / aw : 0);
    else if (rh > 0 && rw <= 0) rw = nh ? rh * nw / nh : (ah && aw ? rh * aw / ah : 0);
    else if (rw <= 0 && rh <= 0) { rw = nw; rh = nh; }
    if (s->max_w.unit == L_PX && rw > s->max_w.v && rw > 0) { rh = rh * s->max_w.v / rw; rw = s->max_w.v; }
    if (rw <= 0 || rh <= 0) {
        if (!im && n->img && P->res[n->img - 1].state == RS_FAILED) return 0;
        if (!im && (aw > 0 || ah > 0)) { *w = aw > 0 ? aw : ah; *h = ah > 0 ? ah : aw; return 1; }
        return 0;
    }
    *w = rw;
    *h = rh;
    return 1;
}

/* ============================== decorations (backgrounds, borders) ============================== */

static int emit_bg(const struct style *s, int x, int y, int w, int h, int link, int *bgimg, struct node *n)
{
    int idx = -1;
    *bgimg = -1;
    if (s->visibility) return -1;
    if (s->bg >> 24) {
        int bgc = s->bg;
        if (s->bg_kind != BG_NONE) {
            idx = cmd_new(K_GRAD);
            if (idx < 0) return -1;
            struct cmd *c = &P->cmds[idx];
            c->color = s->bg; c->color2 = s->bg2; c->flags = s->bg_kind == BG_GRAD_H; c->radius = s->radius;
            c->x = x; c->y = y; c->w = w; c->h = h;
        } else {
            idx = cmd_new(s->radius > 0 ? K_ROUND : K_RECT);
            if (idx < 0) return -1;
            struct cmd *c = &P->cmds[idx];
            c->color = bgc; c->radius = s->radius;
            c->x = x; c->y = y; c->w = w; c->h = h;
        }
    }
    if (s->bg_img && P->res[s->bg_img - 1].state == RS_DONE && P->res[s->bg_img - 1].img) {
        int k = cmd_new(K_IMAGE);
        if (k >= 0) {
            struct cmd *c = &P->cmds[k];
            c->img = s->bg_img;
            c->x = x; c->y = y; c->w = w; c->h = h;
            c->flags = 0x40 | (s->bg_size << 1) | (s->bg_px ? 0x10 : 0);
            c->radius = s->radius;
            *bgimg = k;
        }
    }
    return idx;
}

static void emit_borders(const struct style *s, int x, int y, int w, int h, int idx[4], int skip_l, int skip_r)
{
    for (int i = 0; i < 4; i++) idx[i] = -1;
    if (s->visibility) return;
    for (int i = 0; i < 4; i++) {
        int bw = s->border_w[i];
        if (bw <= 0 || !(s->border_color[i] >> 24)) continue;
        if ((i == 3 && skip_l) || (i == 1 && skip_r)) continue;
        int k = cmd_new(K_RECT);
        if (k < 0) return;
        struct cmd *c = &P->cmds[k];
        c->color = s->border_color[i];
        c->flags = s->border_style[i] == BS_DASHED || s->border_style[i] == BS_DOTTED ? 1 : 0;
        switch (i) {
        case 0: c->x = x; c->y = y; c->w = w; c->h = bw; break;
        case 2: c->x = x; c->y = y + h - bw; c->w = w; c->h = bw; break;
        case 3: c->x = x; c->y = y; c->w = bw; c->h = h; break;
        default: c->x = x + w - bw; c->y = y; c->w = bw; c->h = h; break;
        }
        idx[i] = k;
    }
    /* rounded boxes with a uniform border: paint as a rounded ring */
    if (s->radius > 0 && s->border_w[0] > 0 && s->border_w[0] == s->border_w[1] && s->border_w[0] == s->border_w[2] &&
        s->border_w[0] == s->border_w[3]) {
        for (int i = 0; i < 4; i++) if (idx[i] >= 0) P->cmds[idx[i]].kind = 255;
        int k = cmd_new(K_ROUND);
        if (k >= 0) {
            struct cmd *c = &P->cmds[k];
            c->color = s->border_color[0]; c->radius = s->radius; c->x = x; c->y = y; c->w = w; c->h = h;
            c->flags = 2 | s->border_w[0] << 4;                    /* ring */
            idx[0] = k;
        }
    }
}

/* ============================== inline layout ============================== */

enum { A_WORD, A_SPACE, A_BR, A_BOX, A_OPEN, A_CLOSE };

struct atom {
    u8 kind, decor, nowrap, valign;
    u16 link;
    short dy;
    struct node *n;
    const char *s;
    int len, w, h, asc, c0, c1;
};

struct run {
    struct atom *a;
    int n, cap;
    struct bfc *f;
    int x, w, y;                                   /* container content box and the run's top */
    struct node *owner;
    int y_cur;
};

static struct atom *atom_new(struct run *r, int kind)
{
    if (r->n == r->cap) {
        int nc = r->cap ? r->cap * 2 : 64;
        struct atom *a = realloc(r->a, nc * sizeof *a);
        if (!a) return NULL;
        r->a = a;
        r->cap = nc;
    }
    struct atom *a = &r->a[r->n++];
    memset(a, 0, sizeof *a);
    a->kind = kind;
    return a;
}


/* ---- text into atoms ---- */

static void add_words(struct run *r, struct node *sn, const char *txt, int len, int decor, int dy, int link)
{
    const struct style *st = sn->st;
    int fl = (st->bold ? CF_BOLD : 0) | (st->mono ? CF_MONO : 0);
    int pre = st->white_space == WS_PRE || st->white_space == WS_PRE_WRAP;
    int nowrap = st->white_space == WS_NOWRAP || pre;
    int space_w = text_width(" ", 1, st->font_size, fl);
    const char *s = txt, *e = txt + len;
    struct atom *a;
    if (st->text_transform != TT_NONE) {
        char *t = lay_alloc(len + 1);
        if (t) {
            int cap_next = 1;
            for (int i = 0; i < len; i++) {
                char c = txt[i];
                if (st->text_transform == TT_UPPER) c = c >= 'a' && c <= 'z' ? c - 32 : c;
                else if (st->text_transform == TT_LOWER) c = c >= 'A' && c <= 'Z' ? c + 32 : c;
                else { if (cap_next && c >= 'a' && c <= 'z') c -= 32; cap_next = is_ws(c); }
                t[i] = c;
            }
            t[len] = 0;
            s = t;
            e = t + len;
        }
    }
    while (s < e) {
        if (pre) {
            const char *l = s;
            while (l < e && *l != '\n') l++;
            if (l > s) {
                a = atom_new(r, A_WORD);
                if (!a) return;
                a->n = sn; a->s = s; a->len = l - s; a->w = text_width(s, l - s, st->font_size, fl);
                a->decor = decor; a->link = link; a->dy = dy; a->nowrap = 1;
            }
            if (l < e) {
                a = atom_new(r, A_BR);
                if (!a) return;
                a->n = sn;
            }
            s = l < e ? l + 1 : l;
            continue;
        }
        if (is_ws(*s)) {
            int nl = 0;
            while (s < e && is_ws(*s)) { if (*s == '\n') nl = 1; s++; }
            if (nl && st->white_space == WS_PRE_LINE) {
                a = atom_new(r, A_BR);
                if (a) a->n = sn;
            } else {
                a = atom_new(r, A_SPACE);
                if (!a) return;
                a->n = sn; a->w = space_w; a->link = link; a->decor = decor; a->dy = dy; a->nowrap = nowrap;
            }
            continue;
        }
        const char *w0 = s;
        while (s < e && !is_ws(*s)) s++;
        a = atom_new(r, A_WORD);
        if (!a) return;
        a->n = sn; a->s = w0; a->len = s - w0; a->w = text_width(w0, s - w0, st->font_size, fl);
        a->decor = decor; a->link = link; a->dy = dy; a->nowrap = nowrap;
    }
}

/* ---- replaced elements ---- */

static void text_cmd(int x, int y, const char *s, int len, int px, u32 color, int flags, int w)
{
    int k = cmd_new(K_TEXT);
    if (k < 0) return;
    struct cmd *c = &P->cmds[k];
    c->x = x; c->y = y; c->w = w; c->h = px; c->text = s; c->len = len; c->fsize = px; c->color = color; c->flags = flags;
}

/* Border-box size of a replaced element or inline-block control. */
static void replaced_size(struct node *n, int *w, int *h)
{
    const struct style *s = n->st;
    struct box b;
    get_box(s, 0, &b);
    int ex = b.pl + b.pr + b.bl + b.br, ey = b.pt + b.pb + b.bt + b.bb;
    if (n->tag == TG_IMG) {
        int iw = 0, ih = 0;
        if (!image_size(n, &iw, &ih)) { *w = *h = 0; return; }
        *w = iw; *h = ih;
        return;
    }
    int cw = natural_content(n, 1);
    int lh = line_height(s);
    int ww = s->width.unit == L_PX ? s->width.v + (s->box_sizing ? 0 : 0) : cw + ex;
    int hh = s->height.unit == L_PX ? s->height.v : 0;
    if (n->tag == TG_INPUT) {
        const char *t = attr_val(n, "type");
        if (t && (!strcmp(t, "checkbox") || !strcmp(t, "radio"))) { *w = 13; *h = 13; return; }
        if (!hh) hh = lh + ey;
    } else if (n->tag == TG_TEXTAREA) {
        const char *rs = attr_val(n, "rows");
        if (!hh) hh = (rs ? atoi(rs) : 2) * lh + ey;
    } else if (n->tag == TG_SELECT) {
        ww = s->width.unit == L_PX ? s->width.v : 130;
        if (!hh) hh = lh + ey;
    } else if (n->tag == TG_SVG || n->tag == TG_CANVAS || n->tag == TG_VIDEO) {
        const char *ha = attr_val(n, "height");
        if (!hh) hh = ha ? atoi(ha) : n->tag == TG_SVG ? 0 : 150;
        if (s->width.unit != L_PX && !attr_val(n, "width") && n->tag == TG_SVG) ww = 0;
    }
    *w = ww;
    *h = hh;
}

static void emit_replaced(struct node *n, int x, int y, int w, int h)
{
    const struct style *s = n->st;
    int bgimg;
    if (n->tag == TG_IMG) {
        struct image *im = n->img ? P->res[n->img - 1].img : NULL;
        if (im && !s->visibility) {
            int k = cmd_new(K_IMAGE);
            if (k >= 0) {
                struct cmd *c = &P->cmds[k];
                c->img = n->img; c->x = x; c->y = y; c->w = w; c->h = h; c->link = n->link; c->radius = s->radius;
            }
        } else if (!s->visibility && w > 0 && h > 0) {
            int k = cmd_new(K_RECT);
            if (k >= 0) { P->cmds[k].color = 0xFFEEEEEE; P->cmds[k].x = x; P->cmds[k].y = y; P->cmds[k].w = w; P->cmds[k].h = h; }
        }
        return;
    }
    int bord[4];
    emit_bg(s, x, y, w, h, 0, &bgimg, n);
    emit_borders(s, x, y, w, h, bord, 0, 0);
    struct box b;
    get_box(s, 0, &b);
    int px = s->font_size, lh = line_height(s);
    int tx = x + b.bl + b.pl, ty = y + b.bt + b.pt + (lh - px) / 2;
    if (n->tag == TG_INPUT) {
        const char *t = attr_val(n, "type");
        if (t && (!strcmp(t, "checkbox") || !strcmp(t, "radio"))) {
            int radio = !strcmp(t, "radio"), on = attr_val(n, "checked") != NULL;
            int k = cmd_new(radio ? K_ROUND : K_RECT);
            if (k >= 0) { struct cmd *c = &P->cmds[k]; c->color = 0xFF767676; c->x = x; c->y = y; c->w = 13; c->h = 13; c->radius = radio ? 7 : 2; }
            k = cmd_new(radio ? K_ROUND : K_RECT);
            if (k >= 0) { struct cmd *c = &P->cmds[k]; c->color = on ? 0xFF0075FF : 0xFFFFFFFF; c->x = x + 1; c->y = y + 1; c->w = 11; c->h = 11; c->radius = radio ? 6 : 1; }
            if (on && !radio) text_cmd(x + 2, y - 1, "v", 1, 13, 0xFFFFFFFF, CF_BOLD, 9);
            return;
        }
        const char *val = attr_val(n, "value");
        const char *ph = attr_val(n, "placeholder");
        int pw = t && !strcmp(t, "password");
        const char *shown = val && *val ? val : (t && (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "reset")) ? (val ? val : t) : ph);
        u32 col = val && *val ? s->color : 0xFF757575;
        if (t && (!strcmp(t, "submit") || !strcmp(t, "button") || !strcmp(t, "reset"))) {
            if (!shown) shown = !strcmp(t, "reset") ? "Reset" : "Submit";
            col = s->color;
            int len = strlen(shown), tw = text_width(shown, len, px, 0);
            text_cmd(x + (w - tw) / 2, ty, shown, len, px, col, 0, tw);
            return;
        }
        if (shown && *shown) {
            int len = strlen(shown);
            if (pw && val && *val) {
                char *stars = lay_alloc(len + 1);
                if (stars) { memset(stars, '*', len); stars[len] = 0; shown = stars; }
            }
            int maxc = (w - b.bl - b.br - b.pl - b.pr) * 16 / (px * 8 > 0 ? px * 8 : 1);
            if (len > maxc && maxc > 0) len = maxc;
            text_cmd(tx, ty, shown, len, px, col, 0, text_width(shown, len, px, 0));
        }
    } else if (n->tag == TG_SELECT) {
        const char *txt = "";
        int len = 0;
        for (struct node *o = n->first; o; o = o->next)
            if (o->type == 0 && o->tag == TG_OPTION && o->first && o->first->type == 1) {
                if (!len || attr_val(o, "selected")) { txt = o->first->text; len = o->first->tlen; }
            }
        if (len > 16) len = 16;
        text_cmd(tx, ty, txt, len, px, s->color, 0, text_width(txt, len, px, 0));
        text_cmd(x + w - 16, ty, "v", 1, px, 0xFF555555, 0, 8);
    } else if (n->tag == TG_TEXTAREA) {
        if (n->first && n->first->type == 1) {
            const char *t = n->first->text;
            int len = n->first->tlen, maxc = (w - 8) * 2 / (px > 0 ? px : 1), ln = 0;
            for (const char *p = t; p < t + len && ln < 4; ) {
                const char *e = p;
                while (e < t + len && *e != '\n') e++;
                int l = e - p > maxc ? maxc : e - p;
                text_cmd(tx, ty + ln * lh, p, l, px, s->color, 0, text_width(p, l, px, 0));
                ln++;
                p = e + 1;
            }
        }
    } else if (n->tag == TG_CANVAS || n->tag == TG_VIDEO) {
        int k = cmd_new(K_RECT);
        if (k >= 0) { P->cmds[k].color = n->tag == TG_VIDEO ? 0xFF000000 : 0xFFF4F4F4; P->cmds[k].x = x; P->cmds[k].y = y; P->cmds[k].w = w; P->cmds[k].h = h; }
    }
}

/* ---- the inline run ---- */

static void collect(struct run *r, struct node *c, int decor, int dy)
{
    if (c->type == 1) {
        add_words(r, c, c->text, c->tlen, decor, dy, c->link);
        return;
    }
    const struct style *s = c->st;
    if (!s || s->display == D_NONE) return;
    if (c->tag == TG_BR) {
        struct atom *a = atom_new(r, A_BR);
        if (a) a->n = c;
        return;
    }
    if (s->float_ != FL_NONE) {
        place_float(c, r->y, r->x, r->w, r->f);
        return;
    }
    int boxy = is_replaced(c) || s->display == D_INLINE_BLOCK || s->display == D_INLINE_FLEX;
    if (boxy) {
        struct atom *a = atom_new(r, A_BOX);
        if (!a) return;
        struct box b;
        get_box(s, r->w, &b);
        a->n = c; a->link = c->link; a->nowrap = 0; a->valign = s->vertical_align;
        int c0 = P->ncmds;
        int w, h;
        if (is_replaced(c)) {
            replaced_size(c, &w, &h);
            if (c->tag == TG_IMG && (w <= 0 || h <= 0)) {                    /* not available: show the alt text */
                r->n--;
                const char *alt = attr_val(c, "alt");
                if (alt && *alt && c->img && P->res[c->img - 1].state == RS_FAILED)
                    add_words(r, c, alt, strlen(alt), decor, dy, c->link);
                return;
            }
            int maxw = r->w - b.ml - b.mr;
            if (c->tag == TG_IMG && w > maxw && maxw > 0) { h = h * maxw / w; w = maxw; }
            emit_replaced(c, b.ml, b.mt, w, h);
            a->w = w + b.ml + b.mr;
            a->h = h + b.mt + b.mb;
        } else {
            int fw;
            if (s->width.unit != L_AUTO) fw = len_px(s->width, r->w, 0) + (s->box_sizing ? 0 : b.pl + b.pr + b.bl + b.br);
            else {
                int mx = intr_outer(c, 1) - b.ml - b.mr, mn = intr_outer(c, 0) - b.ml - b.mr;
                int av = r->w - b.ml - b.mr;
                fw = mx < av ? mx : av;
                if (fw < mn) fw = mn;
            }
            struct lres lr;
            int hh = layout_block(c, 0, b.mt, r->w, fw, NULL, &lr);
            a->w = fw + b.ml + b.mr;
            a->h = hh + b.mt + b.mb;
        }
        a->c0 = c0;
        a->c1 = P->ncmds;
        a->asc = a->h;
        if (s->vertical_align == VA_MIDDLE) a->asc = a->h / 2 + s->font_size / 4;
        return;
    }
    /* an ordinary inline element */
    int ndecor = decor | s->decor;
    int ndy = dy;
    if (c->tag == TG_SUP) ndy -= s->font_size / 2 + 2;
    else if (c->tag == TG_SUB) ndy += s->font_size / 4;
    struct box b;
    get_box(s, r->w, &b);
    struct atom *a = atom_new(r, A_OPEN);
    if (a) { a->n = c; a->w = b.ml + b.bl + b.pl; a->dy = ndy; }
    for (struct node *k = c->first; k; k = k->next)
        collect(r, k, ndecor, ndy);
    a = atom_new(r, A_CLOSE);
    if (a) { a->n = c; a->w = b.mr + b.br + b.pr; a->dy = ndy; }
}

struct oline {
    struct node *open[24];
    int nopen;
    int xs[24];
    int first[24];
};

static void frag(struct node *n, int x0, int x1, int base, int first, int last)
{
    const struct style *s = n->st;
    int has_bord = s->border_w[0] || s->border_w[1] || s->border_w[2] || s->border_w[3];
    if (!(s->bg >> 24) && !has_bord) return;
    if (x1 <= x0) return;
    int fs = s->font_size;
    struct box b;
    get_box(s, 0, &b);
    int top = base - fs * 4 / 5 - b.pt - b.bt, h = fs * 6 / 5 + b.pt + b.pb + b.bt + b.bb;
    int bgimg, bord[4];
    emit_bg(s, x0, top, x1 - x0, h, 0, &bgimg, n);
    emit_borders(s, x0, top, x1 - x0, h, bord, !first, !last);
}

/* Lays out atoms [i, j) on one line; returns its height. */
static int flush_line(struct run *r, struct oline *ol, int i, int j, int y, int lx, int avail, int used, int align, int last_line,
                      int *strut_lh, struct node *owner)
{
    int lastc = -1;
    for (int k = i; k < j; k++)
        if (r->a[k].kind == A_WORD || r->a[k].kind == A_BOX) lastc = k;
    int asc = 0, desc = 0;
    for (int k = i; k < j; k++) {
        struct atom *a = &r->a[k];
        if (a->kind == A_WORD) {
            int lh = line_height(a->n->st), fs = a->n->st->font_size;
            int as = (lh - fs) / 2 + fs * 4 / 5 - a->dy, ds = lh - ((lh - fs) / 2 + fs * 4 / 5) + a->dy;
            if (as > asc) asc = as;
            if (ds > desc) desc = ds;
        } else if (a->kind == A_BOX) {
            if (a->asc > asc) asc = a->asc;
            if (a->h - a->asc > desc) desc = a->h - a->asc;
        }
    }
    int slh = *strut_lh, sfs = owner->st->font_size;
    int sa = (slh - sfs) / 2 + sfs * 4 / 5, sd = slh - sa;
    if (sa > asc) asc = sa;
    if (sd > desc) desc = sd;
    int lh = asc + desc, base = y + asc;
    int off = 0;
    if (align == TA_CENTER) off = (avail - used) / 2;
    else if (align == TA_RIGHT) off = avail - used;
    if (off < 0) off = 0;

    int *xs = malloc((j - i + 1) * sizeof(int));
    if (!xs) return lh;
    int x = lx + off;
    for (int k = i; k < j; k++) {
        struct atom *a = &r->a[k];
        xs[k - i] = x;
        if (a->kind == A_SPACE) { if (k < lastc) x += a->w; }
        else if (a->kind != A_BR) x += a->w;
    }
    xs[j - i] = x;

    /* inline backgrounds and borders first */
    struct oline cur = *ol;
    for (int q = 0; q < cur.nopen; q++) { cur.xs[q] = lx + off; cur.first[q] = 0; }
    for (int k = i; k < j; k++) {
        struct atom *a = &r->a[k];
        if (a->kind == A_OPEN && cur.nopen < 24) {
            struct box b;
            get_box(a->n->st, 0, &b);
            cur.open[cur.nopen] = a->n; cur.xs[cur.nopen] = xs[k - i] + b.ml; cur.first[cur.nopen] = 1; cur.nopen++;
        } else if (a->kind == A_CLOSE) {
            for (int q = cur.nopen - 1; q >= 0; q--)
                if (cur.open[q] == a->n) {
                    struct box b;
                    get_box(a->n->st, 0, &b);
                    frag(a->n, cur.xs[q], xs[k + 1 - i] - b.mr, base - a->dy, cur.first[q], 1);
                    for (int z = q; z + 1 < cur.nopen; z++) { cur.open[z] = cur.open[z + 1]; cur.xs[z] = cur.xs[z + 1]; cur.first[z] = cur.first[z + 1]; }
                    cur.nopen--;
                    break;
                }
        }
    }
    for (int q = 0; q < cur.nopen; q++)
        frag(cur.open[q], cur.xs[q], xs[j - i], base, cur.first[q], 0);
    ol->nopen = cur.nopen;
    for (int q = 0; q < cur.nopen; q++) { ol->open[q] = cur.open[q]; }

    /* contents */
    int k = i;
    while (k < j) {
        struct atom *a = &r->a[k];
        if (a->kind == A_BOX) {
            int bx = xs[k - i], by = base - a->asc;
            if (a->valign == VA_TOP) by = y;
            else if (a->valign == VA_BOTTOM) by = y + lh - a->h;
            shift_cmds(a->c0, a->c1, bx, by);
            if (a->link && a->c1 > a->c0)
                for (int z = a->c0; z < a->c1; z++) if (!P->cmds[z].link) P->cmds[z].link = a->link;
            k++;
            continue;
        }
        if (a->kind != A_WORD && a->kind != A_SPACE) { k++; continue; }
        /* merge a stretch of words/spaces from one text node */
        int e = k, total = 0, tw = 0;
        struct node *sn = a->n;
        while (e < j && (r->a[e].kind == A_WORD || r->a[e].kind == A_SPACE) && r->a[e].n == sn && r->a[e].dy == a->dy &&
               r->a[e].link == a->link && r->a[e].decor == a->decor) {
            e++;
        }
        int lastw = e - 1;
        while (lastw >= k && r->a[lastw].kind == A_SPACE) lastw--;
        if (lastw < k) { k = e; continue; }
        for (int z = k; z <= lastw; z++) {
            total += r->a[z].kind == A_SPACE ? 1 : r->a[z].len;
            tw += r->a[z].kind == A_SPACE ? r->a[z].w : r->a[z].w;
        }
        char *buf = lay_alloc(total + 1);
        if (buf) {
            int o = 0;
            for (int z = k; z <= lastw; z++) {
                if (r->a[z].kind == A_SPACE) buf[o++] = ' ';
                else { memcpy(buf + o, r->a[z].s, r->a[z].len); o += r->a[z].len; }
            }
            buf[o] = 0;
            const struct style *st = sn->st;
            int fl = (st->bold ? CF_BOLD : 0) | (st->italic ? CF_ITALIC : 0) | (st->mono ? CF_MONO : 0) |
                     (a->decor & DEC_UNDERLINE ? CF_UNDER : 0) | (a->decor & DEC_STRIKE ? CF_STRIKE : 0);
            if (!st->visibility && (st->color >> 24)) {
                int ci = cmd_new(K_TEXT);
                if (ci >= 0) {
                    struct cmd *c = &P->cmds[ci];
                    c->x = xs[k - i]; c->y = base - st->font_size * 4 / 5 + a->dy; c->w = xs[lastw + 1 - i] - xs[k - i];
                    c->h = st->font_size; c->fsize = st->font_size; c->text = buf; c->len = o; c->color = st->color;
                    c->flags = fl; c->link = a->link;
                    if (c->w < tw) c->w = tw;
                }
            }
        }
        k = e;
    }
    free(xs);
    return lh;
}

static int layout_run(struct node *first, struct node *end, struct node *owner, int x, int y, int w, struct bfc *f)
{
    struct run r = {0};
    r.f = f; r.x = x; r.w = w; r.y = y; r.owner = owner;
    for (struct node *c = first; c != end; c = c->next)
        if (!skipped(owner, c)) collect(&r, c, 0, 0);
    int content = 0;
    for (int k = 0; k < r.n; k++)
        if (r.a[k].kind == A_WORD || r.a[k].kind == A_BOX || r.a[k].kind == A_BR) content = 1;
    if (!content) { free(r.a); return 0; }

    const struct style *os = owner->st;
    int slh = line_height(os);
    struct oline ol = {0};
    int yy = y, i = 0;
    while (i < r.n) {
        while (i < r.n && r.a[i].kind == A_SPACE) i++;
        if (i >= r.n) break;
        int lx, rx;
        line_bounds(f, yy, slh, x, w, &lx, &rx);
        for (int g = 0; g < 32 && rx - lx < 20 && f && f->n; g++) {                 /* no room beside the floats: drop below them */
            int next = 1 << 30;
            for (int q = 0; q < f->n; q++) { int bot = f->fl[q].y + f->fl[q].h; if (bot > yy && bot < next) next = bot; }
            if (next == 1 << 30) break;
            yy = next;
            line_bounds(f, yy, slh, x, w, &lx, &rx);
        }
        int avail = rx - lx;
        int j = i, cur = 0, pend = 0, has = 0, forced = 0;
        while (j < r.n) {
            struct atom *a = &r.a[j];
            if (a->kind == A_BR) { j++; forced = 1; break; }
            if (a->kind == A_SPACE) { if (has) pend = a->w; j++; continue; }
            if (a->kind == A_OPEN || a->kind == A_CLOSE) { cur += a->w; j++; continue; }
            int need = (has ? pend : 0) + a->w;
            if (has && cur + need > avail && !a->nowrap) break;
            if (!has && a->kind == A_WORD && a->w > avail && avail > 8 && !a->nowrap) {       /* a word wider than the line: split it */
                int fl = (a->n->st->bold ? CF_BOLD : 0) | (a->n->st->mono ? CF_MONO : 0);
                const char *s = a->s, *e = a->s + a->len;
                const char *cut = s;
                int wsum = 0;
                while (cut < e) {
                    const char *nx = cut;
                    int cp = utf8_next(&nx, e);
                    int g = web_glyph(cp);
                    int cw = g > 0 ? (glyph_adv(g, a->n->st->font_size, fl) + 8) / 16 : 0;
                    if (wsum + cw > avail && cut > s) break;
                    wsum += cw;
                    cut = nx;
                }
                if (cut < e) {
                    if (r.n == r.cap) {
                        int nc = r.cap * 2;
                        struct atom *na = realloc(r.a, nc * sizeof *na);
                        if (!na) break;
                        r.a = na; r.cap = nc; a = &r.a[j];
                    }
                    memmove(&r.a[j + 2], &r.a[j + 1], (r.n - j - 1) * sizeof *a);
                    r.n++;
                    struct atom *rest = &r.a[j + 1];
                    *rest = *a;
                    rest->s = cut; rest->len = e - cut; rest->w = text_width(cut, e - cut, a->n->st->font_size, fl);
                    a->len = cut - s; a->w = wsum;
                }
            }
            cur += need;
            has = 1;
            pend = 0;
            j++;
        }
        if (j == i) j++;                                                          /* always make progress */
        int align = os->text_align;
        if (align == TA_JUSTIFY) align = TA_LEFT;
        int lh = flush_line(&r, &ol, i, j, yy, lx, avail, cur, align, j >= r.n, &slh, owner);
        yy += lh;
        (void)forced;
        i = j;
    }
    free(r.a);
    return yy - y;
}

/* ============================== block flow ============================== */

static int collapse_m(int a, int b)
{
    if (a >= 0 && b >= 0) return a > b ? a : b;
    if (a < 0 && b < 0) return a < b ? a : b;
    return a + b;
}

static int is_bfc_root(struct node *n)
{
    const struct style *s = n->st;
    return s->float_ != FL_NONE || s->overflow != OV_VISIBLE || s->display == D_INLINE_BLOCK || s->display == D_FLEX ||
           s->display == D_INLINE_FLEX || s->display == D_GRID || s->display == D_TABLE || s->display == D_CELL ||
           s->display == D_ROW;
}

static int eff_mt(struct node *n, int cw)
{
    struct box b;
    get_box(n->st, cw, &b);
    int mt = b.mt;
    if (!is_bfc_root(n) && b.bt == 0 && b.pt == 0) {
        for (struct node *c = n->first; c; c = c->next) {
            if (skipped(n, c)) continue;
            if (c->type == 1) { if (blank_text(c)) continue; break; }
            if (!c->st || c->st->display == D_NONE || c->st->float_ != FL_NONE) continue;
            if (block_level(c)) return collapse_m(mt, eff_mt(c, cw));
            break;
        }
    }
    return mt;
}

static int layout_flow(struct node *par, int x, int y0, int w, struct bfc *f, int collapse_top, int *trail_mb)
{
    int y = y0, pend = 0, first = 1, only_floats = 1, any_float = 0, fb_local = 0;
    for (struct node *c = par->first; c;) {
        if (skipped(par, c)) { c = c->next; continue; }
        if (c->type == 1 || (c->st && c->st->display != D_NONE && c->st->float_ == FL_NONE && !block_level(c))) {
            struct node *e = c;
            while (e && (skipped(par, e) || e->type == 1 || (e->st && (e->st->display == D_NONE || e->st->float_ != FL_NONE || !block_level(e)))))
                e = e->next;
            int h = layout_run(c, e, par, x, y + pend, w, f);
            if (h > 0) { y += pend + h; pend = 0; first = 0; only_floats = 0; }
            c = e;
            continue;
        }
        if (c->type != 0 || !c->st || c->st->display == D_NONE) { c = c->next; continue; }
        if (c->st->float_ != FL_NONE) {
            int b = place_float(c, y + pend, x, w, f);
            if (b > fb_local) fb_local = b;
            any_float = 1;
            c = c->next;
            continue;
        }
        only_floats = 0;
        if (c->st->clear && f) {
            int fb = float_bottom(f, c->st->clear);
            if (fb > y + pend) { y = fb; pend = 0; }
        }
        int gap = first && collapse_top ? 0 : collapse_m(pend, eff_mt(c, w));
        y += gap;
        struct lres r;
        int h = layout_block(c, x, y, w, -1, f, &r);
        y += h;
        pend = r.mb;
        first = 0;
        c = c->next;
    }
    if (any_float && only_floats && fb_local > y) y = fb_local;
    *trail_mb = pend;
    return y - y0;
}

static int list_index(struct node *n)
{
    int idx = 1;
    struct node *p = n->parent;
    if (p && p->tag == TG_OL) {
        const char *st = attr_val(p, "start");
        if (st) idx = atoi(st);
    }
    for (struct node *s = n->prev; s; s = s->prev)
        if (s->type == 0 && s->st && s->st->display == D_LIST_ITEM) idx++;
    const char *v = attr_val(n, "value");
    if (v) idx = atoi(v);
    return idx;
}

static void emit_marker(struct node *n, int cx, int cy)
{
    const struct style *s = n->st;
    if (s->list_style == LS_NONE || s->visibility) return;
    int fs = s->font_size, lh = line_height(s);
    int mid = cy + lh / 2;
    int d = fs / 3 < 4 ? 4 : fs / 3;
    if (s->list_style == LS_DISC || s->list_style == LS_CIRCLE) {
        int k = cmd_new(K_ROUND);
        if (k < 0) return;
        struct cmd *c = &P->cmds[k];
        c->x = cx - fs - d / 2 + 2; c->y = mid - d / 2; c->w = d; c->h = d; c->radius = d; c->color = s->color;
        if (s->list_style == LS_CIRCLE) c->flags = 2 | 1 << 4;
    } else if (s->list_style == LS_SQUARE) {
        int k = cmd_new(K_RECT);
        if (k < 0) return;
        struct cmd *c = &P->cmds[k];
        c->x = cx - fs - d / 2 + 2; c->y = mid - d / 2; c->w = d; c->h = d; c->color = s->color;
    } else {
        int idx = list_index(n);
        char *buf = lay_alloc(16);
        if (!buf) return;
        int len;
        if (s->list_style == LS_ALPHA) { buf[0] = 'a' + (idx - 1) % 26; buf[1] = '.'; buf[2] = 0; len = 2; }
        else len = snprintf(buf, 16, "%d.", idx);
        int fl = (s->bold ? CF_BOLD : 0);
        int tw = text_width(buf, len, fs, fl);
        text_cmd(cx - tw - 6, cy + (lh - fs) / 2, buf, len, fs, s->color, fl, tw);
    }
}

/* ---- flex ---- */

struct fitem {
    struct node *n;
    struct box b;
    int hyp, minc, w, h, order, c0, c1, xpos, grow, shrink;
    struct lres r;
};

static void set_box_height(struct lres *r, int newh)
{
    int d = newh - r->h;
    if (d <= 0) return;
    if (r->bg >= 0) P->cmds[r->bg].h += d;
    if (r->bgimg >= 0) P->cmds[r->bgimg].h += d;
    for (int i = 0; i < 4; i++) {
        int k = r->bord[i];
        if (k < 0) continue;
        struct cmd *c = &P->cmds[k];
        if (c->kind == K_ROUND) c->h += d;
        else if (i == 2) c->y += d;
        else if (i == 1 || i == 3) c->h += d;
    }
    r->h = newh;
}

static int item_basis(struct node *n, struct box *b, int cw, int *minc)
{
    const struct style *s = n->st;
    int extra = b->pl + b->pr + b->bl + b->br;
    int bb;
    if (s->flex_basis.unit != L_AUTO) bb = len_px(s->flex_basis, cw, 0) + (s->box_sizing ? 0 : extra);
    else if (s->width.unit != L_AUTO) bb = len_px(s->width, cw, 0) + (s->box_sizing ? 0 : extra);
    else bb = intr_outer(n, 1) - b->ml - b->mr;
    if (bb < extra) bb = extra;
    if (s->max_w.unit != L_AUTO) { int m = len_px(s->max_w, cw, 1 << 20) + (s->box_sizing ? 0 : extra); if (bb > m) bb = m; }
    if (s->min_w.unit != L_AUTO) { int m = len_px(s->min_w, cw, 0) + (s->box_sizing ? 0 : extra); if (bb < m) bb = m; }
    *minc = s->width.unit == L_AUTO && s->overflow == OV_VISIBLE ? intr_outer(n, 0) - b->ml - b->mr : extra;
    if (s->min_w.unit != L_AUTO) { int m = len_px(s->min_w, cw, 0) + (s->box_sizing ? 0 : extra); if (*minc < m) *minc = m; }
    return bb;
}

static int layout_flex(struct node *n, int cx, int cy, int cw, int def_h)
{
    const struct style *s = n->st;
    struct fitem items[64];
    int ni = 0;
    for (struct node *c = n->first; c && ni < 64; c = c->next) {
        if (c->type != 0 || !c->st || c->st->display == D_NONE) continue;
        items[ni].n = c;
        items[ni].order = c->st->order;
        ni++;
    }
    if (!ni) return 0;
    for (int i = 1; i < ni; i++) {                                           /* stable sort by order */
        struct fitem t = items[i];
        int j = i - 1;
        while (j >= 0 && items[j].order > t.order) { items[j + 1] = items[j]; j--; }
        items[j + 1] = t;
    }
    int col = s->flex_dir == FD_COL || s->flex_dir == FD_COL_REV;
    int rev = s->flex_dir == FD_ROW_REV || s->flex_dir == FD_COL_REV;
    int gapm = col ? s->gap_r : s->gap_c, gapc = col ? s->gap_c : s->gap_r;
    int total_h = 0;

    if (col) {
        int y = cy;
        for (int i = 0; i < ni; i++) {
            struct fitem *it = &items[rev ? ni - 1 - i : i];
            const struct style *is = it->n->st;
            get_box(is, cw, &it->b);
            int align = is->align_self != AI_AUTO ? is->align_self : s->align_items;
            int fw;
            int extra = it->b.pl + it->b.pr + it->b.bl + it->b.br;
            if (is->width.unit != L_AUTO) fw = len_px(is->width, cw, 0) + (is->box_sizing ? 0 : extra);
            else if (align == AI_STRETCH) fw = cw - it->b.ml - it->b.mr;
            else { fw = intr_outer(it->n, 1) - it->b.ml - it->b.mr; if (fw > cw - it->b.ml - it->b.mr) fw = cw - it->b.ml - it->b.mr; }
            int c0 = P->ncmds;
            int h = layout_block(it->n, 0, it->b.mt, cw, fw, NULL, &it->r);
            int free = cw - fw - it->b.ml - it->b.mr, dx = 0;
            if (it->b.ml_auto && it->b.mr_auto) dx = free / 2; else if (it->b.ml_auto) dx = free;
            else if (align == AI_CENTER) dx = free / 2; else if (align == AI_END) dx = free;
            shift_cmds(c0, P->ncmds, cx + dx, y);
            y += h + it->b.mt + it->b.mb + gapm;
        }
        total_h = y - gapm - cy;
        return total_h;
    }

    /* row: size the items */
    for (int i = 0; i < ni; i++) {
        struct fitem *it = &items[i];
        const struct style *is = it->n->st;
        get_box(is, cw, &it->b);
        it->hyp = item_basis(it->n, &it->b, cw, &it->minc);
        it->grow = is->grow;
        it->shrink = is->shrink;
    }
    int y = cy, i0 = 0;
    while (i0 < ni) {
        int i1 = i0, sum = 0;
        while (i1 < ni) {
            int o = items[i1].hyp + items[i1].b.ml + items[i1].b.mr;
            if (s->flex_wrap && i1 > i0 && sum + gapm + o > cw) break;
            sum += (i1 > i0 ? gapm : 0) + o;
            i1++;
        }
        int cnt = i1 - i0;
        int free = cw - sum;
        if (free > 0) {
            int tg = 0;
            for (int i = i0; i < i1; i++) tg += items[i].grow;
            if (tg > 0)
                for (int i = i0; i < i1; i++) items[i].hyp += (int)((long)free * items[i].grow / tg);
        } else if (free < 0) {
            long ts = 0;
            for (int i = i0; i < i1; i++) ts += (long)items[i].shrink * items[i].hyp;
            if (ts > 0)
                for (int i = i0; i < i1; i++) {
                    int cut = (int)((long)(-free) * items[i].shrink * items[i].hyp / ts);
                    items[i].hyp -= cut;
                    if (items[i].hyp < items[i].minc) items[i].hyp = items[i].minc;
                }
        }
        int line_h = 0;
        for (int i = i0; i < i1; i++) {                                      /* lay each item out at the origin */
            struct fitem *it = &items[i];
            it->w = it->hyp;
            it->c0 = P->ncmds;
            int h = layout_block(it->n, 0, it->b.mt, cw, it->w, NULL, &it->r);
            it->c1 = P->ncmds;
            it->h = h + it->b.mt + it->b.mb;
            if (it->h > line_h) line_h = it->h;
        }
        if (def_h >= 0 && !s->flex_wrap && i0 == 0 && i1 == ni) line_h = def_h;
        int used = 0, autos = 0;
        for (int i = i0; i < i1; i++) {
            used += items[i].w + items[i].b.ml + items[i].b.mr;
            autos += items[i].b.ml_auto + items[i].b.mr_auto;
        }
        used += (cnt - 1) * gapm;
        int left = cw - used, startx = 0, extra = 0, per_auto = 0;
        if (left > 0) {
            if (autos) per_auto = left / autos;
            else switch (s->justify) {
                case JC_END: startx = left; break;
                case JC_CENTER: startx = left / 2; break;
                case JC_BETWEEN:
                    if (cnt > 1)
                        extra = left / (cnt - 1);
                    break;
                case JC_AROUND: extra = left / cnt; startx = extra / 2; break;
                case JC_EVENLY: extra = left / (cnt + 1); startx = extra; break;
                }
        }
        int x = startx;
        for (int q = 0; q < cnt; q++) {
            struct fitem *it = &items[rev ? i1 - 1 - q : i0 + q];
            const struct style *is = it->n->st;
            int xl = x + (it->b.ml_auto ? per_auto : 0);
            int align = is->align_self != AI_AUTO ? is->align_self : s->align_items;
            int dy = 0;
            if (it->b.mt_auto && it->b.mb_auto) dy = (line_h - it->h) / 2;
            else if (it->b.mt_auto) dy = line_h - it->h;
            else if (align == AI_CENTER) dy = (line_h - it->h) / 2;
            else if (align == AI_END) dy = line_h - it->h;
            else if (align == AI_STRETCH && is->height.unit == L_AUTO) {
                int want = line_h - it->b.mt - it->b.mb;
                if (want > it->r.h) set_box_height(&it->r, want);
            }
            shift_cmds(it->c0, it->c1, cx + xl, y + dy);
            x = xl + it->w + it->b.ml + it->b.mr + (it->b.mr_auto ? per_auto : 0) + gapm + extra;
        }
        y += line_h + gapc;
        i0 = i1;
    }
    return y - gapc - cy;
}

/* ---- grid ---- */

static int layout_grid(struct node *n, int cx, int cy, int cw)
{
    const struct style *s = n->st;
    int ncols, colw[12];
    int gap = s->gap_c;
    if (s->grid_cols < 0) {
        int mn = s->grid_col[0].v > 0 ? s->grid_col[0].v : 100;
        ncols = (cw + gap) / (mn + gap);
        if (ncols < 1) ncols = 1;
        if (ncols > 12) ncols = 12;
        for (int i = 0; i < ncols; i++) colw[i] = (cw - gap * (ncols - 1)) / ncols;
    } else if (s->grid_cols > 0) {
        ncols = s->grid_cols;
        int fixed = gap * (ncols - 1), frs = 0;
        for (int i = 0; i < ncols; i++) {
            if (s->grid_col[i].unit == L_PX) fixed += s->grid_col[i].v;
            else if (s->grid_col[i].unit == L_PCT) fixed += cw * s->grid_col[i].v / 1000;
            else frs += s->grid_col[i].v;
        }
        int rest = cw - fixed > 0 ? cw - fixed : 0;
        for (int i = 0; i < ncols; i++) {
            const struct len *l = &s->grid_col[i];
            colw[i] = l->unit == L_PX ? l->v : l->unit == L_PCT ? cw * l->v / 1000 : frs ? (int)((long)rest * l->v / frs) : 0;
        }
    } else {
        ncols = 1;
        colw[0] = cw;
    }
    int y = cy, col = 0, rowh = 0, c0row = P->ncmds;
    int xs[12];
    int x = 0;
    for (int i = 0; i < ncols; i++) { xs[i] = x; x += colw[i] + gap; }
    struct { int c0, c1, h; struct lres r; struct box b; } row[12];
    int nrow = 0;
    for (struct node *c = n->first;; c = c ? c->next : NULL) {
        int end = !c;
        if (c && (c->type != 0 || !c->st || c->st->display == D_NONE)) continue;
        if (!end) {
            struct box b;
            get_box(c->st, colw[col], &b);
            int w = colw[col] - b.ml - b.mr;
            int c0 = P->ncmds;
            int h = layout_block(c, 0, b.mt, colw[col], w, NULL, &row[nrow].r);
            row[nrow].c0 = c0; row[nrow].c1 = P->ncmds; row[nrow].h = h + b.mt + b.mb; row[nrow].b = b;
            shift_cmds(c0, P->ncmds, cx + xs[col], 0);
            if (row[nrow].h > rowh) rowh = row[nrow].h;
            nrow++;
            col++;
        }
        if (end || col == ncols) {
            for (int q = 0; q < nrow; q++) {
                shift_cmds(row[q].c0, row[q].c1, 0, y);
                if (n->st->align_items == AI_STRETCH || n->st->align_items == AI_AUTO) {
                    int want = rowh - row[q].b.mt - row[q].b.mb;
                    if (want > row[q].r.h) set_box_height(&row[q].r, want);
                }
            }
            if (nrow) y += rowh + s->gap_r;
            nrow = 0; col = 0; rowh = 0;
        }
        if (end) break;
    }
    (void)c0row;
    return y - cy - (y > cy ? s->gap_r : 0);
}

/* ---- tables ---- */

struct tcell {
    struct node *n;
    int row, col, cspan, rspan, minw, maxw, w, h, c0, c1;
    struct lres r;
};

static int layout_table(struct node *t, int cx, int cy, int tw)
{
    const struct style *ts = t->st;
    int spacing = ts->collapse ? 0 : ts->gap_c;
    struct tcell *cells = malloc(2000 * sizeof *cells);
    if (!cells) return 0;
    int ncell = 0, nrows = 0, ncols = 0;
    int rowspan_left[64] = {0};
    struct node *caption = NULL;
    int row_h_spec[256] = {0};
    struct node *rownode[256];
    const char *cpad = attr_val(t, "cellpadding");
    for (struct node *g = t->first; g; g = g->next) {
        if (g->type != 0 || !g->st || g->st->display == D_NONE) continue;
        struct node *rows[2] = {g, NULL};
        struct node *rlist = g;
        int is_group = g->st->display == D_ROW_GROUP;
        if (g->st->display == D_CAPTION) { caption = g; continue; }
        if (!is_group && g->st->display != D_ROW) continue;
        (void)rows;
        for (struct node *r = is_group ? g->first : g; r; r = is_group ? r->next : NULL) {
            if (r->type != 0 || !r->st || r->st->display != D_ROW) continue;
            (void)rlist;
            if (nrows >= 256) break;
            int col = 0;
            rownode[nrows] = r;
            if (r->st->height.unit == L_PX) row_h_spec[nrows] = r->st->height.v;
            for (struct node *c = r->first; c; c = c->next) {
                if (c->type != 0 || !c->st || c->st->display != D_CELL) continue;
                while (col < 64 && rowspan_left[col] > 0) col++;
                if (ncell >= 2000 || col >= 64) break;
                const char *cs = attr_val(c, "colspan"), *rs = attr_val(c, "rowspan");
                struct tcell *tc = &cells[ncell++];
                memset(tc, 0, sizeof *tc);
                tc->n = c; tc->row = nrows; tc->col = col;
                tc->cspan = cs ? atoi(cs) : 1; if (tc->cspan < 1) tc->cspan = 1; if (col + tc->cspan > 64) tc->cspan = 64 - col;
                tc->rspan = rs ? atoi(rs) : 1; if (tc->rspan < 1) tc->rspan = 1;
                if (tc->rspan > 1) for (int k = 0; k < tc->cspan; k++) rowspan_left[col + k] = tc->rspan;
                col += tc->cspan;
                if (col > ncols) ncols = col;
            }
            for (int k = 0; k < 64; k++) if (rowspan_left[k] > 0) rowspan_left[k]--;
            nrows++;
        }
    }
    int caph = 0;
    if (!ncell) { free(cells); return 0; }
    int colmin[64] = {0}, colmax[64] = {0}, colw[64];
    for (int i = 0; i < ncell; i++) {
        struct tcell *tc = &cells[i];
        const struct style *cs = tc->n->st;
        struct box b;
        get_box(cs, tw, &b);
        int pad_override = cpad && b.pl == 1 && b.pr == 1 && b.pt == 1 && b.pb == 1;
        (void)pad_override;
        tc->minw = intr_outer(tc->n, 0);
        tc->maxw = intr_outer(tc->n, 1);
        if (cs->width.unit == L_PX) {
            int ww = cs->width.v + (cs->box_sizing ? 0 : b.pl + b.pr + b.bl + b.br);
            if (tc->maxw < ww) tc->maxw = ww;
            if (tc->minw < ww) tc->minw = ww;
        } else if (cs->width.unit == L_PCT) {
            int ww = tw * cs->width.v / 1000;
            if (tc->maxw < ww) tc->maxw = ww;
        }
        if (tc->maxw < tc->minw) tc->maxw = tc->minw;
        if (tc->cspan == 1) {
            if (tc->minw > colmin[tc->col]) colmin[tc->col] = tc->minw;
            if (tc->maxw > colmax[tc->col]) colmax[tc->col] = tc->maxw;
        }
    }
    for (int i = 0; i < ncell; i++) {                                         /* spanning cells widen their columns */
        struct tcell *tc = &cells[i];
        if (tc->cspan == 1) continue;
        int smin = 0, smax = 0;
        for (int k = 0; k < tc->cspan; k++) { smin += colmin[tc->col + k]; smax += colmax[tc->col + k]; }
        smin += (tc->cspan - 1) * spacing; smax += (tc->cspan - 1) * spacing;
        if (tc->minw > smin) for (int k = 0; k < tc->cspan; k++) colmin[tc->col + k] += (tc->minw - smin) / tc->cspan;
        if (tc->maxw > smax) for (int k = 0; k < tc->cspan; k++) colmax[tc->col + k] += (tc->maxw - smax) / tc->cspan;
    }
    for (int c = 0; c < ncols; c++) if (colmax[c] < colmin[c]) colmax[c] = colmin[c];
    int avail = tw - spacing * (ncols + 1);
    int smin = 0, smax = 0;
    for (int c = 0; c < ncols; c++) { smin += colmin[c]; smax += colmax[c]; }
    if (avail >= smax) {
        int extra = avail - smax;
        int grow_all = ts->width.unit != L_AUTO;
        for (int c = 0; c < ncols; c++)
            colw[c] = colmax[c] + (grow_all && smax > 0 ? (int)((long)extra * colmax[c] / smax) : grow_all ? extra / ncols : 0);
    } else if (avail <= smin) {
        for (int c = 0; c < ncols; c++) colw[c] = colmin[c];
    } else {
        for (int c = 0; c < ncols; c++)
            colw[c] = colmin[c] + (smax > smin ? (int)((long)(colmax[c] - colmin[c]) * (avail - smin) / (smax - smin)) : 0);
    }
    int colx[65];
    colx[0] = spacing;
    for (int c = 0; c < ncols; c++) colx[c + 1] = colx[c] + colw[c] + spacing;

    if (caption) {
        struct lres cr;
        int c0 = P->ncmds;
        struct box cb;
        get_box(caption->st, tw, &cb);
        caph = layout_block(caption, 0, cb.mt, tw, -1, NULL, &cr) + cb.mt + cb.mb;
        shift_cmds(c0, P->ncmds, cx, cy);
    }
    int y = cy + caph + spacing;
    int *rowh = calloc(nrows + 1, sizeof(int)), *rowy = calloc(nrows + 1, sizeof(int));
    if (!rowh || !rowy) { free(cells); free(rowh); free(rowy); return caph; }
    for (int i = 0; i < ncell; i++) {                                         /* lay every cell out at the origin */
        struct tcell *tc = &cells[i];
        int w = 0;
        for (int k = 0; k < tc->cspan; k++) w += colw[tc->col + k];
        w += (tc->cspan - 1) * spacing;
        tc->w = w;
        tc->c0 = P->ncmds;
        tc->h = layout_block(tc->n, 0, 0, w, w, NULL, &tc->r);
        tc->c1 = P->ncmds;
        if (tc->rspan == 1 && tc->h > rowh[tc->row]) rowh[tc->row] = tc->h;
    }
    for (int r = 0; r < nrows; r++) if (row_h_spec[r] > rowh[r]) rowh[r] = row_h_spec[r];
    for (int i = 0; i < ncell; i++) {                                         /* tall rowspans grow their last row */
        struct tcell *tc = &cells[i];
        if (tc->rspan > 1) {
            int sum = 0, last = tc->row + tc->rspan - 1;
            if (last >= nrows) last = nrows - 1;
            for (int r = tc->row; r <= last; r++) sum += rowh[r] + spacing;
            sum -= spacing;
            if (tc->h > sum) rowh[last] += tc->h - sum;
        }
    }
    int yy = y;
    for (int r = 0; r < nrows; r++) { rowy[r] = yy; yy += rowh[r] + spacing; }
    for (int r = 0; r < nrows; r++) {                                         /* row backgrounds go under the cells */
        struct node *rn = rownode[r];
        if (rn->st->bg >> 24) {
            int k = cmd_new(K_RECT);
            if (k >= 0) { P->cmds[k].color = rn->st->bg; P->cmds[k].x = cx + spacing; P->cmds[k].y = rowy[r]; P->cmds[k].w = tw - 2 * spacing; P->cmds[k].h = rowh[r]; }
        }
    }
    for (int i = 0; i < ncell; i++) {
        struct tcell *tc = &cells[i];
        int last = tc->row + tc->rspan - 1;
        if (last >= nrows) last = nrows - 1;
        int ch = rowy[last] + rowh[last] - rowy[tc->row];
        int cxp = cx + colx[tc->col];
        shift_cmds(tc->c0, tc->c1, cxp, rowy[tc->row]);
        if (ch > tc->h) {
            set_box_height(&tc->r, ch);
            int va = tc->n->st->vertical_align;
            const char *vattr = attr_val(tc->n, "valign");
            if (vattr && !strcmp(vattr, "top")) va = VA_TOP;
            else if (vattr && !strcmp(vattr, "bottom")) va = VA_BOTTOM;
            else if (vattr && !strcmp(vattr, "middle")) va = VA_MIDDLE;
            int dy = va == VA_MIDDLE ? (ch - tc->h) / 2 : va == VA_BOTTOM ? ch - tc->h : 0;
            if (dy) shift_cmds(tc->r.cc0, tc->r.cc1, 0, dy);
        }
    }
    int total = yy - cy;
    free(cells); free(rowh); free(rowy);
    return total;
}

/* ============================== block layout ============================== */

static int layout_block(struct node *n, int x, int y, int cw, int force_w, struct bfc *pf, struct lres *res)
{
    const struct style *s = n->st;
    struct box b;
    get_box(s, cw, &b);
    struct lres dummy;
    if (!res) res = &dummy;
    res->bg = res->bgimg = -1;
    for (int i = 0; i < 4; i++) res->bord[i] = -1;

    int extra_x = b.pl + b.pr + b.bl + b.br;
    int bw;
    if (force_w >= 0) {
        bw = force_w;
    } else {
        int specified = 0;
        if (s->width.unit != L_AUTO) {
            bw = len_px(s->width, cw, 0) + (s->box_sizing ? 0 : extra_x);
            specified = 1;
        } else if (s->display == D_TABLE) {
            bw = intr_outer(n, 1) - b.ml - b.mr;
            if (bw > cw - b.ml - b.mr) bw = cw - b.ml - b.mr;
            specified = 1;
        } else {
            bw = cw - b.ml - b.mr;
        }
        if (s->max_w.unit != L_AUTO) {
            int m = len_px(s->max_w, cw, 1 << 20) + (s->box_sizing ? 0 : extra_x);
            if (bw > m) { bw = m; specified = 1; }
        }
        if (s->min_w.unit != L_AUTO) {
            int m = len_px(s->min_w, cw, 0) + (s->box_sizing ? 0 : extra_x);
            if (bw < m) bw = m;
        }
        if (specified && (b.ml_auto || b.mr_auto)) {
            int free = cw - bw - (b.ml_auto ? 0 : b.ml) - (b.mr_auto ? 0 : b.mr);
            if (free < 0) free = 0;
            if (b.ml_auto && b.mr_auto) b.ml = free / 2; else if (b.ml_auto) b.ml = free;
        }
    }
    if (bw < extra_x) bw = extra_x;
    int bx = x + b.ml;
    int cx = bx + b.bl + b.pl, cwid = bw - extra_x;

    /* replaced elements used as blocks */
    if (is_replaced(n) && n->tag != TG_BUTTON) {
        int w, h;
        replaced_size(n, &w, &h);
        if (n->tag == TG_IMG && w > cw && w > 0) { h = h * cw / w; w = cw; }
        emit_replaced(n, bx, y, w, h);
        res->h = h;
        res->mb = b.mb;
        res->cc0 = res->cc1 = P->ncmds;
        return h;
    }

    struct bfc own = {0};
    int root = is_bfc_root(n);
    struct bfc *f = root || !pf ? &own : pf;
    res->bg = emit_bg(s, bx, y, bw, 0, n->link, &res->bgimg, n);
    int cy = y + b.bt + b.pt;
    int ch, trail = 0;
    res->cc0 = P->ncmds;
    int def_h = -1;
    if (s->height.unit == L_PX) def_h = s->height.v - (s->box_sizing ? b.pt + b.pb + b.bt + b.bb : 0);
    switch (s->display) {
    case D_FLEX: case D_INLINE_FLEX: ch = layout_flex(n, cx, cy, cwid, def_h); break;
    case D_GRID: ch = layout_grid(n, cx, cy, cwid); break;
    case D_TABLE: ch = layout_table(n, cx, cy, cwid); break;
    default:
        ch = layout_flow(n, cx, cy, cwid, f, !root && b.bt == 0 && b.pt == 0, &trail);
        if (root && float_bottom(f, 3) - cy > ch) ch = float_bottom(f, 3) - cy;
        break;
    }
    int mb = b.mb;
    int collapse_bottom = !root && b.bb == 0 && b.pb == 0 && s->height.unit == L_AUTO && s->display != D_FLEX && s->display != D_GRID && s->display != D_TABLE;
    if (s->display == D_BLOCK || s->display == D_LIST_ITEM || s->display == D_CAPTION || s->display == D_CELL) {
        if (collapse_bottom) mb = collapse_m(b.mb, trail);
        else ch += trail;
    }
    res->cc1 = P->ncmds;
    int h = ch + b.pt + b.pb + b.bt + b.bb;
    if (s->height.unit == L_PX) h = s->height.v + (s->box_sizing ? 0 : b.pt + b.pb + b.bt + b.bb);
    if (s->min_h.unit == L_PX) { int m = s->min_h.v + (s->box_sizing ? 0 : b.pt + b.pb + b.bt + b.bb); if (h < m) h = m; }
    else if (s->min_h.unit == L_PCT && s->min_h.v >= 1000) { int m = 560; if (h < m && n->tag == TG_BODY) h = m; }
    if (s->max_h.unit == L_PX) { int m = s->max_h.v + (s->box_sizing ? 0 : b.pt + b.pb + b.bt + b.bb); if (h > m) h = m; }
    if (h < b.pt + b.pb + b.bt + b.bb) h = b.pt + b.pb + b.bt + b.bb;
    if (res->bg >= 0) P->cmds[res->bg].h = h;
    if (res->bgimg >= 0) P->cmds[res->bgimg].h = h;
    emit_borders(s, bx, y, bw, h, res->bord, 0, 0);
    if (s->display == D_LIST_ITEM) emit_marker(n, cx, cy);
    if (n->link && s->display != D_INLINE && s->display != D_INLINE_BLOCK) {
        /* block-level links (cards, buttons): their whole box is clickable text/image commands inside */
        for (int i = res->cc0; i < res->cc1; i++)
            if (!P->cmds[i].link && (P->cmds[i].kind == K_TEXT || P->cmds[i].kind == K_IMAGE)) P->cmds[i].link = n->link;
    }
    res->h = h;
    res->mb = mb;
    return h;
}

/* ============================== entry point ============================== */

static void reset_caches(struct node *n)
{
    n->mincw = n->maxcw = -1;
    n->bk = 0;
    for (struct node *c = n->first; c; c = c->next)
        reset_caches(c);
}

void page_layout(struct page *p, int width)
{
    P = p;
    p->layout_w = width;
    p->ncmds = 0;
    lay_reset();
    if (!p->root || !p->styled) {
        p->height = 0;
        return;
    }
    reset_caches(p->root);
    struct node *body = p->body ? p->body : p->root;
    if (!body->st) {
        p->height = 0;
        return;
    }
    struct lres r;
    int top = eff_mt(body, width);
    if (top < 0) top = 0;
    int h = layout_block(body, 0, top, width, -1, NULL, &r);
    p->height = top + h + (r.mb > 0 ? r.mb : 0);
}
