#include "html.h"

#define MARGIN    14
#define INDENT_PX 28
#define PAGE_FG   RGB(0x20, 0x24, 0x28)
#define LINK_FG   RGB(0x1a, 0x55, 0xc8)
#define LINK_HOT  RGB(0xc8, 0x3a, 0x1a)
#define HEAD_FG   RGB(0x14, 0x2a, 0x50)
#define MUTED_FG  RGB(0x70, 0x78, 0x80)

enum { IT_WORD, IT_BLOCK, IT_BR, IT_PARA, IT_HR, IT_BULLET, IT_GAP };
enum { F_BOLD = 1, F_ITALIC = 2, F_CODE = 4, F_PRE = 8, F_CENTER = 16, F_SP = 32 };

/* ---------------- storage ---------------- */

void page_free(struct page *p)
{
    free(p->pool);
    free(p->items);
    free(p->links);
    free(p->boxes);
    memset(p, 0, sizeof *p);
}

void page_clear(struct page *p)
{
    p->pool_len = 0;
    p->nitems = 0;
    p->nlinks = 0;
    p->nboxes = 0;
    p->title[0] = 0;
    p->layout_w = 0;
    p->height = 0;
}

static struct item *add_item(struct page *p, int type)
{
    if (p->nitems == p->cap_items) {
        int cap = p->cap_items ? p->cap_items * 2 : 512;
        struct item *n = realloc(p->items, cap * sizeof *n);
        if (!n)
            return NULL;
        p->items = n;
        p->cap_items = cap;
    }
    struct item *it = &p->items[p->nitems++];
    memset(it, 0, sizeof *it);
    it->type = type;
    return it;
}

static u32 pool_add(struct page *p, const char *s, int len)
{
    if (p->pool_len + len + 1 > p->pool_cap) {
        u32 cap = p->pool_cap ? p->pool_cap * 2 : 16384;
        while (cap < p->pool_len + len + 1)
            cap *= 2;
        char *n = realloc(p->pool, cap);
        if (!n)
            return 0;
        p->pool = n;
        p->pool_cap = cap;
    }
    u32 off = p->pool_len;
    memcpy(p->pool + off, s, len);
    p->pool_len += len;
    return off;
}

static int add_link(struct page *p, const char *href)
{
    if (!p->links && !(p->links = malloc(MAXLINK * sizeof *p->links)))
        return 0;
    if (p->nlinks >= MAXLINK)
        return 0;
    url_resolve(p->url, href, p->links[p->nlinks], sizeof p->links[0]);
    return ++p->nlinks;
}

/* ---------------- parser state ---------------- */

static struct {
    int bold, italic, code, pre, center, indent, scale;
    int link, in_title, want_space, first_cell, pre_first;
    u32 color;
    struct { int ordered, count; } list[8];
    int depth;
} S;

static char wb[512];
static int wl;

static void emit_word(struct page *p, const char *w, int len, u32 color_override)
{
    if (len <= 0)
        return;
    if (S.in_title) {
        int n = strlen(p->title);
        if (n && p->title[n - 1] != ' ' && n < (int)sizeof p->title - 1)
            p->title[n++] = ' ';
        for (int i = 0; i < len && n < (int)sizeof p->title - 1; i++)
            p->title[n++] = w[i];
        p->title[n] = 0;
        return;
    }
    struct item *it = add_item(p, IT_WORD);
    if (!it)
        return;
    it->flags = (S.bold ? F_BOLD : 0) | (S.italic ? F_ITALIC : 0) | (S.code ? F_CODE : 0) | (S.pre ? F_PRE : 0) |
                (S.center ? F_CENTER : 0) | (S.want_space ? F_SP : 0);
    it->scale = S.scale ? S.scale : 1;
    it->indent = S.indent;
    it->link = S.link;
    it->color = color_override ? color_override : S.color;
    it->len = len;
    it->off = pool_add(p, w, len);
    S.want_space = 0;
}

static void flush_word(struct page *p)
{
    emit_word(p, wb, wl, 0);
    wl = 0;
}

static void block(struct page *p, int type)
{
    flush_word(p);
    if (S.in_title)
        return;
    struct item *it = add_item(p, type);
    if (it) {
        it->indent = S.indent;
        it->flags = S.center ? F_CENTER : 0;
    }
    S.want_space = 0;
}

static void put_char(struct page *p, char c)
{
    if (S.pre) {
        if (S.pre_first && (c == '\n' || c == '\r')) {
            if (c == '\n')
                S.pre_first = 0;
            return;
        }
        S.pre_first = 0;
        if (c == '\n') {
            flush_word(p);
            block(p, IT_BR);
        } else if (c == '\t') {
            for (int i = 0; i < 4 && wl < (int)sizeof wb; i++)
                wb[wl++] = ' ';
        } else if (c != '\r' && wl < (int)sizeof wb) {
            wb[wl++] = c;
        }
        if (wl >= (int)sizeof wb)
            flush_word(p);
        return;
    }
    if (c == ' ' || c == '\n' || c == '\t' || c == '\r') {
        flush_word(p);
        S.want_space = 1;
        return;
    }
    if (wl < (int)sizeof wb)
        wb[wl++] = c;
    if (wl >= (int)sizeof wb)
        flush_word(p);
}

static void put_str(struct page *p, const char *s)
{
    for (; *s; s++)
        put_char(p, *s);
}

/* ---------------- entities and UTF-8 ---------------- */

static const char *entity_text(const char *name, int n, char *buf)
{
    static const struct { const char *name, *text; } tab[] = {
        {"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"}, {"nbsp", " "},
        {"copy", "(c)"}, {"reg", "(R)"}, {"trade", "TM"}, {"mdash", "--"}, {"ndash", "-"},
        {"hellip", "..."}, {"bull", "*"}, {"middot", "."}, {"laquo", "<<"}, {"raquo", ">>"},
        {"lsquo", "'"}, {"rsquo", "'"}, {"ldquo", "\""}, {"rdquo", "\""}, {"times", "x"},
        {"euro", "EUR"}, {"pound", "GBP"}, {"deg", "deg"}, {"larr", "<-"}, {"rarr", "->"},
    };
    if (name[0] == '#') {
        int cp = name[1] == 'x' || name[1] == 'X' ? 0 : atoi(name + 1);
        if (name[1] == 'x' || name[1] == 'X')
            for (int i = 2; i < n; i++) {
                char c = name[i] | 0x20;
                cp = cp * 16 + (c >= 'a' ? c - 'a' + 10 : c - '0');
            }
        extern const char *cp_text(int cp, char *buf);
        return cp_text(cp, buf);
    }
    for (u32 i = 0; i < sizeof tab / sizeof tab[0]; i++)
        if ((int)strlen(tab[i].name) == n && !strncmp(tab[i].name, name, n))
            return tab[i].text;
    return NULL;
}

const char *cp_text(int cp, char *buf)
{
    switch (cp) {
    case 0xA0: return " ";
    case 0xA9: return "(c)";
    case 0xAE: return "(R)";
    case 0xB7: case 0x2022: return "*";
    case 0xAB: return "<<";
    case 0xBB: return ">>";
    case 0x2013: return "-";
    case 0x2014: return "--";
    case 0x2018: case 0x2019: return "'";
    case 0x201C: case 0x201D: return "\"";
    case 0x2026: return "...";
    case 0x2122: return "TM";
    case 0x2192: return "->";
    case 0x2190: return "<-";
    }
    if (cp >= 32 && cp < 127) {
        buf[0] = cp;
        buf[1] = 0;
        return buf;
    }
    return "?";
}

/* ---------------- tags ---------------- */

struct attr {
    char name[16], value[512];
};

static const char *attr_get(struct attr *a, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(a[i].name, name))
            return a[i].value;
    return NULL;
}

static int is(const char *name, const char *a)
{
    return !strcmp(name, a);
}

static int one_of(const char *name, const char *list)       /* space-separated */
{
    int n = strlen(name);
    for (const char *p = list; *p;) {
        const char *e = strchr(p, ' ');
        int l = e ? e - p : (int)strlen(p);
        if (l == n && !strncmp(p, name, n))
            return 1;
        if (!e)
            break;
        p = e + 1;
    }
    return 0;
}

static void open_tag(struct page *p, const char *t, struct attr *a, int na)
{
    if (is(t, "title")) {
        flush_word(p);
        S.in_title = 1;
    } else if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) {
        block(p, IT_PARA);
        S.bold++;
        S.scale = t[1] == '1' ? 2 : 1;
        S.color = HEAD_FG;
    } else if (one_of(t, "p")) {
        block(p, IT_PARA);
    } else if (one_of(t, "div section article header footer nav main aside form fieldset address figure figcaption dl dd dt tr noscript")) {
        block(p, IT_BLOCK);
        if (is(t, "tr"))
            S.first_cell = 1;
    } else if (is(t, "br")) {
        block(p, IT_BR);
    } else if (is(t, "hr")) {
        block(p, IT_BLOCK);
        block(p, IT_HR);
    } else if (one_of(t, "b strong")) {
        flush_word(p);
        S.bold++;
    } else if (one_of(t, "i em cite var dfn")) {
        flush_word(p);
        S.italic++;
    } else if (one_of(t, "code tt kbd samp")) {
        flush_word(p);
        S.code++;
    } else if (is(t, "pre")) {
        block(p, IT_PARA);
        S.pre++;
        S.code++;
        S.pre_first = 1;                                   /* a newline right after <pre> is not shown */
    } else if (is(t, "blockquote")) {
        block(p, IT_PARA);
        S.indent++;
        S.italic++;
    } else if (is(t, "center")) {
        block(p, IT_BLOCK);
        S.center++;
    } else if (is(t, "ul") || is(t, "ol")) {
        block(p, S.depth ? IT_BLOCK : IT_PARA);
        if (S.depth < 8) {
            S.list[S.depth].ordered = is(t, "ol");
            S.list[S.depth].count = 0;
            S.depth++;
        }
        S.indent++;
    } else if (is(t, "li")) {
        block(p, IT_BLOCK);
        struct item *it = add_item(p, IT_BULLET);
        if (it) {
            it->indent = S.indent;
            if (S.depth && S.list[S.depth - 1].ordered) {
                char num[12];
                snprintf(num, sizeof num, "%d.", ++S.list[S.depth - 1].count);
                it->len = strlen(num);
                it->off = pool_add(p, num, it->len);
            }
        }
    } else if (is(t, "a")) {
        flush_word(p);
        const char *href = attr_get(a, na, "href");
        S.link = href && *href ? add_link(p, href) : 0;
    } else if (is(t, "img")) {
        const char *alt = attr_get(a, na, "alt"), *src = attr_get(a, na, "src");
        char label[160];
        if (alt && *alt)
            snprintf(label, sizeof label, "[image: %s]", alt);
        else if (src && *src) {
            const char *base = strrchr(src, '/');
            snprintf(label, sizeof label, "[image: %s]", base ? base + 1 : src);
        } else {
            safestrcpy(label, "[image]", sizeof label);
        }
        flush_word(p);
        S.want_space = 1;
        S.italic++;
        for (char *w = label; *w;) {
            char *sp = strchr(w, ' ');
            int l = sp ? sp - w : (int)strlen(w);
            emit_word(p, w, l, MUTED_FG);
            w += l + (sp ? 1 : 0);
            if (!sp)
                break;
            S.want_space = 1;
        }
        S.italic--;
    } else if (is(t, "table")) {
        block(p, IT_PARA);
    } else if (is(t, "td") || is(t, "th")) {
        flush_word(p);
        if (!S.first_cell) {
            struct item *it = add_item(p, IT_GAP);
            if (it)
                it->indent = S.indent;
        }
        S.first_cell = 0;
        S.want_space = 0;
        if (is(t, "th"))
            S.bold++;
    } else if (is(t, "input")) {
        const char *type = attr_get(a, na, "type"), *value = attr_get(a, na, "value");
        if (type && is(type, "hidden"))
            return;
        char label[96];
        if (type && (is(type, "submit") || is(type, "button")))
            snprintf(label, sizeof label, "[ %s ]", value && *value ? value : "Submit");
        else if (type && (is(type, "checkbox") || is(type, "radio")))
            safestrcpy(label, "[ ]", sizeof label);
        else
            safestrcpy(label, "[________]", sizeof label);
        flush_word(p);
        S.want_space = 1;
        emit_word(p, label, strlen(label), MUTED_FG);
    }
}

static void close_tag(struct page *p, const char *t)
{
    if (is(t, "title")) {
        flush_word(p);
        S.in_title = 0;
    } else if (t[0] == 'h' && t[1] >= '1' && t[1] <= '6' && !t[2]) {
        flush_word(p);
        if (S.bold) S.bold--;
        S.scale = 1;
        S.color = 0;
        block(p, IT_PARA);
    } else if (is(t, "p")) {
        block(p, IT_PARA);
    } else if (one_of(t, "div section article header footer nav main aside form fieldset address figure figcaption dl dd dt tr noscript")) {
        block(p, IT_BLOCK);
    } else if (one_of(t, "b strong")) {
        flush_word(p);
        if (S.bold) S.bold--;
    } else if (one_of(t, "i em cite var dfn")) {
        flush_word(p);
        if (S.italic) S.italic--;
    } else if (one_of(t, "code tt kbd samp")) {
        flush_word(p);
        if (S.code) S.code--;
    } else if (is(t, "pre")) {
        flush_word(p);
        if (S.pre) S.pre--;
        if (S.code) S.code--;
        block(p, IT_PARA);
    } else if (is(t, "blockquote")) {
        block(p, IT_PARA);
        if (S.indent) S.indent--;
        if (S.italic) S.italic--;
    } else if (is(t, "center")) {
        block(p, IT_BLOCK);
        if (S.center) S.center--;
    } else if (is(t, "ul") || is(t, "ol")) {
        block(p, S.depth > 1 ? IT_BLOCK : IT_PARA);
        if (S.depth) S.depth--;
        if (S.indent) S.indent--;
    } else if (is(t, "a")) {
        flush_word(p);
        S.link = 0;
    } else if (is(t, "table")) {
        block(p, IT_PARA);
    } else if (is(t, "th")) {
        flush_word(p);
        if (S.bold) S.bold--;
    }
}

/* Parses the tag starting at html[i] == '<'; returns the index after it. */
static int parse_tag(struct page *p, const char *html, int i, int len)
{
    if (!strncmp(html + i, "<!--", 4)) {
        for (i += 4; i + 2 < len; i++)
            if (html[i] == '-' && html[i + 1] == '-' && html[i + 2] == '>')
                return i + 3;
        return len;
    }
    if (html[i + 1] == '!' || html[i + 1] == '?') {
        while (i < len && html[i] != '>')
            i++;
        return i + 1;
    }
    int closing = html[i + 1] == '/';
    i += 1 + closing;
    char name[16];
    int n = 0;
    while (i < len && n < (int)sizeof name - 1 && html[i] != '>' && html[i] != '/' && html[i] != ' ' &&
           html[i] != '\t' && html[i] != '\n' && html[i] != '\r') {
        char ch = html[i++];
        name[n++] = ch >= 'A' && ch <= 'Z' ? ch + 32 : ch;
    }
    name[n] = 0;

    struct attr a[8];
    int na = 0;
    while (i < len && html[i] != '>') {
        while (i < len && (html[i] == ' ' || html[i] == '\t' || html[i] == '\n' || html[i] == '\r' || html[i] == '/'))
            i++;
        if (i >= len || html[i] == '>')
            break;
        char an[16];
        int k = 0;
        while (i < len && html[i] != '=' && html[i] != '>' && html[i] != ' ' && html[i] != '\t' && html[i] != '\n' &&
               html[i] != '/' && html[i] != '\r') {
            if (k < (int)sizeof an - 1)
                an[k++] = html[i] >= 'A' && html[i] <= 'Z' ? html[i] + 32 : html[i];
            i++;
        }
        an[k] = 0;
        char val[512];
        int v = 0;
        if (i < len && html[i] == '=') {
            i++;
            if (i < len && (html[i] == '"' || html[i] == '\'')) {
                char q = html[i++];
                while (i < len && html[i] != q) {
                    if (v < (int)sizeof val - 1)
                        val[v++] = html[i];
                    i++;
                }
                i++;
            } else {
                while (i < len && html[i] != ' ' && html[i] != '>' && html[i] != '\t' && html[i] != '\n') {
                    if (v < (int)sizeof val - 1)
                        val[v++] = html[i];
                    i++;
                }
            }
        }
        val[v] = 0;
        if (k && na < 8) {
            safestrcpy(a[na].name, an, sizeof a[na].name);
            safestrcpy(a[na].value, val, sizeof a[na].value);
            na++;
        }
    }
    i++;                                                              /* past '>' */

    if (closing) {
        close_tag(p, name);
    } else {
        open_tag(p, name, a, na);
        if (is(name, "script") || is(name, "style")) {                /* skip raw text up to the end tag */
            for (; i + 2 < len; i++) {
                if (html[i] == '<' && html[i + 1] == '/' && (html[i + 2] | 0x20) == name[0]) {
                    while (i < len && html[i] != '>')
                        i++;
                    return i + 1;
                }
            }
            return len;
        }
    }
    return i;
}

void html_parse(struct page *p, const char *html, int len)
{
    memset(&S, 0, sizeof S);
    S.scale = 1;
    wl = 0;
    for (int i = 0; i < len;) {
        unsigned char c = html[i];
        if (c == '<' && i + 1 < len && (((html[i + 1] | 0x20) >= 'a' && (html[i + 1] | 0x20) <= 'z') || html[i + 1] == '/' ||
                                         html[i + 1] == '!' || html[i + 1] == '?')) {
            flush_word(p);
            i = parse_tag(p, html, i, len);
            continue;
        }
        if (c == '&') {                                               /* &name; or &#nn; */
            int j = i + 1;
            while (j < len && j - i < 10 && html[j] != ';' && html[j] != ' ' && html[j] != '<' && html[j] != '&')
                j++;
            char tmp[8];
            const char *rep = j < len && html[j] == ';' ? entity_text(html + i + 1, j - i - 1, tmp) : NULL;
            if (rep) {
                if (!strcmp(rep, " "))
                    put_char(p, ' ');
                else
                    put_str(p, rep);
                i = j + 1;
                continue;
            }
        }
        if (c >= 0xC0) {                                              /* UTF-8 */
            int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1, cp = c & (0x3F >> extra);
            int k = 1;
            for (; k <= extra && i + k < len && (html[i + k] & 0xC0) == 0x80; k++)
                cp = (cp << 6) | (html[i + k] & 0x3F);
            char tmp[2];
            put_str(p, cp_text(cp, tmp));
            i += k;
            continue;
        }
        if (c >= 0x80) {
            put_char(p, '?');
            i++;
            continue;
        }
        put_char(p, c);
        i++;
    }
    flush_word(p);
}

void text_parse(struct page *p, const char *text, int len)
{
    memset(&S, 0, sizeof S);
    S.scale = 1;
    S.pre = S.code = 1;
    wl = 0;
    for (int i = 0; i < len; i++)
        put_char(p, (unsigned char)text[i] < 0x80 ? text[i] : '?');
    flush_word(p);
}

/* ---------------- layout ---------------- */

static struct {
    struct page *p;
    int x, y, left, right, lh, first, center, gap;
} L;

static struct box *add_box(void)
{
    struct page *p = L.p;
    if (p->nboxes == p->cap_boxes) {
        int cap = p->cap_boxes ? p->cap_boxes * 2 : 512;
        struct box *n = realloc(p->boxes, cap * sizeof *n);
        if (!n)
            return NULL;
        p->boxes = n;
        p->cap_boxes = cap;
    }
    struct box *b = &p->boxes[p->nboxes++];
    memset(b, 0, sizeof *b);
    return b;
}

static void flush_line(void)
{
    struct page *p = L.p;
    if (L.first == p->nboxes) {
        L.x = L.left;
        return;
    }
    struct box *f = &p->boxes[L.first], *l = &p->boxes[p->nboxes - 1];
    if (L.center) {
        int shift = (L.right - L.left - (l->x + l->w - f->x)) / 2;
        if (shift > 0)
            for (int i = L.first; i < p->nboxes; i++)
                p->boxes[i].x += shift;
    }
    for (int i = L.first; i < p->nboxes; i++)
        p->boxes[i].y = L.y + L.lh - p->boxes[i].h;
    L.y += L.lh + 2;
    L.lh = 0;
    L.first = p->nboxes;
    L.x = L.left;
    L.center = 0;
}

void page_layout(struct page *p, int W)
{
    memset(&L, 0, sizeof L);
    L.p = p;
    p->nboxes = 0;
    p->layout_w = W;
    L.y = MARGIN;
    L.left = MARGIN;
    L.right = W - MARGIN;
    L.x = L.left;

    for (int n = 0; n < p->nitems; n++) {
        struct item *it = &p->items[n];
        int left = MARGIN + it->indent * INDENT_PX;
        if (left != L.left) {
            flush_line();
            L.left = left;
            L.x = left;
        }
        switch (it->type) {
        case IT_BLOCK:
            flush_line();
            break;
        case IT_PARA:
            flush_line();
            if (L.y > MARGIN && !L.gap) {
                L.y += 8;
                L.gap = 1;
            }
            break;
        case IT_BR:
            if (L.first == p->nboxes)
                L.y += 18;
            else
                flush_line();
            break;
        case IT_HR: {
            flush_line();
            struct box *b = add_box();
            if (b) {
                b->type = IT_HR;
                b->x = L.left;
                b->w = L.right - L.left;
                b->h = 10;
                b->y = L.y;
            }
            L.first = p->nboxes;
            L.y += 14;
            L.gap = 0;
            break;
        }
        case IT_GAP:
            if (L.x > L.left)
                L.x += 28;
            break;
        case IT_BULLET: {
            flush_line();
            struct box *b = add_box();
            if (b) {
                b->type = IT_BULLET;
                b->len = it->len;
                b->off = it->off;
                b->h = 16;
                b->w = it->len ? it->len * 8 : 8;
                b->x = L.left - (it->len ? it->len * 8 + 6 : 14);
                b->y = L.y;
                L.first = p->nboxes - 1;            /* the marker belongs to this line */
                if (L.lh < 16)
                    L.lh = 16;
                L.center = 0;
            }
            L.gap = 0;
            break;
        }
        case IT_WORD: {
            int cw = 8 * it->scale, w = it->len * cw, chunk = (L.right - L.left) / cw;
            if (chunk < 1)
                chunk = 1;
            int sp = (it->flags & F_SP) && L.x > L.left && L.first < p->nboxes ? 8 : 0;
            if (L.first == p->nboxes)
                L.center = it->flags & F_CENTER;
            if (!(it->flags & F_PRE) && L.x > L.left && L.x + sp + w > L.right && w <= L.right - L.left) {
                flush_line();
                sp = 0;
            }
            for (int off = 0; off < it->len;) {
                int take = it->len - off, tw;
                if (take * cw > L.right - L.x - sp && L.x > L.left && L.first < p->nboxes) {
                    flush_line();
                    sp = 0;
                }
                if (take > chunk)
                    take = chunk;
                tw = take * cw;
                struct box *b = add_box();
                if (!b)
                    break;
                b->type = IT_WORD;
                b->flags = it->flags;
                b->scale = it->scale;
                b->link = it->link;
                b->color = it->color;
                b->off = it->off + off;
                b->len = take;
                b->w = tw;
                b->h = 16 * it->scale;
                b->x = L.x + sp;
                L.x = b->x + tw;
                if (b->h > L.lh)
                    L.lh = b->h;
                off += take;
                sp = 0;
                if (off < it->len)
                    flush_line();
            }
            L.gap = 0;
            break;
        }
        }
    }
    flush_line();
    p->height = L.y + MARGIN;
}

/* ---------------- drawing and hit testing ---------------- */

void page_draw(struct page *p, struct rect view, int scroll, int hover)
{
    char tmp[512];
    for (int i = 0; i < p->nboxes; i++) {
        struct box *b = &p->boxes[i];
        int sy = view.y + b->y - scroll;
        if (sy + b->h < view.y || sy > view.y + view.h)
            continue;
        int sx = view.x + b->x;
        if (b->type == IT_HR) {
            gfx_fill(sx, sy + 4, b->w, 2, RGB(0xc8, 0xcc, 0xd0));
        } else if (b->type == IT_BULLET) {
            if (b->len) {
                int n = b->len < 12 ? b->len : 12;
                memcpy(tmp, p->pool + b->off, n);
                tmp[n] = 0;
                gfx_text(sx, sy, tmp, MUTED_FG);
            } else {
                gfx_fill(sx + 1, sy + 6, 5, 5, MUTED_FG);
            }
        } else if (b->type == IT_WORD) {
            int n = b->len < (int)sizeof tmp - 1 ? b->len : (int)sizeof tmp - 1;
            memcpy(tmp, p->pool + b->off, n);
            tmp[n] = 0;
            u32 fg = b->color ? b->color : PAGE_FG;
            int linked = b->link != 0, hot = linked && (int)b->link - 1 == hover;
            if (linked)
                fg = hot ? LINK_HOT : LINK_FG;
            else if (b->flags & F_ITALIC && !b->color)
                fg = RGB(0x3c, 0x44, 0x5c);
            if (b->flags & F_CODE)
                gfx_fill(sx - 1, sy, b->w + 2, b->h, RGB(0xed, 0xef, 0xf2));
            if (b->scale > 1) {
                gfx_text_scaled(sx, sy, tmp, fg, b->scale);
            } else {
                gfx_text(sx, sy, tmp, fg);
                if (b->flags & F_BOLD)
                    gfx_text(sx + 1, sy, tmp, fg);
            }
            if (linked)
                gfx_fill(sx, sy + b->h - 2, b->w, 1, fg);
        }
    }
}

int page_link_at(struct page *p, int x, int y)
{
    for (int i = 0; i < p->nboxes; i++) {
        struct box *b = &p->boxes[i];
        if (b->type == IT_WORD && b->link && x >= b->x && x < b->x + b->w && y >= b->y && y < b->y + b->h)
            return b->link - 1;
    }
    return -1;
}
