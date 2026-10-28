/* CSS: stylesheet parsing, selector matching, the cascade and computed styles. */
#include "web.h"

enum {
    P_NONE, P_DISPLAY, P_POSITION, P_FLOAT, P_CLEAR, P_COLOR, P_BACKGROUND, P_BG_COLOR, P_BG_IMAGE, P_BG_SIZE, P_BG_POS,
    P_FONT, P_FONT_SIZE, P_FONT_WEIGHT, P_FONT_STYLE, P_FONT_FAMILY, P_TEXT_ALIGN, P_TEXT_DECOR, P_TEXT_TRANSFORM,
    P_LINE_HEIGHT, P_WHITE_SPACE, P_VISIBILITY, P_WIDTH, P_HEIGHT, P_MIN_W, P_MAX_W, P_MIN_H, P_MAX_H,
    P_MARGIN, P_MARGIN_T, P_MARGIN_R, P_MARGIN_B, P_MARGIN_L, P_PADDING, P_PADDING_T, P_PADDING_R, P_PADDING_B,
    P_PADDING_L, P_BORDER, P_BORDER_T, P_BORDER_R, P_BORDER_B, P_BORDER_L, P_BORDER_W, P_BORDER_STYLE, P_BORDER_COLOR,
    P_BORDER_TW, P_BORDER_RW, P_BORDER_BW, P_BORDER_LW, P_RADIUS, P_BOX_SIZING, P_FLEX, P_FLEX_DIR, P_FLEX_WRAP,
    P_FLEX_FLOW, P_GROW, P_SHRINK, P_BASIS, P_JUSTIFY, P_ALIGN_ITEMS, P_ALIGN_SELF, P_GAP, P_ROW_GAP, P_COL_GAP,
    P_LIST_STYLE, P_VALIGN, P_OVERFLOW, P_OPACITY, P_ORDER, P_GRID_COLS, P_BSPACING, P_BCOLLAPSE, P_VAR, P_COUNT
};

static const struct { const char *name; int id; } PROPS[] = {
    {"display", P_DISPLAY}, {"position", P_POSITION}, {"float", P_FLOAT}, {"clear", P_CLEAR}, {"color", P_COLOR},
    {"background", P_BACKGROUND}, {"background-color", P_BG_COLOR}, {"background-image", P_BG_IMAGE},
    {"background-size", P_BG_SIZE}, {"background-position", P_BG_POS}, {"font", P_FONT}, {"font-size", P_FONT_SIZE},
    {"font-weight", P_FONT_WEIGHT}, {"font-style", P_FONT_STYLE}, {"font-family", P_FONT_FAMILY},
    {"text-align", P_TEXT_ALIGN}, {"text-decoration", P_TEXT_DECOR}, {"text-decoration-line", P_TEXT_DECOR},
    {"text-transform", P_TEXT_TRANSFORM}, {"line-height", P_LINE_HEIGHT}, {"white-space", P_WHITE_SPACE},
    {"visibility", P_VISIBILITY}, {"width", P_WIDTH}, {"height", P_HEIGHT}, {"min-width", P_MIN_W},
    {"max-width", P_MAX_W}, {"min-height", P_MIN_H}, {"max-height", P_MAX_H}, {"margin", P_MARGIN},
    {"margin-top", P_MARGIN_T}, {"margin-right", P_MARGIN_R}, {"margin-bottom", P_MARGIN_B},
    {"margin-left", P_MARGIN_L}, {"padding", P_PADDING}, {"padding-top", P_PADDING_T}, {"padding-right", P_PADDING_R},
    {"padding-bottom", P_PADDING_B}, {"padding-left", P_PADDING_L}, {"border", P_BORDER}, {"border-top", P_BORDER_T},
    {"border-right", P_BORDER_R}, {"border-bottom", P_BORDER_B}, {"border-left", P_BORDER_L},
    {"border-width", P_BORDER_W}, {"border-style", P_BORDER_STYLE}, {"border-color", P_BORDER_COLOR},
    {"border-top-width", P_BORDER_TW}, {"border-right-width", P_BORDER_RW}, {"border-bottom-width", P_BORDER_BW},
    {"border-left-width", P_BORDER_LW}, {"border-radius", P_RADIUS}, {"box-sizing", P_BOX_SIZING}, {"flex", P_FLEX},
    {"flex-direction", P_FLEX_DIR}, {"flex-wrap", P_FLEX_WRAP}, {"flex-flow", P_FLEX_FLOW}, {"flex-grow", P_GROW},
    {"flex-shrink", P_SHRINK}, {"flex-basis", P_BASIS}, {"justify-content", P_JUSTIFY},
    {"align-items", P_ALIGN_ITEMS}, {"align-self", P_ALIGN_SELF}, {"gap", P_GAP}, {"grid-gap", P_GAP},
    {"row-gap", P_ROW_GAP}, {"column-gap", P_COL_GAP}, {"list-style", P_LIST_STYLE}, {"list-style-type", P_LIST_STYLE},
    {"vertical-align", P_VALIGN}, {"overflow", P_OVERFLOW}, {"overflow-x", P_OVERFLOW}, {"opacity", P_OPACITY},
    {"order", P_ORDER}, {"grid-template-columns", P_GRID_COLS}, {"border-spacing", P_BSPACING},
    {"border-collapse", P_BCOLLAPSE},
};

struct decl {
    u8 prop, imp;
    short vlen, nlen;
    const char *val, *name;                        /* name: custom properties only */
};

struct simple {
    u8 comb, tag, ncls, nattr, pseudo, has_nth;
    const char *tagname, *id;
    short tagn, idn;
    const char *cls[4];
    short clsn[4];
    struct { const char *n, *v; short nl, vl; u8 op; } attr[3];
    int nth_a, nth_b;
    struct simple *neg;
};
enum { PS_FIRST = 1, PS_LAST = 2, PS_ROOT = 4, PS_EMPTY = 8, PS_LINK = 16 };

struct rule {
    struct simple *sel;
    int nsel;
    u32 key, mask[2];
    struct decl *d;
    int nd;
};

static struct page *g_page;
static const char *g_sheet_url;
static int g_vw = 800, g_vh = 600;

/* ---- small helpers ---- */

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
/* Bounded substring search: the CSS text is not NUL-terminated at the end of a value. */
static int contains(const char *v, int n, const char *w)
{
    int m = strlen(w);
    for (int i = 0; i + m <= n; i++)
        if (!strncmp(v + i, w, m))
            return 1;
    return 0;
}

static const char *find_in(const char *v, int n, const char *w)
{
    int m = strlen(w);
    for (int i = 0; i + m <= n; i++)
        if (!strncmp(v + i, w, m))
            return v + i;
    return NULL;
}

static int is_space(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }
static int ident_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c >= 0x80;
}

static int ieq(const char *s, int n, const char *w)
{
    if ((int)strlen(w) != n)
        return 0;
    for (int i = 0; i < n; i++)
        if (lc(s[i]) != w[i])
            return 0;
    return 1;
}

static const char *skip_ws(const char *p, const char *e)
{
    while (p < e) {
        if (is_space(*p))
            p++;
        else if (p + 1 < e && p[0] == '/' && p[1] == '*') {
            p += 2;
            while (p + 1 < e && !(p[0] == '*' && p[1] == '/'))
                p++;
            p = p + 2 < e ? p + 2 : e;
        } else
            break;
    }
    return p;
}

/* Next whitespace-separated value token (parentheses and quotes group). */
static const char *next_tok(const char *p, const char *e, const char **ts, int *tl)
{
    p = skip_ws(p, e);
    *ts = p;
    int depth = 0;
    char q = 0;
    while (p < e) {
        char c = *p;
        if (q) {
            if (c == q) q = 0;
        } else if (c == '"' || c == '\'') q = c;
        else if (c == '(') depth++;
        else if (c == ')') depth--;
        else if (!depth && (is_space(c) || c == ',') && p > *ts) break;
        else if (!depth && c == ',' && p == *ts) { p++; break; }
        p++;
    }
    *tl = p - *ts;
    return p;
}

/* ---- numbers, lengths, colors ---- */

static int parse_num(const char *s, int n, int *whole_x1000, const char **rest)
{
    int i = 0, neg = 0;
    if (i < n && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    int v = 0, any = 0, frac = 0, scale = 1;
    while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i++] - '0'); any = 1; if (v > 1000000) v = 1000000; }
    if (i < n && s[i] == '.') {
        i++;
        while (i < n && s[i] >= '0' && s[i] <= '9') {
            if (scale < 1000) { frac = frac * 10 + (s[i] - '0'); scale *= 10; }
            i++;
            any = 1;
        }
    }
    if (!any)
        return 0;
    int r = v * 1000 + (scale > 1 ? frac * 1000 / scale : 0);
    *whole_x1000 = neg ? -r : r;
    *rest = s + i;
    return 1;
}

static int px_of(int x1000, const char *unit, int un, int font_px)
{
    if (!un || ieq(unit, un, "px")) return x1000 / 1000;
    if (ieq(unit, un, "em")) return x1000 * font_px / 1000;
    if (ieq(unit, un, "rem")) return x1000 * 16 / 1000;
    if (ieq(unit, un, "pt")) return x1000 * 4 / 3000;
    if (ieq(unit, un, "pc")) return x1000 * 16 / 1000;
    if (ieq(unit, un, "ch")) return x1000 * font_px / 2000;
    if (ieq(unit, un, "ex")) return x1000 * font_px / 2000;
    if (ieq(unit, un, "vw")) return x1000 * g_vw / 100000;
    if (ieq(unit, un, "vh")) return x1000 * g_vh / 100000;
    if (ieq(unit, un, "vmin")) return x1000 * (g_vw < g_vh ? g_vw : g_vh) / 100000;
    if (ieq(unit, un, "vmax")) return x1000 * (g_vw > g_vh ? g_vw : g_vh) / 100000;
    if (ieq(unit, un, "cm")) return x1000 * 96 / 2540;
    if (ieq(unit, un, "mm")) return x1000 * 96 / 25400;
    if (ieq(unit, un, "in")) return x1000 * 96 / 1000;
    return 0;
}

static int calc_px(const char *s, int n, int font_px, int *ok);

int css_parse_len(const char *s, int n, int font_px, struct len *out)
{
    while (n > 0 && is_space(*s)) { s++; n--; }
    while (n > 0 && is_space(s[n - 1])) n--;
    if (n == 0) return 0;
    if (ieq(s, n, "auto") || ieq(s, n, "none") || ieq(s, n, "normal") || ieq(s, n, "initial") || ieq(s, n, "unset")) {
        out->unit = L_AUTO;
        out->v = 0;
        return 1;
    }
    if (n > 5 && !strncmp(s, "calc(", 5)) {
        int ok = 1;
        int v = calc_px(s + 5, n - 6, font_px, &ok);
        if (!ok) { out->unit = L_AUTO; out->v = 0; return 1; }
        out->unit = L_PX;
        out->v = v;
        return 1;
    }
    if (n > 4 && (!strncmp(s, "min(", 4) || !strncmp(s, "max(", 4) || !strncmp(s, "clamp(", 6))) {
        out->unit = L_AUTO;
        out->v = 0;
        return 1;
    }
    int x;
    const char *rest;
    if (!parse_num(s, n, &x, &rest))
        return 0;
    int un = s + n - rest;
    if (un == 1 && *rest == '%') {
        out->unit = L_PCT;
        out->v = x / 100;
        return 1;
    }
    if (un == 2 && ieq(rest, un, "fr")) {
        out->unit = L_FR;
        out->v = x;
        return 1;
    }
    if (un && !(un >= 1 && (rest[0] | 0x20) >= 'a' && (rest[0] | 0x20) <= 'z'))
        return 0;
    out->unit = L_PX;
    out->v = px_of(x, rest, un, font_px);
    return 1;
}

/* A calc() made of px-like terms only; anything with % makes the whole thing "auto". */
static int calc_px(const char *s, int n, int font_px, int *ok)
{
    int total = 0, sign = 1, i = 0;
    while (i < n) {
        while (i < n && is_space(s[i])) i++;
        if (i >= n) break;
        if ((s[i] == '+' || s[i] == '-') && (i + 1 < n && is_space(s[i + 1]))) {
            sign = s[i] == '-' ? -1 : 1;
            i++;
            continue;
        }
        int j = i;
        int depth = 0;
        while (j < n && (depth || !is_space(s[j]))) {
            if (s[j] == '(') depth++;
            if (s[j] == ')') depth--;
            j++;
        }
        struct len l;
        if (s[i] == '(') {
            total += sign * calc_px(s + i + 1, j - i - 2, font_px, ok);
        } else if (s[i] == '*' || s[i] == '/') {
            j = i + 1;
        } else if (css_parse_len(s + i, j - i, font_px, &l) && l.unit == L_PX) {
            total += sign * l.v;
        } else {
            *ok = 0;
        }
        sign = 1;
        i = j;
    }
    return total;
}

static const struct { const char *n; u32 c; } NAMED[] = {
    {"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff},
    {"yellow", 0xffff00}, {"orange", 0xffa500}, {"purple", 0x800080}, {"gray", 0x808080}, {"grey", 0x808080},
    {"silver", 0xc0c0c0}, {"maroon", 0x800000}, {"navy", 0x000080}, {"teal", 0x008080}, {"lime", 0x00ff00},
    {"aqua", 0x00ffff}, {"cyan", 0x00ffff}, {"fuchsia", 0xff00ff}, {"magenta", 0xff00ff}, {"olive", 0x808000},
    {"pink", 0xffc0cb}, {"brown", 0xa52a2a}, {"gold", 0xffd700}, {"indigo", 0x4b0082}, {"violet", 0xee82ee},
    {"coral", 0xff7f50}, {"salmon", 0xfa8072}, {"khaki", 0xf0e68c}, {"crimson", 0xdc143c}, {"tomato", 0xff6347},
    {"turquoise", 0x40e0d0}, {"tan", 0xd2b48c}, {"beige", 0xf5f5dc}, {"ivory", 0xfffff0}, {"lavender", 0xe6e6fa},
    {"darkgray", 0xa9a9a9}, {"darkgrey", 0xa9a9a9}, {"lightgray", 0xd3d3d3}, {"lightgrey", 0xd3d3d3},
    {"dimgray", 0x696969}, {"dimgrey", 0x696969}, {"whitesmoke", 0xf5f5f5}, {"gainsboro", 0xdcdcdc},
    {"darkblue", 0x00008b}, {"darkgreen", 0x006400}, {"darkred", 0x8b0000}, {"lightblue", 0xadd8e6},
    {"lightgreen", 0x90ee90}, {"skyblue", 0x87ceeb}, {"steelblue", 0x4682b4}, {"royalblue", 0x4169e1},
    {"dodgerblue", 0x1e90ff}, {"slategray", 0x708090}, {"slategrey", 0x708090}, {"midnightblue", 0x191970},
    {"firebrick", 0xb22222}, {"forestgreen", 0x228b22}, {"seagreen", 0x2e8b57}, {"limegreen", 0x32cd32},
    {"chocolate", 0xd2691e}, {"sienna", 0xa0522d}, {"orchid", 0xda70d6}, {"plum", 0xdda0dd}, {"snow", 0xfffafa},
    {"aliceblue", 0xf0f8ff}, {"ghostwhite", 0xf8f8ff}, {"honeydew", 0xf0fff0}, {"mintcream", 0xf5fffa},
    {"azure", 0xf0ffff}, {"linen", 0xfaf0e6}, {"wheat", 0xf5deb3}, {"cornsilk", 0xfff8dc}, {"darkorange", 0xff8c00},
    {"hotpink", 0xff69b4}, {"deeppink", 0xff1493}, {"darkviolet", 0x9400d3}, {"darkcyan", 0x008b8b},
    {"lightyellow", 0xffffe0}, {"lightcyan", 0xe0ffff}, {"lightpink", 0xffb6c1}, {"lightsalmon", 0xffa07a},
    {"rebeccapurple", 0x663399}, {"cadetblue", 0x5f9ea0}, {"darkslategray", 0x2f4f4f}, {"goldenrod", 0xdaa520},
};

static int hexd(int c) { c |= 0x20; return c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= '0' && c <= '9' ? c - '0' : -1; }

static u32 hsl_to_rgb(int h, int s, int l)                       /* h 0..359, s/l 0..100 */
{
    h = ((h % 360) + 360) % 360;
    int c = (100 - abs_i(2 * l - 100)) * s / 100;                /* chroma, 0..100 */
    int hp = h / 60, f = (h % 60) * 100 / 60;
    int x = hp & 1 ? c * (100 - f) / 100 : c * f / 100;
    int r = 0, g = 0, b = 0;
    switch (hp) {
    case 0: r = c; g = x; break;
    case 1: r = x; g = c; break;
    case 2: g = c; b = x; break;
    case 3: g = x; b = c; break;
    case 4: r = x; b = c; break;
    default: r = c; b = x; break;
    }
    int m = l - c / 2;
    r = (r + m) * 255 / 100; g = (g + m) * 255 / 100; b = (b + m) * 255 / 100;
    if (r < 0) r = 0; if (g < 0) g = 0; if (b < 0) b = 0;
    if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
    return 0xFF000000u | r << 16 | g << 8 | b;
}

/* 0xAARRGGBB; returns 0 on failure (so "transparent" is reported through *transparent). */
static int parse_color(const char *s, int n, u32 cur, u32 *out)
{
    while (n > 0 && is_space(*s)) { s++; n--; }
    while (n > 0 && is_space(s[n - 1])) n--;
    if (n == 0) return 0;
    if (n > 11 && !strncmp(s, "light-dark(", 11)) {                       /* we are always "light" */
        const char *a = s + 11, *e = s + n - 1, *c = a;
        int depth = 0;
        while (c < e && !(*c == ',' && !depth)) { if (*c == '(') depth++; if (*c == ')') depth--; c++; }
        return parse_color(a, c - a, cur, out);
    }
    if (s[0] == '#') {
        int d[8], k = 0;
        for (int i = 1; i < n && k < 8; i++) {
            d[k] = hexd(s[i]);
            if (d[k++] < 0) return 0;
        }
        if (k == 3 || k == 4) {
            *out = (k == 4 ? d[3] * 17u : 255u) << 24 | d[0] * 17u << 16 | d[1] * 17u << 8 | d[2] * 17u;
            return 1;
        }
        if (k == 6 || k == 8) {
            *out = (k == 8 ? (d[6] << 4 | d[7]) : 255u) << 24 | (u32)(d[0] << 4 | d[1]) << 16 | (u32)(d[2] << 4 | d[3]) << 8 |
                   (d[4] << 4 | d[5]);
            return 1;
        }
        return 0;
    }
    if (ieq(s, n, "transparent")) { *out = 0; return 1; }
    if (ieq(s, n, "currentcolor")) { *out = cur; return 1; }
    if ((n > 4 && (!strncmp(s, "rgb(", 4) || !strncmp(s, "hsl(", 4))) || (n > 5 && (!strncmp(s, "rgba(", 5) || !strncmp(s, "hsla(", 5)))) {
        int hsl = s[0] == 'h';
        const char *p = strchr(s, '(') + 1, *e = s + n - 1;
        int v[4] = {0, 0, 0, 255}, k = 0;
        while (p < e && k < 4) {
            while (p < e && (is_space(*p) || *p == ',' || *p == '/')) p++;
            if (p >= e) break;
            int x;
            const char *rest;
            if (!parse_num(p, e - p, &x, &rest)) return 0;
            int pct = rest < e && *rest == '%';
            if (k == 3)
                v[k] = pct ? x * 255 / 100000 : x * 255 / 1000;
            else if (hsl)
                v[k] = k == 0 ? x / 1000 : x / 1000;
            else
                v[k] = pct ? x * 255 / 100000 : x / 1000;
            k++;
            p = rest;
            while (p < e && !is_space(*p) && *p != ',' && *p != '/') p++;
        }
        for (int i = 0; i < 4; i++) { if (v[i] < 0) v[i] = 0; if (v[i] > 255 && !(hsl && i == 0)) v[i] = 255; }
        u32 c = hsl ? hsl_to_rgb(v[0], v[1] > 100 ? 100 : v[1], v[2] > 100 ? 100 : v[2]) : 0xFF000000u | v[0] << 16 | v[1] << 8 | v[2];
        *out = (c & 0xFFFFFF) | (u32)v[3] << 24;
        return 1;
    }
    for (u32 i = 0; i < sizeof NAMED / sizeof NAMED[0]; i++)
        if (ieq(s, n, NAMED[i].n)) { *out = 0xFF000000u | NAMED[i].c; return 1; }
    return 0;
}

/* ---- stylesheet parsing ---- */

static void add_rule(struct page *p, const struct rule *r)
{
    if (p->nrules == p->cap_rules) {
        int nc = p->cap_rules ? p->cap_rules * 2 : 256;
        struct rule *nr = realloc(p->rules, nc * sizeof *nr);
        if (!nr)
            return;
        p->rules = nr;
        p->cap_rules = nc;
    }
    p->rules[p->nrules++] = *r;
}

void css_free_rules(struct page *p)
{
    free(p->rules);
    p->rules = NULL;
    p->nrules = p->cap_rules = 0;
}

static int prop_id(const char *s, int n)
{
    for (u32 i = 0; i < sizeof PROPS / sizeof PROPS[0]; i++)
        if (ieq(s, n, PROPS[i].name))
            return PROPS[i].id;
    return P_NONE;
}

/* Parses "a:b; c:d !important" in [p,e) into declarations. */
static struct decl *parse_decls(struct page *pg, const char *p, const char *e, int *count)
{
    struct decl tmp[96];
    int nd = 0;
    while (p < e && nd < 96) {
        p = skip_ws(p, e);
        while (p < e && *p == ';') p = skip_ws(p + 1, e);
        if (p >= e) break;
        const char *ns = p;
        while (p < e && *p != ':' && *p != ';') p++;
        if (p >= e || *p != ':') break;
        const char *ne = p;
        while (ne > ns && is_space(ne[-1])) ne--;
        p++;
        const char *vs = skip_ws(p, e), *v = vs;
        int depth = 0;
        char q = 0;
        while (v < e) {
            if (q) { if (*v == q) q = 0; }
            else if (*v == '"' || *v == '\'') q = *v;
            else if (*v == '(') depth++;
            else if (*v == ')') depth--;
            else if (*v == ';' && !depth) break;
            v++;
        }
        const char *ve = v;
        while (ve > vs && is_space(ve[-1])) ve--;
        int imp = 0;
        if (ve - vs > 10) {
            const char *t = ve;
            while (t > vs && ident_char(t[-1])) t--;
            if (ieq(t, ve - t, "important") && t > vs && t[-1] == '!') {
                imp = 1;
                ve = t - 1;
                while (ve > vs && is_space(ve[-1])) ve--;
            }
        }
        p = v;
        if (ne - ns > 2 && ns[0] == '-' && ns[1] == '-') {
            tmp[nd++] = (struct decl){P_VAR, imp, (short)(ve - vs), (short)(ne - ns), vs, ns};
        } else {
            int id = prop_id(ns, ne - ns);
            if (id != P_NONE && ve > vs)
                tmp[nd++] = (struct decl){id, imp, (short)(ve - vs), 0, vs, NULL};
        }
    }
    *count = nd;
    if (!nd)
        return NULL;
    struct decl *d = arena_alloc(pg, nd * sizeof *d);
    if (d)
        memcpy(d, tmp, nd * sizeof *d);
    return d;
}

/* One compound selector (no combinators) from [p,e); returns 0 if unsupported. */
static int parse_simple(struct page *pg, const char *p, const char *e, struct simple *s)
{
    memset(s, 0, sizeof *s);
    while (p < e) {
        char c = *p;
        if (c == '*') { p++; continue; }
        if (c == '.' || c == '#') {
            const char *a = ++p;
            while (p < e && ident_char(*p)) p++;
            if (p == a) return 0;
            if (c == '#') { s->id = a; s->idn = p - a; }
            else if (s->ncls < 4) { s->cls[s->ncls] = a; s->clsn[s->ncls++] = p - a; }
            else return 0;
        } else if (c == '[') {
            const char *a = ++p;
            while (p < e && *p != ']') p++;
            if (p >= e || s->nattr >= 3) return 0;
            const char *end = p++;
            const char *q = a;
            while (q < end && ident_char(*q)) q++;
            int ai = s->nattr++;
            s->attr[ai].n = a;
            s->attr[ai].nl = q - a;
            if (q < end) {
                if (*q == '=') { s->attr[ai].op = '='; q++; }
                else if (q + 1 < end && q[1] == '=' && (*q == '~' || *q == '^' || *q == '$' || *q == '*' || *q == '|')) { s->attr[ai].op = *q; q += 2; }
                else return 0;
                const char *vs = q, *ve = end;
                while (ve > vs && (is_space(ve[-1]) || ve[-1] == 'i' || ve[-1] == 's') && ve - 2 >= vs && is_space(ve[-2])) ve--;
                while (ve > vs && is_space(ve[-1])) ve--;
                if (vs < ve && (*vs == '"' || *vs == '\'')) { vs++; if (ve > vs) ve--; }
                s->attr[ai].v = vs;
                s->attr[ai].vl = ve - vs;
            }
        } else if (c == ':') {
            p++;
            if (p < e && *p == ':') return 0;                      /* pseudo-elements: never match */
            const char *a = p;
            while (p < e && (ident_char(*p))) p++;
            int n = p - a;
            if (ieq(a, n, "first-child")) s->pseudo |= PS_FIRST;
            else if (ieq(a, n, "last-child")) s->pseudo |= PS_LAST;
            else if (ieq(a, n, "only-child")) s->pseudo |= PS_FIRST | PS_LAST;
            else if (ieq(a, n, "root")) s->pseudo |= PS_ROOT;
            else if (ieq(a, n, "empty")) s->pseudo |= PS_EMPTY;
            else if (ieq(a, n, "link") || ieq(a, n, "any-link")) s->pseudo |= PS_LINK;
            else if ((ieq(a, n, "nth-child") || ieq(a, n, "not")) && p < e && *p == '(') {
                const char *a2 = ++p;
                int depth = 1;
                while (p < e && depth) { if (*p == '(') depth++; if (*p == ')') depth--; p++; }
                const char *in_end = p - 1;
                if (ieq(a, n, "not")) {
                    struct simple *ng = arena_alloc(pg, sizeof *ng);
                    if (!ng || !parse_simple(pg, a2, in_end, ng)) return 0;
                    s->neg = ng;
                } else {
                    int A = 0, B = 0;
                    const char *t = a2;
                    while (t < in_end && is_space(*t)) t++;
                    if (ieq(t, in_end - t, "odd")) { A = 2; B = 1; }
                    else if (ieq(t, in_end - t, "even")) { A = 2; B = 0; }
                    else {
                        int x;
                        const char *rest;
                        const char *np = strchr(t, 'n');
                        if (np && np < in_end) {
                            if (np == t) A = 1;
                            else if (t[0] == '-' && np == t + 1) A = -1;
                            else if (parse_num(t, np - t, &x, &rest)) A = x / 1000;
                            else return 0;
                            const char *bp = np + 1;
                            while (bp < in_end && is_space(*bp)) bp++;
                            if (bp < in_end && parse_num(bp, in_end - bp, &x, &rest)) B = x / 1000;
                        } else if (parse_num(t, in_end - t, &x, &rest)) {
                            B = x / 1000;
                        } else return 0;
                    }
                    s->has_nth = 1;
                    s->nth_a = A;
                    s->nth_b = B;
                }
            } else return 0;                                      /* :hover, :is(), ... never match */
        } else if (ident_char(c)) {
            const char *a = p;
            while (p < e && ident_char(*p)) p++;
            s->tagname = a;
            s->tagn = p - a;
            s->tag = 1;
        } else return 0;
    }
    return 1;
}

static void simple_mask(const struct simple *s, u32 mask[2])
{
    mask[0] = mask[1] = 0;
    if (s->tag) mask_set(mask, s->tagname, s->tagn);
    if (s->id) mask_set(mask, s->id, s->idn);
    for (int i = 0; i < s->ncls; i++) mask_set(mask, s->cls[i], s->clsn[i]);
}

static u32 specificity(const struct rule *r)
{
    u32 a = 0, b = 0, c = 0;
    for (int i = 0; i < r->nsel; i++) {
        const struct simple *s = &r->sel[i];
        if (s->id) a++;
        b += s->ncls + s->nattr + (s->pseudo ? 1 : 0) + s->has_nth;
        if (s->tag) c++;
        if (s->neg) b++;
    }
    return (a > 3 ? 3 : a) << 10 | (b > 31 ? 31 : b) << 5 | (c > 31 ? 31 : c);
}

/* Parses one selector (with combinators) from [p,e) and adds a rule for it. */
static void add_selector(struct page *pg, const char *p, const char *e, struct decl *d, int nd, int origin, int order)
{
    struct simple tmp[12];
    int n = 0;
    char comb = 0;
    p = skip_ws(p, e);
    while (p < e) {
        const char *a = p;
        int depth = 0;
        while (p < e && (depth || !(is_space(*p) || *p == '>' || *p == '+' || *p == '~'))) {
            if (*p == '(' || *p == '[') depth++;
            if (*p == ')' || *p == ']') depth--;
            p++;
        }
        if (p > a) {
            if (n >= 12 || !parse_simple(pg, a, p, &tmp[n]))
                return;
            tmp[n].comb = comb;
            n++;
            comb = ' ';
        }
        const char *q = skip_ws(p, e);
        if (q < e && (*q == '>' || *q == '+' || *q == '~')) {
            comb = *q;
            q = skip_ws(q + 1, e);
        }
        p = q;
        if (p == a)
            return;
    }
    if (!n)
        return;
    struct rule r;
    r.sel = arena_alloc(pg, n * sizeof *r.sel);
    if (!r.sel)
        return;
    memcpy(r.sel, tmp, n * sizeof *r.sel);
    r.nsel = n;
    r.d = d;
    r.nd = nd;
    simple_mask(&r.sel[n - 1], r.mask);
    r.key = (u32)origin << 31 | specificity(&r) << 16 | (order & 0xFFFF);
    add_rule(pg, &r);
}

static int media_ok(const char *p, const char *e)
{
    int any = 0;
    while (p < e) {
        const char *q = p;
        while (q < e && *q != ',') q++;
        int ok = 1, neg = 0;
        const char *t = skip_ws(p, q);
        while (t < q) {
            const char *ts;
            int tl;
            t = next_tok(t, q, &ts, &tl);
            if (tl == 0) break;
            if (ieq(ts, tl, "not")) { neg = 1; continue; }
            if (ieq(ts, tl, "and") || ieq(ts, tl, "only") || ieq(ts, tl, "screen") || ieq(ts, tl, "all")) continue;
            if (ieq(ts, tl, "print") || ieq(ts, tl, "speech")) { ok = 0; continue; }
            if (ts[0] == '(') {
                const char *c = ts + 1, *ce = ts + tl - 1;
                const char *colon = c;
                while (colon < ce && *colon != ':') colon++;
                int nl = colon - c;
                while (nl > 0 && is_space(c[nl - 1])) nl--;
                struct len l;
                if (colon < ce && (ieq(c, nl, "min-width") || ieq(c, nl, "max-width")) &&
                    css_parse_len(colon + 1, ce - colon - 1, 16, &l) && l.unit == L_PX) {
                    if (ieq(c, nl, "min-width") ? g_vw < l.v : g_vw > l.v) ok = 0;
                } else if (colon < ce && (ieq(c, nl, "min-height") || ieq(c, nl, "max-height"))) {
                    /* height queries: assume satisfied */
                } else if (colon < ce && ieq(c, nl, "prefers-color-scheme")) {
                    if (contains(colon, ce - colon, "dark")) ok = 0;
                } else if (colon < ce && ieq(c, nl, "prefers-reduced-motion")) {
                    if (contains(colon, ce - colon, "reduce")) ok = 0;
                } else if (colon < ce && (ieq(c, nl, "orientation") || ieq(c, nl, "-webkit-min-device-pixel-ratio") ||
                                          ieq(c, nl, "min-resolution") || ieq(c, nl, "min-device-pixel-ratio"))) {
                    if (contains(colon, ce - colon, "portrait") || contains(colon, ce - colon, "2") || contains(colon, ce - colon, "192")) ok = 0;
                } else if (colon >= ce && (ieq(c, nl, "hover") || ieq(c, nl, "any-hover") || ieq(c, nl, "color"))) {
                    /* satisfied */
                } else ok = 0;
            } else ok = 0;
        }
        if (neg) ok = !ok;
        any |= ok;
        p = q < e ? q + 1 : e;
    }
    return any;
}

static const char *skip_block(const char *p, const char *e)      /* p just after '{' */
{
    int depth = 1;
    while (p < e && depth) {
        if (*p == '{') depth++;
        else if (*p == '}') depth--;
        else if (*p == '"' || *p == '\'') {
            char q = *p++;
            while (p < e && *p != q) p++;
        }
        p++;
    }
    return p;
}

static void parse_sheet(struct page *pg, const char *p, const char *e, int origin, int *order, int depth)
{
    while (p < e) {
        p = skip_ws(p, e);
        if (p >= e) break;
        if (p[0] == '<' && p + 3 < e && !strncmp(p, "<!--", 4)) { p += 4; continue; }
        if (p[0] == '-' && p + 2 < e && !strncmp(p, "-->", 3)) { p += 3; continue; }
        if (*p == '}') { p++; continue; }
        if (*p == '@') {
            const char *a = ++p;
            while (p < e && ident_char(*p)) p++;
            int nl = p - a;
            const char *ps = p;
            while (p < e && *p != '{' && *p != ';') p++;
            if (p >= e) break;
            if (*p == ';') { p++; continue; }
            const char *pe = p;
            const char *body = p + 1, *end = skip_block(body, e);
            if (depth < 3 && ieq(a, nl, "media")) {
                if (media_ok(ps, pe))
                    parse_sheet(pg, body, end - 1, origin, order, depth + 1);
            } else if (depth < 3 && (ieq(a, nl, "supports") || ieq(a, nl, "layer") || ieq(a, nl, "container"))) {
                if (!ieq(a, nl, "container"))
                    parse_sheet(pg, body, end - 1, origin, order, depth + 1);
            }
            p = end;
            continue;
        }
        const char *sel = p;
        while (p < e && *p != '{') {
            if (*p == '"' || *p == '\'') { char q = *p++; while (p < e && *p != q) p++; }
            p++;
        }
        if (p >= e) break;
        const char *sel_end = p;
        const char *body = p + 1, *end = skip_block(body, e);
        int nd;
        struct decl *d = parse_decls(pg, body, end - 1, &nd);
        if (d) {
            const char *s = sel;
            while (s < sel_end) {
                const char *c = s;
                int depth2 = 0;
                while (c < sel_end && (depth2 || *c != ',')) {
                    if (*c == '(' || *c == '[') depth2++;
                    if (*c == ')' || *c == ']') depth2--;
                    c++;
                }
                add_selector(pg, s, c, d, nd, origin, (*order)++);
                s = c + 1;
            }
        }
        p = end;
    }
}

/* ---- matching ---- */

static int is_elem(const struct node *n) { return n && n->type == 0; }

static struct node *prev_elem(struct node *n)
{
    for (n = n->prev; n; n = n->prev)
        if (n->type == 0) return n;
    return NULL;
}

static int has_class(const char *cl, const char *c, int cn)
{
    while (*cl) {
        while (is_space(*cl)) cl++;
        const char *e = cl;
        while (*e && !is_space(*e)) e++;
        if (e - cl == cn && !strncmp(cl, c, cn)) return 1;
        cl = e;
    }
    return 0;
}

static int match_simple(struct node *n, const struct simple *s)
{
    if (s->tag) {
        if (n->tag == TG_OTHER) {
            if (!n->text || (int)strlen(n->text) != s->tagn || strncmp(n->text, s->tagname, s->tagn)) return 0;
        } else {
            extern int css_tag_is(int tag, const char *name, int len);
            if (!css_tag_is(n->tag, s->tagname, s->tagn)) return 0;
        }
    }
    if (s->id) {
        const char *id = attr_val(n, "id");
        if (!id || (int)strlen(id) != s->idn || strncmp(id, s->id, s->idn)) return 0;
    }
    if (s->ncls) {
        const char *cl = attr_val(n, "class");
        if (!cl) return 0;
        for (int i = 0; i < s->ncls; i++)
            if (!has_class(cl, s->cls[i], s->clsn[i])) return 0;
    }
    for (int i = 0; i < s->nattr; i++) {
        char name[32];
        int nl = s->attr[i].nl < 31 ? s->attr[i].nl : 31;
        for (int k = 0; k < nl; k++) name[k] = lc(s->attr[i].n[k]);
        name[nl] = 0;
        const char *v = attr_val(n, name);
        if (!v) return 0;
        int vl = strlen(v), cl = s->attr[i].vl;
        const char *want = s->attr[i].v;
        switch (s->attr[i].op) {
        case '=': if (vl != cl || strncmp(v, want, cl)) return 0; break;
        case '~': if (!has_class(v, want, cl)) return 0; break;
        case '^': if (vl < cl || strncmp(v, want, cl)) return 0; break;
        case '$': if (vl < cl || strncmp(v + vl - cl, want, cl)) return 0; break;
        case '*': { int f = 0; for (int k = 0; k + cl <= vl; k++) if (!strncmp(v + k, want, cl)) { f = 1; break; } if (!f) return 0; break; }
        case '|': if (strncmp(v, want, cl) || (vl > cl && v[cl] != '-')) return 0; break;
        }
    }
    if (s->pseudo & PS_ROOT) if (n->tag != TG_HTML) return 0;
    if (s->pseudo & PS_LINK) if (n->tag != TG_A || !attr_val(n, "href")) return 0;
    if (s->pseudo & PS_FIRST) if (prev_elem(n)) return 0;
    if (s->pseudo & PS_LAST) { for (struct node *x = n->next; x; x = x->next) if (x->type == 0) return 0; }
    if (s->pseudo & PS_EMPTY) { for (struct node *x = n->first; x; x = x->next) if (x->type == 0 || x->tlen) return 0; }
    if (s->has_nth) {
        int idx = 1;
        for (struct node *x = prev_elem(n); x; x = prev_elem(x)) idx++;
        int d = idx - s->nth_b;
        if (s->nth_a == 0 ? d != 0 : (d % s->nth_a != 0 || d / s->nth_a < 0)) return 0;
    }
    if (s->neg && match_simple(n, s->neg)) return 0;
    return 1;
}

static int match_from(const struct rule *r, int i, struct node *n)
{
    if (!match_simple(n, &r->sel[i]))
        return 0;
    if (i == 0)
        return 1;
    struct node *a;
    switch (r->sel[i].comb) {
    case '>':
        return is_elem(n->parent) && match_from(r, i - 1, n->parent);
    case '+':
        a = prev_elem(n);
        return a && match_from(r, i - 1, a);
    case '~':
        for (a = prev_elem(n); a; a = prev_elem(a))
            if (match_from(r, i - 1, a)) return 1;
        return 0;
    default:
        for (a = n->parent; a; a = a->parent)
            if (match_from(r, i - 1, a)) return 1;
        return 0;
    }
}

/* ---- computed values ---- */

static struct len auto_len(void) { return (struct len){0, L_AUTO}; }

static void init_root_style(struct style *s)
{
    memset(s, 0, sizeof *s);
    s->display = D_INLINE;
    s->color = 0xFF000000;
    s->font_size = 16;
    s->opacity = 255;
    s->list_style = LS_DISC;
    s->flex_dir = FD_ROW;
    s->align_items = AI_STRETCH;
    s->align_self = AI_AUTO;
    s->shrink = 1000;
    for (int i = 0; i < 4; i++) {
        s->margin[i] = s->padding[i] = (struct len){0, L_PX};
    }
    s->width = s->height = s->min_w = s->max_w = s->min_h = s->max_h = s->flex_basis = auto_len();
}

/* Starts a node's style: inherited properties from the parent, the rest reset. */
static void inherit_style(struct style *s, const struct style *par)
{
    init_root_style(s);
    if (!par)
        return;
    s->color = par->color;
    s->font_size = par->font_size;
    s->line_height = par->line_height;
    s->bold = par->bold;
    s->italic = par->italic;
    s->mono = par->mono;
    s->text_align = par->text_align;
    s->white_space = par->white_space;
    s->text_transform = par->text_transform;
    s->list_style = par->list_style;
    s->visibility = par->visibility;
    s->vars = par->vars;
    s->nvars = par->nvars;
}

static const struct var *find_var(const struct style *s, const char *name, int nl)
{
    for (int i = s->nvars - 1; i >= 0; i--)
        if (s->vars[i].nlen == nl && !strncmp(s->vars[i].name, name, nl))
            return &s->vars[i];
    return NULL;
}

/* Expands var(--x, fallback) into out; returns the new length. */
static int expand_vars(const struct style *s, const char *v, int n, char *out, int cap, int depth)
{
    int o = 0;
    for (int i = 0; i < n && o < cap - 1;) {
        if (i + 4 <= n && !strncmp(v + i, "var(", 4) && depth < 6) {
            int j = i + 4, pd = 1;
            while (j < n && pd) { if (v[j] == '(') pd++; if (v[j] == ')') pd--; j++; }
            const char *a = v + i + 4, *e = v + j - 1;
            const char *comma = a;
            int d2 = 0;
            while (comma < e && !(*comma == ',' && !d2)) { if (*comma == '(') d2++; if (*comma == ')') d2--; comma++; }
            const char *ne = comma;
            while (ne > a && is_space(ne[-1])) ne--;
            while (a < ne && is_space(*a)) a++;
            const struct var *var = find_var(s, a, ne - a);
            if (var)
                o += expand_vars(s, var->val, var->vlen, out + o, cap - o, depth + 1);
            else if (comma < e)
                o += expand_vars(s, comma + 1, e - comma - 1, out + o, cap - o, depth + 1);
            i = j;
        } else {
            out[o++] = v[i++];
        }
    }
    out[o] = 0;
    return o;
}

static int font_size_of(const char *v, int n, int parent_px)
{
    static const struct { const char *k; int px; } KW[] = {
        {"xx-small", 9}, {"x-small", 10}, {"small", 13}, {"medium", 16}, {"large", 18}, {"x-large", 24}, {"xx-large", 32},
        {"xxx-large", 48},
    };
    for (int i = 0; i < 8; i++)
        if (ieq(v, n, KW[i].k)) return KW[i].px;
    if (ieq(v, n, "smaller")) return parent_px * 5 / 6;
    if (ieq(v, n, "larger")) return parent_px * 6 / 5;
    struct len l;
    if (!css_parse_len(v, n, parent_px, &l)) return -1;
    if (l.unit == L_PCT) return parent_px * l.v / 1000;
    if (l.unit == L_PX) return l.v;
    return -1;
}

static void set_color(struct style *s, u32 *dst, const char *v, int n)
{
    u32 c;
    if (parse_color(v, n, s->color, &c))
        *dst = c;
}

static int set_len(struct style *s, struct len *dst, const char *v, int n)
{
    struct len l;
    if (css_parse_len(v, n, s->font_size, &l)) {
        *dst = l;
        return 1;
    }
    return 0;
}

static int bw_of(struct style *s, const char *v, int n)
{
    if (ieq(v, n, "thin")) return 1;
    if (ieq(v, n, "medium")) return 3;
    if (ieq(v, n, "thick")) return 5;
    struct len l;
    if (css_parse_len(v, n, s->font_size, &l) && l.unit == L_PX) return l.v;
    return -1;
}

static int bstyle_of(const char *v, int n)
{
    if (ieq(v, n, "none") || ieq(v, n, "hidden")) return BS_NONE;
    if (ieq(v, n, "dashed")) return BS_DASHED;
    if (ieq(v, n, "dotted")) return BS_DOTTED;
    if (ieq(v, n, "solid") || ieq(v, n, "double") || ieq(v, n, "groove") || ieq(v, n, "ridge") || ieq(v, n, "inset") ||
        ieq(v, n, "outset")) return BS_SOLID;
    return -1;
}

static void border_side(struct style *s, int side, const char *v, int n)
{
    const char *e = v + n, *t;
    int tl, got_w = 0, got_s = 0, got_c = 0;
    u32 c;
    int w = 3, bs = BS_NONE;
    u32 col = s->color;
    while (v < e) {
        v = next_tok(v, e, &t, &tl);
        if (!tl) break;
        int x;
        if ((x = bstyle_of(t, tl)) >= 0) { bs = x; got_s = 1; }
        else if ((x = bw_of(s, t, tl)) >= 0) { w = x; got_w = 1; }
        else if (parse_color(t, tl, s->color, &c)) { col = c; got_c = 1; }
    }
    (void)got_w; (void)got_c;
    if (!got_s) bs = BS_NONE;
    if (side < 0) {
        for (int i = 0; i < 4; i++) { s->border_w[i] = bs == BS_NONE ? 0 : w; s->border_style[i] = bs; s->border_color[i] = col; }
    } else {
        s->border_w[side] = bs == BS_NONE ? 0 : w;
        s->border_style[side] = bs;
        s->border_color[side] = col;
    }
}

static void four_values(struct style *s, const char *v, int n, struct len out[4])
{
    const char *e = v + n, *t;
    int tl, k = 0;
    struct len l[4];
    while (v < e && k < 4) {
        v = next_tok(v, e, &t, &tl);
        if (!tl) break;
        if (!css_parse_len(t, tl, s->font_size, &l[k])) return;
        k++;
    }
    if (!k) return;
    out[0] = l[0];
    out[1] = k > 1 ? l[1] : l[0];
    out[2] = k > 2 ? l[2] : l[0];
    out[3] = k > 3 ? l[3] : out[1];
}

static void parse_background(struct style *s, struct node *nd, const char *v, int n)
{
    const char *e = v + n, *t;
    int tl;
    u32 c;
    s->bg = 0;
    s->bg_kind = BG_NONE;
    s->bg_img = 0;
    while (v < e) {
        v = next_tok(v, e, &t, &tl);
        if (!tl) break;
        if (t[0] == ',') break;
        if (tl > 4 && !strncmp(t, "url(", 4)) {
            const char *a = t + 4, *z = t + tl - 1;
            while (a < z && (is_space(*a) || *a == '"' || *a == '\'')) a++;
            while (z > a && (is_space(z[-1]) || z[-1] == '"' || z[-1] == '\'')) z--;
            char raw[512], full[512];
            int m = z - a < 511 ? z - a : 511;
            memcpy(raw, a, m);
            raw[m] = 0;
            if (strncmp(raw, "data:", 5) && g_page->nres < 80) {
                url_resolve(g_sheet_url ? g_sheet_url : g_page->url, raw, full, sizeof full);
                int ri = add_resource(g_page, full, R_IMAGE);
                if (ri >= 0) s->bg_img = ri + 1;
            }
        } else if (tl > 16 && (!strncmp(t, "linear-gradient(", 16))) {
            const char *a = t + 16, *z = t + tl - 1;
            s->bg_kind = BG_GRAD_V;
            u32 first = 0, last = 0;
            int nc = 0, rev = 0;
            const char *q = a;
            while (q < z) {
                const char *ct;
                int cl;
                q = next_tok(q, z, &ct, &cl);
                if (!cl) break;
                if (ct[0] == ',') continue;
                if (ieq(ct, cl, "to") ) continue;
                if (ieq(ct, cl, "right") || ieq(ct, cl, "left")) { s->bg_kind = BG_GRAD_H; rev = ieq(ct, cl, "left"); continue; }
                if (ieq(ct, cl, "top")) { rev = 1; continue; }
                if (ieq(ct, cl, "bottom")) continue;
                if (cl > 3 && ct[cl - 1] == 'g' && ct[cl - 2] == 'e' && ct[cl - 3] == 'd') {      /* 90deg etc. */
                    int x; const char *rest;
                    if (parse_num(ct, cl, &x, &rest)) {
                        int deg = (x / 1000) % 360;
                        if (deg == 90 || deg == 270) { s->bg_kind = BG_GRAD_H; rev = deg == 270; }
                        else if (deg == 0) rev = 1;
                    }
                    continue;
                }
                if (parse_color(ct, cl, s->color, &c)) {
                    if (!nc++) first = c;
                    last = c;
                }
            }
            if (nc >= 2) {
                s->bg = rev ? last : first;
                s->bg2 = rev ? first : last;
            } else {
                s->bg_kind = BG_NONE;
                if (nc) s->bg = first;
            }
        } else if (parse_color(t, tl, s->color, &c)) {
            s->bg = c;
        }
    }
}

static void apply(struct style *s, struct node *nd, const struct style *par, int prop, const char *v, int n)
{
    struct len l;
    int x;
    switch (prop) {
    case P_DISPLAY:
        if (ieq(v, n, "none")) s->display = D_NONE;
        else if (ieq(v, n, "block") || ieq(v, n, "flow-root") || ieq(v, n, "contents")) s->display = D_BLOCK;
        else if (ieq(v, n, "inline")) s->display = D_INLINE;
        else if (ieq(v, n, "inline-block")) s->display = D_INLINE_BLOCK;
        else if (ieq(v, n, "list-item")) s->display = D_LIST_ITEM;
        else if (ieq(v, n, "flex")) s->display = D_FLEX;
        else if (ieq(v, n, "inline-flex")) s->display = D_INLINE_FLEX;
        else if (ieq(v, n, "grid") || ieq(v, n, "inline-grid")) s->display = D_GRID;
        else if (ieq(v, n, "table") || ieq(v, n, "inline-table")) s->display = D_TABLE;
        else if (ieq(v, n, "table-row")) s->display = D_ROW;
        else if (ieq(v, n, "table-cell")) s->display = D_CELL;
        else if (ieq(v, n, "table-row-group") || ieq(v, n, "table-header-group") || ieq(v, n, "table-footer-group")) s->display = D_ROW_GROUP;
        else if (ieq(v, n, "table-caption")) s->display = D_CAPTION;
        break;
    case P_POSITION:
        s->position = ieq(v, n, "relative") || ieq(v, n, "sticky") ? POS_RELATIVE : ieq(v, n, "absolute") ? POS_ABSOLUTE :
                      ieq(v, n, "fixed") ? POS_FIXED : POS_STATIC;
        break;
    case P_FLOAT: s->float_ = ieq(v, n, "left") ? FL_LEFT : ieq(v, n, "right") ? FL_RIGHT : FL_NONE; break;
    case P_CLEAR: s->clear = ieq(v, n, "left") ? 1 : ieq(v, n, "right") ? 2 : ieq(v, n, "both") ? 3 : 0; break;
    case P_COLOR: set_color(s, &s->color, v, n); s->color |= s->color & 0xFF000000 ? 0 : 0xFF000000; break;
    case P_BACKGROUND: parse_background(s, nd, v, n); break;
    case P_BG_COLOR: set_color(s, &s->bg, v, n); break;
    case P_BG_IMAGE: parse_background(s, nd, v, n); break;
    case P_BG_SIZE: s->bg_size = ieq(v, n, "cover") ? 1 : ieq(v, n, "contain") ? 2 : 0; break;
    case P_BG_POS:
        if (contains(v, n, "center")) s->bg_px = s->bg_py = 50;
        break;
    case P_FONT_SIZE:
        x = font_size_of(v, n, par ? par->font_size : 16);
        if (x >= 0) s->font_size = x;
        break;
    case P_FONT_WEIGHT:
        if (ieq(v, n, "bold") || ieq(v, n, "bolder")) s->bold = 1;
        else if (ieq(v, n, "normal") || ieq(v, n, "lighter")) s->bold = 0;
        else if (css_parse_len(v, n, 16, &l) && l.unit == L_PX) s->bold = l.v >= 600;
        break;
    case P_FONT_STYLE: s->italic = ieq(v, n, "italic") || ieq(v, n, "oblique"); break;
    case P_FONT_FAMILY: {
        char tmp[200];
        int m = n < 199 ? n : 199;
        for (int i = 0; i < m; i++) tmp[i] = lc(v[i]);
        tmp[m] = 0;
        const char *first = tmp;
        const char *comma = strchr(tmp, ',');
        char head[100];
        int hl = comma ? comma - first : m;
        if (hl > 99) hl = 99;
        memcpy(head, first, hl);
        head[hl] = 0;
        s->mono = strstr(head, "mono") || strstr(head, "courier") || strstr(head, "consolas") || strstr(head, "menlo");
        break;
    }
    case P_FONT: {
        const char *e = v + n, *t, *p = v;
        int tl;
        s->bold = 0;
        s->italic = 0;
        while (p < e) {
            p = next_tok(p, e, &t, &tl);
            if (!tl) break;
            if (ieq(t, tl, "bold") || ieq(t, tl, "bolder")) s->bold = 1;
            else if (ieq(t, tl, "italic") || ieq(t, tl, "oblique")) s->italic = 1;
            else if (ieq(t, tl, "normal")) {}
            else if ((t[0] >= '0' && t[0] <= '9') || t[0] == '.') {
                const char *slash = memchr(t, '/', tl);
                int sl = slash ? slash - t : tl;
                int fs = font_size_of(t, sl, par ? par->font_size : 16);
                if (fs >= 0) {
                    s->font_size = fs;
                    if (slash) apply(s, nd, par, P_LINE_HEIGHT, slash + 1, t + tl - slash - 1);
                    else if (sl == tl && css_parse_len(t, tl, 16, &l) && l.unit == L_PX && tl <= 3 && ieq(t + tl - 1, 1, "0")) {
                        /* a bare 400/700 weight */
                    }
                    apply(s, nd, par, P_FONT_FAMILY, p, e - p);
                    return;
                } else if (t[0] >= '1' && t[0] <= '9' && tl == 3) {
                    s->bold = (t[0] - '0') >= 6;
                }
            } else if (font_size_of(t, tl, par ? par->font_size : 16) >= 0 && !(t[0] == '"' || t[0] == '\'')) {
                s->font_size = font_size_of(t, tl, par ? par->font_size : 16);
                apply(s, nd, par, P_FONT_FAMILY, p, e - p);
                return;
            }
        }
        break;
    }
    case P_TEXT_ALIGN:
        s->text_align = ieq(v, n, "center") ? TA_CENTER : ieq(v, n, "right") || ieq(v, n, "end") ? TA_RIGHT :
                        ieq(v, n, "justify") ? TA_JUSTIFY : TA_LEFT;
        break;
    case P_TEXT_DECOR:
        s->decor = 0;
        if (contains(v, n, "underline")) s->decor |= DEC_UNDERLINE;
        if (contains(v, n, "line-through")) s->decor |= DEC_STRIKE;
        break;
    case P_TEXT_TRANSFORM:
        s->text_transform = ieq(v, n, "uppercase") ? TT_UPPER : ieq(v, n, "lowercase") ? TT_LOWER :
                            ieq(v, n, "capitalize") ? TT_CAP : TT_NONE;
        break;
    case P_LINE_HEIGHT:
        if (ieq(v, n, "normal")) { s->line_height = 0; break; }
        if (css_parse_len(v, n, s->font_size, &l)) {
            const char *rest;
            int xx;
            if (l.unit == L_PX && parse_num(v, n, &xx, &rest) && rest == v + n) s->line_height = -xx;     /* unitless */
            else if (l.unit == L_PX) s->line_height = l.v > 0 ? l.v : 0;
            else if (l.unit == L_PCT) s->line_height = s->font_size * l.v / 1000;
        }
        break;
    case P_WHITE_SPACE:
        s->white_space = ieq(v, n, "pre") ? WS_PRE : ieq(v, n, "nowrap") ? WS_NOWRAP : ieq(v, n, "pre-wrap") ? WS_PRE_WRAP :
                         ieq(v, n, "pre-line") ? WS_PRE_LINE : WS_NORMAL;
        break;
    case P_VISIBILITY: s->visibility = !ieq(v, n, "visible"); break;
    case P_WIDTH: set_len(s, &s->width, v, n); break;
    case P_HEIGHT: set_len(s, &s->height, v, n); break;
    case P_MIN_W: set_len(s, &s->min_w, v, n); break;
    case P_MAX_W: set_len(s, &s->max_w, v, n); break;
    case P_MIN_H: set_len(s, &s->min_h, v, n); break;
    case P_MAX_H: set_len(s, &s->max_h, v, n); break;
    case P_MARGIN: four_values(s, v, n, s->margin); break;
    case P_MARGIN_T: case P_MARGIN_R: case P_MARGIN_B: case P_MARGIN_L: set_len(s, &s->margin[prop - P_MARGIN_T], v, n); break;
    case P_PADDING: four_values(s, v, n, s->padding); break;
    case P_PADDING_T: case P_PADDING_R: case P_PADDING_B: case P_PADDING_L: set_len(s, &s->padding[prop - P_PADDING_T], v, n); break;
    case P_BORDER: border_side(s, -1, v, n); break;
    case P_BORDER_T: case P_BORDER_R: case P_BORDER_B: case P_BORDER_L: border_side(s, prop - P_BORDER_T, v, n); break;
    case P_BORDER_W: {
        const char *e = v + n, *t, *p = v;
        int tl, k = 0, w[4];
        while (p < e && k < 4) { p = next_tok(p, e, &t, &tl); if (!tl) break; w[k] = bw_of(s, t, tl); if (w[k] >= 0) k++; }
        if (k) { s->border_w[0] = w[0]; s->border_w[1] = k > 1 ? w[1] : w[0]; s->border_w[2] = k > 2 ? w[2] : w[0]; s->border_w[3] = k > 3 ? w[3] : s->border_w[1]; }
        for (int i = 0; i < 4; i++) if (s->border_style[i] == BS_NONE && s->border_w[i]) s->border_style[i] = BS_SOLID;
        break;
    }
    case P_BORDER_STYLE: {
        int bs = bstyle_of(v, n);
        const char *sp = memchr(v, ' ', n);
        if (!sp && bs >= 0) for (int i = 0; i < 4; i++) { s->border_style[i] = bs; if (bs == BS_NONE) s->border_w[i] = 0; }
        else if (sp) { bs = bstyle_of(v, sp - v); if (bs >= 0) for (int i = 0; i < 4; i++) s->border_style[i] = bs; }
        break;
    }
    case P_BORDER_COLOR: {
        u32 c;
        const char *e = v + n, *t, *p = v;
        int tl, k = 0;
        u32 cols[4];
        while (p < e && k < 4) { p = next_tok(p, e, &t, &tl); if (!tl) break; if (parse_color(t, tl, s->color, &c)) cols[k++] = c; }
        if (k) { for (int i = 0; i < 4; i++) s->border_color[i] = cols[k == 1 ? 0 : k == 2 ? i & 1 : k == 3 ? (i == 3 ? 1 : i) : i]; }
        break;
    }
    case P_BORDER_TW: case P_BORDER_RW: case P_BORDER_BW: case P_BORDER_LW:
        x = bw_of(s, v, n);
        if (x >= 0) { s->border_w[prop - P_BORDER_TW] = x; if (s->border_style[prop - P_BORDER_TW] == BS_NONE && x) s->border_style[prop - P_BORDER_TW] = BS_SOLID; }
        break;
    case P_RADIUS:
        if (css_parse_len(v, n, s->font_size, &l)) s->radius = l.unit == L_PX ? l.v : l.unit == L_PCT && l.v >= 500 ? 9999 : l.unit == L_PCT ? 12 : 0;
        break;
    case P_BOX_SIZING: s->box_sizing = ieq(v, n, "border-box"); break;
    case P_FLEX: {
        if (ieq(v, n, "none")) { s->grow = 0; s->shrink = 0; s->flex_basis = auto_len(); break; }
        if (ieq(v, n, "auto")) { s->grow = 1000; s->shrink = 1000; s->flex_basis = auto_len(); break; }
        const char *e = v + n, *t, *p = v;
        int tl, k = 0;
        s->grow = 1000; s->shrink = 1000; s->flex_basis = (struct len){0, L_PCT};
        while (p < e) {
            p = next_tok(p, e, &t, &tl);
            if (!tl) break;
            int xx; const char *rest;
            if (parse_num(t, tl, &xx, &rest) && rest == t + tl) { if (k == 0) s->grow = xx; else if (k == 1) s->shrink = xx; else s->flex_basis = (struct len){0, L_PX}; k++; }
            else css_parse_len(t, tl, s->font_size, &s->flex_basis);
        }
        break;
    }
    case P_FLEX_DIR:
        s->flex_dir = ieq(v, n, "column") ? FD_COL : ieq(v, n, "column-reverse") ? FD_COL_REV : ieq(v, n, "row-reverse") ? FD_ROW_REV : FD_ROW;
        break;
    case P_FLEX_WRAP: s->flex_wrap = !ieq(v, n, "nowrap"); break;
    case P_FLEX_FLOW: s->flex_wrap = contains(v, n, "wrap") && !contains(v, n, "nowrap");
        s->flex_dir = contains(v, n, "column-reverse") ? FD_COL_REV : contains(v, n, "column") ? FD_COL : contains(v, n, "row-reverse") ? FD_ROW_REV : FD_ROW;
        break;
    case P_GROW: { int xx; const char *rest; if (parse_num(v, n, &xx, &rest)) s->grow = xx; break; }
    case P_SHRINK: { int xx; const char *rest; if (parse_num(v, n, &xx, &rest)) s->shrink = xx; break; }
    case P_BASIS: set_len(s, &s->flex_basis, v, n); break;
    case P_JUSTIFY:
        s->justify = ieq(v, n, "center") ? JC_CENTER : ieq(v, n, "flex-end") || ieq(v, n, "end") || ieq(v, n, "right") ? JC_END :
                     ieq(v, n, "space-between") ? JC_BETWEEN : ieq(v, n, "space-around") ? JC_AROUND :
                     ieq(v, n, "space-evenly") ? JC_EVENLY : JC_START;
        break;
    case P_ALIGN_ITEMS: case P_ALIGN_SELF: {
        int a = ieq(v, n, "center") ? AI_CENTER : ieq(v, n, "flex-end") || ieq(v, n, "end") ? AI_END :
                ieq(v, n, "flex-start") || ieq(v, n, "start") ? AI_START : ieq(v, n, "baseline") ? AI_BASELINE :
                ieq(v, n, "stretch") ? AI_STRETCH : AI_AUTO;
        if (prop == P_ALIGN_ITEMS) s->align_items = a == AI_AUTO ? AI_STRETCH : a; else s->align_self = a;
        break;
    }
    case P_GAP: {
        const char *e = v + n, *t, *p = v;
        int tl;
        struct len a, b;
        p = next_tok(p, e, &t, &tl);
        if (css_parse_len(t, tl, s->font_size, &a)) {
            b = a;
            p = next_tok(p, e, &t, &tl);
            if (tl) css_parse_len(t, tl, s->font_size, &b);
            s->gap_r = a.unit == L_PX ? a.v : 0;
            s->gap_c = b.unit == L_PX ? b.v : 0;
        }
        break;
    }
    case P_ROW_GAP: if (css_parse_len(v, n, s->font_size, &l) && l.unit == L_PX) s->gap_r = l.v; break;
    case P_COL_GAP: if (css_parse_len(v, n, s->font_size, &l) && l.unit == L_PX) s->gap_c = l.v; break;
    case P_LIST_STYLE:
        s->list_style = contains(v, n, "none") ? LS_NONE : contains(v, n, "decimal") ? LS_DECIMAL : contains(v, n, "circle") ? LS_CIRCLE :
                        contains(v, n, "square") ? LS_SQUARE : contains(v, n, "disc") ? LS_DISC : contains(v, n, "alpha") ? LS_ALPHA :
                        contains(v, n, "roman") ? LS_ROMAN : s->list_style;
        break;
    case P_VALIGN:
        s->vertical_align = ieq(v, n, "middle") ? VA_MIDDLE : ieq(v, n, "top") || ieq(v, n, "text-top") ? VA_TOP :
                            ieq(v, n, "bottom") || ieq(v, n, "text-bottom") ? VA_BOTTOM : VA_BASELINE;
        break;
    case P_OVERFLOW: s->overflow = ieq(v, n, "visible") ? OV_VISIBLE : OV_HIDDEN; break;
    case P_OPACITY: { int xx; const char *rest; if (parse_num(v, n, &xx, &rest)) s->opacity = xx >= 1000 ? 255 : xx <= 0 ? 0 : xx * 255 / 1000; break; }
    case P_ORDER: { int xx; const char *rest; if (parse_num(v, n, &xx, &rest)) s->order = xx / 1000; break; }
    case P_BSPACING: {
        const char *e = v + n, *t, *p = v;
        int tl;
        struct len a, b;
        p = next_tok(p, e, &t, &tl);
        if (css_parse_len(t, tl, s->font_size, &a) && a.unit == L_PX) {
            b = a;
            p = next_tok(p, e, &t, &tl);
            if (tl) css_parse_len(t, tl, s->font_size, &b);
            s->gap_c = a.v;
            s->gap_r = b.unit == L_PX ? b.v : a.v;
        }
        break;
    }
    case P_BCOLLAPSE: s->collapse = ieq(v, n, "collapse"); break;
    case P_GRID_COLS: {
        const char *e = v + n, *t, *p = v;
        int tl;
        s->grid_cols = 0;
        while (p < e && s->grid_cols < 12 && s->grid_cols >= 0) {
            p = next_tok(p, e, &t, &tl);
            if (!tl) break;
            if (tl > 7 && !strncmp(t, "repeat(", 7)) {
                const char *a = t + 7, *z = t + tl - 1, *comma = memchr(a, ',', z - a);
                if (!comma) continue;
                struct len track = {0, L_FR};
                int cnt;
                const char *rest;
                int xx;
                const char *tr = comma + 1;
                int auto_fit = ieq(a, comma - a, "auto-fit") || ieq(a, comma - a, "auto-fill");
                const char *mm = find_in(tr, z - tr, "minmax(");
                if (mm && mm < z) {
                    const char *mc = mm + 7;
                    struct len mn;
                    if (css_parse_len(mc, strchr(mc, ',') - mc, s->font_size, &mn) && mn.unit == L_PX) track = mn;
                    else track = (struct len){100, L_PX};
                } else {
                    css_parse_len(tr, z - tr, s->font_size, &track);
                }
                if (auto_fit) {
                    s->grid_cols = -1;
                    s->grid_col[0] = track.unit == L_PX ? track : (struct len){100, L_PX};
                } else if (parse_num(a, comma - a, &xx, &rest) && xx / 1000 > 0) {
                    cnt = xx / 1000;
                    for (int i = 0; i < cnt && s->grid_cols < 12; i++) s->grid_col[s->grid_cols++] = track;
                }
            } else {
                struct len l2;
                if (tl > 7 && !strncmp(t, "minmax(", 7)) l2 = (struct len){0, L_FR};
                else if (!css_parse_len(t, tl, s->font_size, &l2)) continue;
                if (l2.unit == L_AUTO) l2 = (struct len){1000, L_FR};
                s->grid_col[s->grid_cols++] = l2;
            }
        }
        break;
    }
    }
}

static void apply_decl(struct style *s, struct node *nd, const struct style *par, const struct decl *d)
{
    char buf[400];
    const char *v = d->val;
    int n = d->vlen;
    if (memchr(v, 'v', n) && contains(v, n, "var(")) {
        char *tmp = malloc(2048);
        if (!tmp) return;
        n = expand_vars(s, v, n, tmp, 2048, 0);
        if (n > 0) {
            if (n < (int)sizeof buf) {
                memcpy(buf, tmp, n);
                buf[n] = 0;
                free(tmp);
                v = buf;
            } else {
                v = arena_str(g_page, tmp, n);
                free(tmp);
            }
        } else {
            free(tmp);
            return;
        }
    }
    if (ieq(v, n, "inherit") && par) {
        struct style t = *s;
        inherit_style(&t, par);
        switch (d->prop) {
        case P_COLOR: s->color = par->color; break;
        case P_FONT_SIZE: s->font_size = par->font_size; break;
        case P_BG_COLOR: s->bg = par->bg; break;
        case P_TEXT_DECOR: s->decor = par->decor; break;
        default: break;
        }
        return;
    }
    if (ieq(v, n, "initial") || ieq(v, n, "unset") || ieq(v, n, "revert")) {
        if (d->prop == P_COLOR && par) s->color = par->color;
        return;
    }
    if (d->prop == P_VAR) return;
    apply(s, nd, par, d->prop, v, n);
}

static void collect_vars(struct page *pg, struct style *s, const struct style *par, const struct decl *ds, int nd)
{
    for (int k = 0; k < nd; k++) {
        if (ds[k].prop != P_VAR)
            continue;
        if (!s->vars || s->vars == (par ? par->vars : NULL)) {          /* copy-on-write the inherited table */
            struct var *nvars = arena_alloc(pg, MAXVARS * sizeof *nvars);
            if (!nvars)
                return;
            if (par && par->nvars)
                memcpy(nvars, par->vars, par->nvars * sizeof *nvars);
            s->vars = nvars;
            s->nvars = par ? par->nvars : 0;
        }
        if (s->nvars < MAXVARS)
            s->vars[s->nvars++] = (struct var){ds[k].name, ds[k].val, ds[k].nlen, ds[k].vlen};
    }
}

struct match {
    u32 key;
    const struct rule *r;
};

static void hints(struct node *n, struct style *s)
{
    u32 c;
    const char *v;
    struct len l;
    if ((v = attr_val(n, "bgcolor")) && parse_color(v, strlen(v), s->color, &c)) s->bg = c;
    if (n->tag == TG_TABLE || n->tag == TG_TD || n->tag == TG_TH || n->tag == TG_COL) {
        if ((v = attr_val(n, "width")) && css_parse_len(v, strlen(v), s->font_size, &l)) s->width = l;
        if (n->tag != TG_COL && (v = attr_val(n, "height")) && css_parse_len(v, strlen(v), s->font_size, &l)) s->height = l;
    }
    if (n->tag == TG_BODY) {
        if ((v = attr_val(n, "text")) && parse_color(v, strlen(v), s->color, &c)) s->color = c;
    }
    if ((v = attr_val(n, "align"))) {
        if (n->tag == TG_TABLE || n->tag == TG_IMG) {
            if (!strcmp(v, "center") && n->tag == TG_TABLE) s->margin[1] = s->margin[3] = (struct len){0, L_AUTO};
            else if (!strcmp(v, "right")) s->float_ = FL_RIGHT;
            else if (!strcmp(v, "left") && n->tag == TG_IMG) s->float_ = FL_LEFT;
        } else if (!strcmp(v, "center")) s->text_align = TA_CENTER;
        else if (!strcmp(v, "right")) s->text_align = TA_RIGHT;
        else if (!strcmp(v, "left")) s->text_align = TA_LEFT;
    }
    if (attr_val(n, "nowrap")) s->white_space = WS_NOWRAP;
    if (n->tag == TG_TABLE) {
        if ((v = attr_val(n, "cellspacing"))) s->gap_c = s->gap_r = atoi(v);
        if ((v = attr_val(n, "border")) && atoi(v) > 0)
            for (int i = 0; i < 4; i++) { s->border_w[i] = atoi(v); s->border_style[i] = BS_SOLID; s->border_color[i] = 0xFF808080; }
    }
    if (n->tag == TG_TD || n->tag == TG_TH) {
        struct node *t = n->parent;
        while (t && t->tag != TG_TABLE) t = t->parent;
        if (t) {
            if ((v = attr_val(t, "cellpadding")))
                for (int i = 0; i < 4; i++) s->padding[i] = (struct len){atoi(v), L_PX};
            if ((v = attr_val(t, "border")) && atoi(v) > 0)
                for (int i = 0; i < 4; i++) { s->border_w[i] = 1; s->border_style[i] = BS_SOLID; s->border_color[i] = 0xFFA0A0A0; }
        }
    }
}

/* Text directly inside a flex/grid container becomes an anonymous block item. */
static void wrap_flex_text(struct page *pg, struct node *n)
{
    for (struct node *c = n->first; c; c = c->next) {
        if (c->type != 1) continue;
        int blank = 1;
        for (int i = 0; i < c->tlen; i++) if (!is_space(c->text[i])) blank = 0;
        if (blank) continue;
        struct node *w = arena_alloc(pg, sizeof *w);
        if (!w) return;
        w->type = 0; w->tag = TG_DIV; w->mincw = w->maxcw = -1;
        w->parent = n; w->prev = c->prev; w->next = c->next; w->first = w->last = c; w->link = c->link;
        if (c->prev) c->prev->next = w; else n->first = w;
        if (c->next) c->next->prev = w; else n->last = w;
        c->parent = w; c->prev = c->next = NULL;
        c = w;
    }
}

static void compute_node(struct page *pg, struct node *n, struct style *par)
{
    struct style *s = arena_alloc(pg, sizeof *s);
    if (!s)
        return;
    inherit_style(s, par);
    n->st = s;
    hints(n, s);
    static struct match cand[600];
    int nc = 0;
    for (int i = 0; i < pg->nrules && nc < 600; i++) {
        const struct rule *r = &pg->rules[i];
        if ((r->mask[0] & ~n->mask[0]) || (r->mask[1] & ~n->mask[1]))
            continue;
        if (match_from(r, r->nsel - 1, n)) {
            cand[nc].key = r->key;
            cand[nc].r = r;
            nc++;
        }
    }
    for (int i = 1; i < nc; i++) {                                  /* insertion sort by (origin, specificity, order) */
        struct match m = cand[i];
        int j = i - 1;
        while (j >= 0 && cand[j].key > m.key) { cand[j + 1] = cand[j]; j--; }
        cand[j + 1] = m;
    }
    /* custom properties first, so var() works anywhere in the declarations */
    struct decl *id = NULL;
    int ninl = 0;
    const char *style_attr = attr_val(n, "style");
    if (style_attr)
        id = parse_decls(pg, style_attr, style_attr + strlen(style_attr), &ninl);
    for (int i = 0; i < nc; i++)
        collect_vars(pg, s, par, cand[i].r->d, cand[i].r->nd);
    collect_vars(pg, s, par, id, ninl);
    for (int imp = 0; imp < 2; imp++) {
        for (int i = 0; i < nc; i++) {
            const struct decl *ds = cand[i].r->d;
            for (int k = 0; k < cand[i].r->nd; k++)
                if (ds[k].imp == imp) apply_decl(s, n, par, &ds[k]);
        }
        for (int k = 0; k < ninl; k++)
            if (id[k].imp == imp) apply_decl(s, n, par, &id[k]);
    }
    for (int i = 0; i < 4; i++)
        if (s->border_style[i] == BS_NONE)
            s->border_w[i] = 0;
    if (n->tag == TG_FONT) {
        const char *c = attr_val(n, "color");
        u32 col;
        if (c && parse_color(c, strlen(c), s->color, &col)) s->color = col;
        const char *sz = attr_val(n, "size");
        if (sz) {
            static const int fs[] = {10, 13, 16, 18, 24, 32, 48};
            int k = atoi(sz);
            if (k >= 1 && k <= 7) s->font_size = fs[k - 1];
        }
    }
    if (s->display == D_NONE || n->tag == TG_NOSCRIPT)
        s->display = D_NONE;
    if (s->position == POS_ABSOLUTE || s->position == POS_FIXED)
        s->display = D_NONE;
    /* floats and flex/grid items are blockified */
    if (s->float_ != FL_NONE && s->display == D_INLINE)
        s->display = D_BLOCK;
    if (s->display == D_FLEX || s->display == D_INLINE_FLEX || s->display == D_GRID)
        wrap_flex_text(pg, n);
    for (struct node *c = n->first; c; c = c->next) {
        if (c->type == 0)
            compute_node(pg, c, s);
        else
            c->st = s;
    }
}

int css_tag_is(int tag, const char *name, int len)
{
    static const char *const names[] = {
#define X(id, nm) nm,
        TAGS(X)
#undef X
    };
    return tag > 0 && tag < TG_COUNT && ieq(name, len, names[tag]);
}

static const char UA_CSS[] =
    "html,body,div,p,h1,h2,h3,h4,h5,h6,ul,ol,dl,dt,dd,form,pre,blockquote,address,center,nav,header,footer,main,"
    "section,article,aside,figure,figcaption,fieldset,details,summary,menu,dir,legend,hr,picture{display:block}"
    "head,script,style,title,meta,link,base,template,option,source,iframe,audio,object,colgroup,col,noscript{display:none}"
    "li{display:list-item}"
    "body{margin:8px}"
    "p,dl,blockquote{margin:16px 0}"
    "blockquote{margin:16px 40px}"
    "h1{font-size:32px;font-weight:bold;margin:21px 0}h2{font-size:24px;font-weight:bold;margin:20px 0}"
    "h3{font-size:19px;font-weight:bold;margin:19px 0}h4{font-weight:bold;margin:21px 0}"
    "h5{font-size:13px;font-weight:bold;margin:22px 0}h6{font-size:11px;font-weight:bold;margin:25px 0}"
    "ul,ol,menu,dir{margin:16px 0;padding-left:40px}ul{list-style-type:disc}ol{list-style-type:decimal}"
    "ul ul,ol ul{list-style-type:circle;margin:0}ul ul ul{list-style-type:square}ol ol,ul ol{margin:0}"
    "dd{margin-left:40px}"
    "pre{font-family:monospace;white-space:pre;margin:16px 0}"
    "code,kbd,samp,tt{font-family:monospace}"
    "b,strong,th{font-weight:bold}i,em,cite,var,dfn,address{font-style:italic}"
    "u,ins{text-decoration:underline}s,strike,del{text-decoration:line-through}"
    "small{font-size:13px}big{font-size:19px}sub,sup{font-size:13px}mark{background:#ff0;color:#000}"
    "a:link{color:#0000ee;text-decoration:underline}"
    "hr{margin:8px 0;border-top:1px solid #9a9a9a;height:0}"
    "table{display:table;border-spacing:2px}thead,tbody,tfoot{display:table-row-group}tr{display:table-row}"
    "td,th{display:table-cell;padding:1px}th{text-align:center}caption{display:table-caption;text-align:center}"
    "center{text-align:center}center table{text-align:left}"
    "img,input,button,select,textarea,svg,canvas,video{display:inline-block}"
    "button,input,select,textarea{font-size:14px}"
    "td,th{vertical-align:middle}"
    "input,select,textarea{background:#fff;border:1px solid #767676;padding:2px 4px;color:#000}"
    "button,input[type=submit],input[type=button],input[type=reset]{background:#efefef;border:1px solid #767676;"
    "border-radius:3px;padding:2px 8px;color:#000;text-align:center}"
    "input[type=checkbox],input[type=radio],input[type=image]{border:0;padding:0;background:transparent}"
    "input[type=hidden]{display:none}"
    "fieldset{margin:8px 2px;padding:6px 12px;border:2px groove #ccc}"
    "details>summary{display:list-item}"
    "[hidden]{display:none}";

void css_restyle(struct page *p, int viewport_w)
{
    if (!p->root)
        return;
    g_page = p;
    g_vw = viewport_w;
    g_vh = 560;
    css_free_rules(p);
    int order = 0;
    g_sheet_url = NULL;
    parse_sheet(p, UA_CSS, UA_CSS + sizeof UA_CSS - 1, 0, &order, 0);
    for (int i = 0; i < p->nsheets; i++) {
        const char *t = p->sheets[i].text;
        int len = p->sheets[i].len;
        g_sheet_url = NULL;
        if (p->sheets[i].res) {
            struct res *r = &p->res[p->sheets[i].res - 1];
            if (r->state != RS_DONE || !r->css)
                continue;
            t = r->css;
            len = r->css_len;
            g_sheet_url = r->url;
        }
        parse_sheet(p, t, t + len, 1, &order, 0);
    }
    g_sheet_url = NULL;
    compute_node(p, p->root, NULL);
    struct node *b = p->body ? p->body : p->root;
    u32 bg = b->st ? b->st->bg : 0;
    if (!(bg >> 24) && p->root->st)
        bg = p->root->st->bg;
    p->page_bg = bg >> 24 ? bg : 0xFFFFFFFF;
    p->styled = 1;
    p->dirty_style = 0;
}
