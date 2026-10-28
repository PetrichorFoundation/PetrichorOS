/* HTML tokenizer and tree builder. */
#include "web.h"

static const char *const tag_names[] = {
#define X(id, name) name,
    TAGS(X)
#undef X
};

/* ---- memory ---- */

void *arena_alloc(struct page *p, int n)
{
    n = (n + 7) & ~7;
    struct arena *a = p->arena;
    if (!a || a->used + n > a->cap) {
        int cap = n > 60000 ? n : 64000;
        struct arena *na = malloc(sizeof *na + cap);
        if (!na)
            return NULL;
        na->next = a;
        na->used = 0;
        na->cap = cap;
        p->arena = a = na;
    }
    void *r = a->data + a->used;
    a->used += n;
    memset(r, 0, n);
    return r;
}

const char *arena_str(struct page *p, const char *s, int n)
{
    char *d = arena_alloc(p, n + 1);
    if (!d)
        return "";
    memcpy(d, s, n);
    d[n] = 0;
    return d;
}

void page_clear(struct page *p)
{
    css_free_rules(p);
    for (int i = 0; i < p->nres; i++) {
        if (p->res[i].img)
            img_free(p->res[i].img);
        free(p->res[i].css);
    }
    free(p->res);
    free(p->sheets);
    free(p->links);
    free(p->cmds);
    for (struct arena *a = p->arena; a;) {
        struct arena *n = a->next;
        free(a);
        a = n;
    }
    memset(p, 0, sizeof *p);
}

void page_free(struct page *p) { page_clear(p); }

int add_resource(struct page *p, const char *url, int kind)
{
    for (int i = 0; i < p->nres; i++)
        if (p->res[i].kind == kind && !strcmp(p->res[i].url, url))
            return i;
    if (p->nres == p->cap_res) {
        int nc = p->cap_res ? p->cap_res * 2 : 32;
        struct res *r = realloc(p->res, nc * sizeof *r);
        if (!r)
            return -1;
        p->res = r;
        p->cap_res = nc;
    }
    struct res *r = &p->res[p->nres];
    memset(r, 0, sizeof *r);
    safestrcpy(r->url, url, sizeof r->url);
    r->kind = kind;
    r->state = RS_WANT;
    return p->nres++;
}

/* ---- entities ---- */

static int utf8_put(char *o, int cp)
{
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63);
    return 4;
}

static const struct { const char *name; int cp; } ENT[] = {
    {"amp", '&'}, {"lt", '<'}, {"gt", '>'}, {"quot", '"'}, {"apos", '\''}, {"nbsp", 0xA0}, {"copy", 0xA9},
    {"reg", 0xAE}, {"trade", 0x2122}, {"mdash", 0x2014}, {"ndash", 0x2013}, {"hellip", 0x2026}, {"bull", 0x2022},
    {"middot", 0xB7}, {"laquo", 0xAB}, {"raquo", 0xBB}, {"lsquo", 0x2018}, {"rsquo", 0x2019}, {"ldquo", 0x201C},
    {"rdquo", 0x201D}, {"times", 0xD7}, {"euro", 0x20AC}, {"pound", 0xA3}, {"yen", 0xA5}, {"cent", 0xA2},
    {"deg", 0xB0}, {"larr", 0x2190}, {"rarr", 0x2192}, {"uarr", 0x2191}, {"darr", 0x2193}, {"sect", 0xA7},
    {"para", 0xB6}, {"plusmn", 0xB1}, {"frac12", 0xBD}, {"check", 0x2713}, {"hearts", 0x2665}, {"ensp", 0x2002},
    {"emsp", 0x2003}, {"thinsp", 0x2009}, {"zwnj", 0x200C}, {"zwj", 0x200D}, {"shy", 0xAD}, {"eacute", 0xE9},
    {"egrave", 0xE8}, {"agrave", 0xE0}, {"aacute", 0xE1}, {"uuml", 0xFC}, {"ouml", 0xF6}, {"auml", 0xE4},
    {"szlig", 0xDF}, {"ntilde", 0xF1}, {"ccedil", 0xE7}, {"iexcl", 0xA1}, {"iquest", 0xBF}, {"infin", 0x221E},
    {"ne", 0x2260}, {"le", 0x2264}, {"ge", 0x2265}, {"minus", 0x2212}, {"divide", 0xF7}, {"micro", 0xB5},
};

/* Decodes entities in s[0..n) into out (which must hold at least n*4 bytes); returns the length. */
static int decode_entities(const char *s, int n, char *out)
{
    int o = 0;
    for (int i = 0; i < n;) {
        if (s[i] != '&') {
            out[o++] = s[i++];
            continue;
        }
        int j = i + 1;
        while (j < n && j - i < 12 && s[j] != ';' && s[j] != ' ' && s[j] != '&' && s[j] != '<')
            j++;
        int cp = -1;
        if (j < n && s[j] == ';') {
            if (s[i + 1] == '#') {
                cp = 0;
                int hex = s[i + 2] == 'x' || s[i + 2] == 'X';
                for (int k = i + 2 + hex; k < j; k++) {
                    char c = s[k] | 0x20;
                    int d = c >= 'a' ? c - 'a' + 10 : c - '0';
                    if (d < 0 || d >= (hex ? 16 : 10)) { cp = -1; break; }
                    cp = cp * (hex ? 16 : 10) + d;
                }
            } else {
                for (u32 k = 0; k < sizeof ENT / sizeof ENT[0]; k++)
                    if ((int)strlen(ENT[k].name) == j - i - 1 && !strncmp(ENT[k].name, s + i + 1, j - i - 1))
                        cp = ENT[k].cp;
            }
        }
        if (cp > 0 && cp < 0x110000) {
            o += utf8_put(out + o, cp);
            i = j + 1;
        } else {
            out[o++] = s[i++];
        }
    }
    return o;
}

int b64_decode(const char *s, int n, u8 *out)
{
    int o = 0, acc = 0, bits = 0;
    for (int i = 0; i < n; i++) {
        int c = s[i], v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == '+' || c == '-') v = 62;
        else if (c == '/' || c == '_') v = 63;
        else continue;
        acc = acc << 6 | v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = acc >> bits;
        }
    }
    return o;
}

/* ---- attributes ---- */

const char *attr_val(const struct node *n, const char *name)
{
    for (int i = 0; i < n->nattr; i++)
        if (!strcmp(n->attr[i].name, name))
            return n->attr[i].val;
    return NULL;
}

static int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

static int name_char(int c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

static int find_tag(const char *s, int n)
{
    for (int i = 1; i < TG_OTHER; i++)
        if ((int)strlen(tag_names[i]) == n && !strncmp(tag_names[i], s, n))
            return i;
    return TG_OTHER;
}

static u32 hash_name(const char *s, int n)
{
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ (u32)lc((u8)s[i])) * 16777619u;
    return h;
}

void mask_set(u32 mask[2], const char *s, int n)
{
    u32 h = hash_name(s, n) % 64;
    mask[h >> 5] |= 1u << (h & 31);
}

/* ---- the tree builder ---- */

struct builder {
    struct page *p;
    struct node *cur;
    int in_body;
};

static struct node *new_node(struct builder *b, int type, int tag)
{
    struct node *n = arena_alloc(b->p, sizeof *n);
    if (!n)
        return NULL;
    n->type = type;
    n->tag = tag;
    n->mincw = n->maxcw = -1;
    return n;
}

static void append(struct node *parent, struct node *n)
{
    n->parent = parent;
    n->prev = parent->last;
    if (parent->last)
        parent->last->next = n;
    else
        parent->first = n;
    parent->last = n;
}

static int add_link(struct page *p, const char *href)
{
    if (p->nlinks >= 2000)
        return 0;
    char (*nl)[512] = realloc(p->links, (p->nlinks + 1) * sizeof *nl);
    if (!nl)
        return 0;
    p->links = nl;
    url_resolve(p->url, href, nl[p->nlinks], 512);
    return ++p->nlinks;
}

static int is_block_tag(int t)
{
    switch (t) {
    case TG_DIV: case TG_P: case TG_H1: case TG_H2: case TG_H3: case TG_H4: case TG_H5: case TG_H6: case TG_UL: case TG_OL:
    case TG_DL: case TG_TABLE: case TG_PRE: case TG_BLOCKQUOTE: case TG_FORM: case TG_NAV: case TG_HEADER: case TG_FOOTER:
    case TG_MAIN: case TG_SECTION: case TG_ARTICLE: case TG_ASIDE: case TG_FIGURE: case TG_HR: case TG_ADDRESS:
    case TG_FIELDSET: case TG_CENTER: case TG_DETAILS: case TG_MENU:
        return 1;
    }
    return 0;
}

static int has_open(struct node *cur, int tag, const int *stop, int nstop)
{
    for (struct node *n = cur; n; n = n->parent) {
        if (n->tag == tag)
            return 1;
        for (int i = 0; i < nstop; i++)
            if (n->tag == stop[i])
                return 0;
    }
    return 0;
}

static void pop_to(struct builder *b, int tag)
{
    for (struct node *n = b->cur; n; n = n->parent)
        if (n->tag == tag) {
            b->cur = n->parent ? n->parent : n;
            return;
        }
}

static void implied_end(struct builder *b, int tag)
{
    static const int scope[] = {TG_TABLE, TG_TD, TG_TH, TG_BUTTON, TG_HTML, TG_BODY, TG_OBJECT};
    if (is_block_tag(tag) && has_open(b->cur, TG_P, scope, 7))
        pop_to(b, TG_P);
    switch (tag) {
    case TG_LI: {
        static const int st[] = {TG_UL, TG_OL, TG_MENU, TG_DIR};
        if (has_open(b->cur, TG_LI, st, 4))
            pop_to(b, TG_LI);
        break;
    }
    case TG_DT: case TG_DD: {
        static const int st[] = {TG_DL};
        if (has_open(b->cur, TG_DT, st, 1)) pop_to(b, TG_DT);
        else if (has_open(b->cur, TG_DD, st, 1)) pop_to(b, TG_DD);
        break;
    }
    case TG_TR: {
        static const int st[] = {TG_TABLE};
        if (has_open(b->cur, TG_TD, st, 1)) pop_to(b, TG_TD);
        else if (has_open(b->cur, TG_TH, st, 1)) pop_to(b, TG_TH);
        if (has_open(b->cur, TG_TR, st, 1)) pop_to(b, TG_TR);
        break;
    }
    case TG_TD: case TG_TH: {
        static const int st[] = {TG_TR, TG_TABLE};
        if (has_open(b->cur, TG_TD, st, 2)) pop_to(b, TG_TD);
        else if (has_open(b->cur, TG_TH, st, 2)) pop_to(b, TG_TH);
        break;
    }
    case TG_THEAD: case TG_TBODY: case TG_TFOOT: {
        static const int st[] = {TG_TABLE};
        if (has_open(b->cur, TG_TD, st, 1)) pop_to(b, TG_TD);
        if (has_open(b->cur, TG_TH, st, 1)) pop_to(b, TG_TH);
        if (has_open(b->cur, TG_TR, st, 1)) pop_to(b, TG_TR);
        if (has_open(b->cur, TG_THEAD, st, 1)) pop_to(b, TG_THEAD);
        if (has_open(b->cur, TG_TBODY, st, 1)) pop_to(b, TG_TBODY);
        break;
    }
    case TG_OPTION: {
        static const int st[] = {TG_SELECT};
        if (has_open(b->cur, TG_OPTION, st, 1))
            pop_to(b, TG_OPTION);
        break;
    }
    }
}

static int is_void(int t)
{
    return t == TG_BR || t == TG_HR || t == TG_IMG || t == TG_INPUT || t == TG_META || t == TG_LINK || t == TG_BASE ||
           t == TG_COL || t == TG_SOURCE;
}

static void add_text(struct builder *b, const char *s, int n)
{
    if (n <= 0)
        return;
    char *tmp = malloc(n * 4 + 1);
    if (!tmp)
        return;
    int m = decode_entities(s, n, tmp);
    struct node *t = new_node(b, 1, TG_NONE);
    if (t) {
        t->text = arena_str(b->p, tmp, m);
        t->tlen = m;
        t->link = b->cur->link;
        append(b->cur, t);
    }
    free(tmp);
}

static void start_element(struct builder *b, int tag, const char *name, int nlen, struct attr *at, int nat, int selfclose)
{
    struct page *p = b->p;
    implied_end(b, tag);
    struct node *n = new_node(b, 0, tag);
    if (!n)
        return;
    if (tag == TG_OTHER)
        n->text = arena_str(p, name, nlen);
    n->link = b->cur->link;
    if (nat) {
        n->attr = arena_alloc(p, nat * sizeof *at);
        if (n->attr)
            memcpy(n->attr, at, nat * sizeof *at);
        n->nattr = n->attr ? nat : 0;
    }
    if (tag < TG_OTHER)
        mask_set(n->mask, tag_names[tag], strlen(tag_names[tag]));
    const char *id = attr_val(n, "id");
    if (id)
        mask_set(n->mask, id, strlen(id));
    const char *cl = attr_val(n, "class");
    for (const char *c = cl; c && *c;) {
        while (*c == ' ' || *c == '\t' || *c == '\n')
            c++;
        const char *e = c;
        while (*e && *e != ' ' && *e != '\t' && *e != '\n')
            e++;
        if (e > c)
            mask_set(n->mask, c, e - c);
        c = e;
    }
    if (tag == TG_A) {
        const char *href = attr_val(n, "href");
        if (href && strncmp(href, "javascript:", 11) && href[0] != '#')
            n->link = add_link(p, href);
        else if (href && href[0] == '#')
            n->link = 0;
    }
    append(b->cur, n);
    if (tag == TG_BODY && !p->body)
        p->body = n;
    if (tag == TG_IMG) {
        const char *src = attr_val(n, "src");
        if (!src || !*src) src = attr_val(n, "data-src");
        if (src && *src && p->nres < 80) {
            char full[512];
            if (!strncmp(src, "data:", 5)) {
                const char *comma = strchr(src, ',');
                if (comma && strstr(src, "base64")) {
                    int len = strlen(comma + 1);
                    u8 *raw = malloc(len);
                    if (raw) {
                        int m = b64_decode(comma + 1, len, raw);
                        struct image *im = img_decode(raw, m);
                        free(raw);
                        if (im) {
                            char key[32];
                            snprintf(key, sizeof key, "data:%d", p->nres);
                            int ri = add_resource(p, key, R_IMAGE);
                            if (ri >= 0) {
                                p->res[ri].img = im;
                                p->res[ri].state = RS_DONE;
                                n->img = ri + 1;
                            }
                        }
                    }
                }
            } else {
                url_resolve(p->url, src, full, sizeof full);
                int ri = add_resource(p, full, R_IMAGE);
                if (ri >= 0)
                    n->img = ri + 1;
            }
        }
    }
    if (is_void(tag) || selfclose) {
        if (tag == TG_LINK) {
            const char *rel = attr_val(n, "rel"), *href = attr_val(n, "href");
            if (rel && href && strstr(rel, "stylesheet") && !strstr(rel, "alternate") && p->nres < 80) {
                char full[512];
                url_resolve(p->url, href, full, sizeof full);
                int ri = add_resource(p, full, R_CSS);
                if (ri >= 0 && p->nsheets < 64) {
                    if (p->nsheets == p->cap_sheets) {
                        int nc = p->cap_sheets ? p->cap_sheets * 2 : 8;
                        struct sheet *ns = realloc(p->sheets, nc * sizeof *ns);
                        if (!ns) return;
                        p->sheets = ns;
                        p->cap_sheets = nc;
                    }
                    p->sheets[p->nsheets++] = (struct sheet){NULL, 0, ri + 1};
                }
            }
        }
        return;
    }
    b->cur = n;
}

static void add_sheet_text(struct page *p, const char *text, int len)
{
    if (p->nsheets == p->cap_sheets) {
        int nc = p->cap_sheets ? p->cap_sheets * 2 : 8;
        struct sheet *ns = realloc(p->sheets, nc * sizeof *ns);
        if (!ns)
            return;
        p->sheets = ns;
        p->cap_sheets = nc;
    }
    p->sheets[p->nsheets++] = (struct sheet){arena_str(p, text, len), len, 0};
}

static int ci_match(const char *s, const char *word, int n)
{
    for (int i = 0; i < n; i++)
        if (lc(s[i]) != word[i])
            return 0;
    return 1;
}

void html_parse(struct page *p, const char *html, int len)
{
    struct builder b = {.p = p};
    struct node *root = new_node(&b, 0, TG_HTML);
    if (!root)
        return;
    p->root = root;
    b.cur = root;
    int i = 0;
    while (i < len) {
        if (html[i] != '<') {
            int j = i;
            while (j < len && html[j] != '<')
                j++;
            add_text(&b, html + i, j - i);
            i = j;
            continue;
        }
        if (i + 3 < len && !strncmp(html + i, "<!--", 4)) {
            const char *e = NULL;
            for (int k = i + 4; k + 2 < len; k++)
                if (html[k] == '-' && html[k + 1] == '-' && html[k + 2] == '>') { e = html + k + 3; break; }
            i = e ? e - html : len;
            continue;
        }
        if (i + 1 < len && (html[i + 1] == '!' || html[i + 1] == '?')) {
            while (i < len && html[i] != '>')
                i++;
            i++;
            continue;
        }
        int j = i + 1;
        int closing = j < len && html[j] == '/';
        if (closing)
            j++;
        if (j >= len || !((html[j] | 0x20) >= 'a' && (html[j] | 0x20) <= 'z')) {
            add_text(&b, "<", 1);                                  /* a stray < */
            i++;
            continue;
        }
        int ns = j;
        while (j < len && name_char(html[j]))
            j++;
        int nlen = j - ns;
        char name[32];
        int ml = nlen < 31 ? nlen : 31;
        for (int k = 0; k < ml; k++)
            name[k] = lc(html[ns + k]);
        name[ml] = 0;
        int tag = find_tag(name, ml);
        struct attr at[24];
        int nat = 0, selfclose = 0;
        while (j < len && html[j] != '>') {
            while (j < len && (html[j] == ' ' || html[j] == '\t' || html[j] == '\n' || html[j] == '\r' || html[j] == '/')) {
                if (html[j] == '/' && j + 1 < len && html[j + 1] == '>')
                    selfclose = 1;
                j++;
            }
            if (j >= len || html[j] == '>')
                break;
            int as = j;
            while (j < len && html[j] != '=' && html[j] != '>' && html[j] != ' ' && html[j] != '\t' && html[j] != '\n' &&
                   html[j] != '/' && html[j] != '\r')
                j++;
            int al = j - as;
            while (j < len && (html[j] == ' ' || html[j] == '\n' || html[j] == '\t'))
                j++;
            const char *val = "";
            int vl = 0;
            if (j < len && html[j] == '=') {
                j++;
                while (j < len && (html[j] == ' ' || html[j] == '\n' || html[j] == '\t'))
                    j++;
                if (j < len && (html[j] == '"' || html[j] == '\'')) {
                    char q = html[j++];
                    val = html + j;
                    while (j < len && html[j] != q)
                        j++;
                    vl = html + j - val;
                    if (j < len)
                        j++;
                } else {
                    val = html + j;
                    while (j < len && html[j] != ' ' && html[j] != '>' && html[j] != '\n' && html[j] != '\t')
                        j++;
                    vl = html + j - val;
                }
            }
            if (al > 0 && nat < 24 && !closing) {
                char *an = arena_alloc(p, al + 1);
                if (an) {
                    for (int k = 0; k < al; k++)
                        an[k] = lc(html[as + k]);
                    char *tmp = malloc(vl * 4 + 1);
                    if (tmp) {
                        int m = decode_entities(val, vl, tmp);
                        at[nat].name = an;
                        at[nat].val = arena_str(p, tmp, m);
                        nat++;
                        free(tmp);
                    }
                }
            }
        }
        i = j < len ? j + 1 : len;
        if (closing) {
            if (tag != TG_OTHER || b.cur->tag == TG_OTHER)
                pop_to(&b, tag);
            continue;
        }
        start_element(&b, tag, name, ml, at, nat, selfclose);
        if (tag == TG_SCRIPT || tag == TG_STYLE || tag == TG_TEXTAREA || tag == TG_TITLE) {
            int e = i;
            const char *want = tag == TG_SCRIPT ? "script" : tag == TG_STYLE ? "style" : tag == TG_TEXTAREA ? "textarea" : "title";
            int wl = strlen(want);
            while (e < len) {
                if (html[e] == '<' && e + 1 + wl < len && html[e + 1] == '/' && ci_match(html + e + 2, want, wl))
                    break;
                e++;
            }
            if (tag == TG_STYLE) {
                add_sheet_text(p, html + i, e - i);
            } else if (tag == TG_TITLE) {
                char *tmp = malloc((e - i) * 4 + 1);
                if (tmp) {
                    int m = decode_entities(html + i, e - i, tmp);
                    int o = 0;
                    for (int k = 0; k < m && o < (int)sizeof p->title - 1; k++)
                        if (tmp[k] != '\n' && tmp[k] != '\r' && tmp[k] != '\t')
                            p->title[o++] = tmp[k];
                    p->title[o] = 0;
                    free(tmp);
                }
            } else if (tag == TG_TEXTAREA) {
                add_text(&b, html + i, e - i);
            }
            i = e;
            while (i < len && html[i] != '>')
                i++;
            i++;
            if (b.cur->tag == tag)
                b.cur = b.cur->parent ? b.cur->parent : b.cur;
        }
    }
    if (!p->body)
        p->body = root;
    p->dirty_style = 1;
}

void text_parse(struct page *p, const char *text, int len)
{
    char *html = malloc(len * 5 + 64);
    if (!html)
        return;
    int o = snprintf(html, 32, "<pre>");
    for (int i = 0; i < len; i++) {
        if (text[i] == '<') o += snprintf(html + o, 8, "&lt;");
        else if (text[i] == '&') o += snprintf(html + o, 8, "&amp;");
        else html[o++] = text[i];
    }
    o += snprintf(html + o, 16, "</pre>");
    html_parse(p, html, o);
    free(html);
}
