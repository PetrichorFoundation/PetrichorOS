/* The browser's page engine: HTML -> DOM, CSS cascade, box layout into a display list, painting, images.
 *
 *   dom.c     tokenizer + tree builder, entities, text/URL helpers
 *   css.c     stylesheet parser, selector matching, cascade and computed values
 *   layout.c  block / inline / flex / table / float layout, text metrics
 *   paint.c   glyph rendering (any size), display-list painting, hit testing
 *   img.c     PNG / JPEG / GIF decoders and scaling
 */
#pragma once
#include "desk.h"

/* ---- tags ---- */
#define TAGS(X) \
    X(NONE, "") X(HTML, "html") X(HEAD, "head") X(BODY, "body") X(DIV, "div") X(SPAN, "span") X(P, "p") X(A, "a") \
    X(H1, "h1") X(H2, "h2") X(H3, "h3") X(H4, "h4") X(H5, "h5") X(H6, "h6") X(UL, "ul") X(OL, "ol") X(LI, "li") \
    X(DL, "dl") X(DT, "dt") X(DD, "dd") X(TABLE, "table") X(THEAD, "thead") X(TBODY, "tbody") X(TFOOT, "tfoot") \
    X(TR, "tr") X(TD, "td") X(TH, "th") X(CAPTION, "caption") X(COLGROUP, "colgroup") X(COL, "col") X(IMG, "img") \
    X(BR, "br") X(HR, "hr") X(PRE, "pre") X(CODE, "code") X(B, "b") X(STRONG, "strong") X(I, "i") X(EM, "em") \
    X(U, "u") X(S, "s") X(STRIKE, "strike") X(DEL, "del") X(INS, "ins") X(SMALL, "small") X(BIG, "big") X(SUB, "sub") \
    X(SUP, "sup") X(BLOCKQUOTE, "blockquote") X(CENTER, "center") X(FORM, "form") X(INPUT, "input") \
    X(BUTTON, "button") X(TEXTAREA, "textarea") X(SELECT, "select") X(OPTION, "option") X(LABEL, "label") \
    X(NAV, "nav") X(HEADER, "header") X(FOOTER, "footer") X(MAIN, "main") X(SECTION, "section") \
    X(ARTICLE, "article") X(ASIDE, "aside") X(FIGURE, "figure") X(FIGCAPTION, "figcaption") X(STYLE, "style") \
    X(SCRIPT, "script") X(TITLE, "title") X(META, "meta") X(LINK, "link") X(BASE, "base") X(SVG, "svg") \
    X(NOSCRIPT, "noscript") X(IFRAME, "iframe") X(TT, "tt") X(KBD, "kbd") X(SAMP, "samp") X(VAR, "var") \
    X(MARK, "mark") X(CITE, "cite") X(Q, "q") X(ABBR, "abbr") X(ADDRESS, "address") X(DETAILS, "details") \
    X(SUMMARY, "summary") X(FIELDSET, "fieldset") X(LEGEND, "legend") X(FONT, "font") X(MENU, "menu") \
    X(DIR, "dir") X(TEMPLATE, "template") X(OBJECT, "object") X(VIDEO, "video") X(AUDIO, "audio") \
    X(CANVAS, "canvas") X(PICTURE, "picture") X(SOURCE, "source") X(OTHER, "")

enum {
#define X(id, name) TG_##id,
    TAGS(X)
#undef X
    TG_COUNT
};

/* ---- lengths and computed style ---- */
enum { L_AUTO, L_PX, L_PCT, L_FR };                /* L_PCT: v in tenths of a percent; L_FR: v in thousandths */
struct len {
    int v;
    u8 unit;
};

enum { D_NONE, D_BLOCK, D_INLINE, D_INLINE_BLOCK, D_LIST_ITEM, D_TABLE, D_ROW_GROUP, D_ROW, D_CELL, D_CAPTION,
       D_FLEX, D_INLINE_FLEX, D_GRID, D_COL };
enum { POS_STATIC, POS_RELATIVE, POS_ABSOLUTE, POS_FIXED };
enum { FL_NONE, FL_LEFT, FL_RIGHT };
enum { TA_LEFT, TA_CENTER, TA_RIGHT, TA_JUSTIFY };
enum { WS_NORMAL, WS_NOWRAP, WS_PRE, WS_PRE_WRAP, WS_PRE_LINE };
enum { TT_NONE, TT_UPPER, TT_LOWER, TT_CAP };
enum { LS_DISC, LS_CIRCLE, LS_SQUARE, LS_DECIMAL, LS_NONE, LS_ALPHA, LS_ROMAN };
enum { BS_NONE, BS_SOLID, BS_DASHED, BS_DOTTED };
enum { FD_ROW, FD_ROW_REV, FD_COL, FD_COL_REV };
enum { JC_START, JC_END, JC_CENTER, JC_BETWEEN, JC_AROUND, JC_EVENLY };
enum { AI_STRETCH, AI_START, AI_END, AI_CENTER, AI_BASELINE, AI_AUTO };
enum { VA_BASELINE, VA_TOP, VA_MIDDLE, VA_BOTTOM };
enum { DEC_UNDERLINE = 1, DEC_STRIKE = 2 };
enum { OV_VISIBLE, OV_HIDDEN };
enum { BG_NONE, BG_GRAD_V, BG_GRAD_H };

#define MAXVARS 24
struct var {
    const char *name, *val;
    short nlen, vlen;
};

struct style {
    u8 display, position, float_, clear, text_align, white_space, text_transform, list_style;
    u8 visibility, box_sizing, flex_dir, flex_wrap, justify, align_items, align_self, vertical_align;
    u8 decor, bold, italic, mono, overflow, border_style[4], bg_kind, collapse;
    u32 color, bg, bg2, border_color[4];
    int font_size, line_height;                    /* px; line_height 0 = normal */
    struct len width, height, min_w, max_w, min_h, max_h, margin[4], padding[4], flex_basis;
    int border_w[4];                               /* top right bottom left */
    int grow, shrink;                              /* x1000 */
    int gap_r, gap_c, radius, order, opacity;      /* opacity 0..255 */
    int bg_img;                                    /* resource index + 1 */
    u8 bg_size, bg_px, bg_py;                      /* bg_size: 0 auto, 1 cover, 2 contain; position in percent */
    int grid_cols;                                 /* tracks (explicit) */
    struct len grid_col[12];
    int nvars;
    struct var *vars;
};

/* ---- DOM ---- */
struct attr {
    const char *name, *val;
};

struct node {
    u8 type, tag;                                  /* type: 0 element, 1 text */
    u16 nattr, link;                               /* link: 0 none, else page->links index + 1 */
    struct node *parent, *first, *last, *next, *prev;
    struct attr *attr;
    const char *text;                              /* text nodes; element name for TG_OTHER */
    int tlen;
    struct style *st;
    u32 mask[2];                                   /* hashed tag/id/classes, for quick selector rejection */
    int img;                                       /* resource index + 1 for <img> */
    /* layout scratch */
    int mincw, maxcw;                              /* cached intrinsic widths (-1: unknown) */
    u8 bk;                                         /* 0 unknown, 1 block-level, 2 inline-level */
};

/* ---- display list ---- */
enum { K_RECT, K_ROUND, K_TEXT, K_IMAGE, K_GRAD, K_LINE };
struct cmd {
    u8 kind, flags;
    u16 link;
    short fsize;
    int x, y, w, h;
    u32 color, color2;
    const char *text;
    int len;
    int img;                                       /* resource index + 1, or 0 */
    int radius;
};
enum { CF_BOLD = 1, CF_ITALIC = 2, CF_MONO = 4, CF_UNDER = 8, CF_STRIKE = 16, CF_ALT = 32 };

/* ---- resources the page wants ---- */
enum { R_CSS, R_IMAGE };
enum { RS_WANT, RS_LOADING, RS_DONE, RS_FAILED };
struct image {
    int w, h;
    u32 *px;                                       /* 0xAARRGGBB straight alpha */
    int sw, sh;                                    /* cached scaled copy */
    u32 *scaled;
};
struct res {
    char url[512];
    u8 kind, state;
    struct image *img;
    char *css;                                     /* stylesheet text */
    int css_len;
};

struct sheet {
    const char *text;
    int len;
    int res;                                       /* resource index + 1 for linked sheets */
};

struct arena {
    struct arena *next;
    int used, cap;
    char data[];
};

struct rule;

struct page {
    char url[512];
    char title[96];
    struct arena *arena;
    struct node *root, *body;
    char (*links)[512];
    int nlinks;
    struct res *res;
    int nres, cap_res;
    struct sheet *sheets;
    int nsheets, cap_sheets;
    struct rule *rules;
    int nrules, cap_rules;
    struct cmd *cmds;
    int ncmds, cap_cmds;
    struct arena *lay;                             /* text built during layout; reset on every layout */
    int layout_w, height;
    int styled;                                    /* rules have been applied */
    int dirty_style;                               /* a stylesheet arrived since the last restyle */
    u32 page_bg;                                   /* canvas background (body or html) */
    int text_only;
};

/* dom.c */
void mask_set(u32 mask[2], const char *s, int n);
void page_free(struct page *p);
void page_clear(struct page *p);
void html_parse(struct page *p, const char *html, int len);
void text_parse(struct page *p, const char *text, int len);
void *arena_alloc(struct page *p, int n);
const char *arena_str(struct page *p, const char *s, int n);
const char *attr_val(const struct node *n, const char *name);
int add_resource(struct page *p, const char *url, int kind);
int b64_decode(const char *s, int n, u8 *out);

/* css.c */
void css_restyle(struct page *p, int viewport_w);
void css_free_rules(struct page *p);
int css_parse_len(const char *s, int n, int font_px, struct len *out);

/* layout.c */
void page_layout(struct page *p, int width);
int text_width(const char *s, int n, int px, int flags);
int glyph_adv(int ch, int px, int flags);           /* advance in 1/16 px */

/* paint.c */
void page_draw(struct page *p, struct rect view, int scroll, int hover_link);
int page_link_at(struct page *p, int x, int y);     /* page coordinates; -1 if none */
void glyph_flush(void);

/* img.c */
struct image *img_decode(const u8 *data, int len);
const u32 *img_scaled(struct image *im, int w, int h);
void img_free(struct image *im);

/* from browser.c */
int page_next_resource(struct page *p);             /* index of a resource still wanted, or -1 */
