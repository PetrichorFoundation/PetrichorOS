/* A small HTML renderer: parse markup into a flat list of items, lay them out for a given
 * width, and draw the visible part.  Supports headings, paragraphs, lists, links, tables
 * (as rows of cells), preformatted text, entities and UTF-8 punctuation.  No CSS or scripts. */
#pragma once
#include "desk.h"

#define MAXLINK 512

struct item {
    u8 type, flags, scale, indent;
    u16 link;                       /* 0 = none, else index + 1 */
    u16 len;
    u32 off;                        /* into the page's text pool */
    u32 color;                      /* 0 = default */
};

struct box {
    int y;
    short x, w, h;
    u8 type, flags, scale;
    u16 link;
    u16 len;
    u32 off;
    u32 color;
};

struct page {
    char url[512];
    char title[96];
    char *pool;
    u32 pool_len, pool_cap;
    struct item *items;
    int nitems, cap_items;
    char (*links)[512];
    int nlinks;
    struct box *boxes;
    int nboxes, cap_boxes;
    int layout_w, height;
};

void page_free(struct page *p);
void page_clear(struct page *p);                         /* keeps nothing but the allocations */
void html_parse(struct page *p, const char *html, int len);
void text_parse(struct page *p, const char *text, int len);   /* plain text */
void page_layout(struct page *p, int width);
void page_draw(struct page *p, struct rect view, int scroll, int hover_link);
int page_link_at(struct page *p, int x, int y);          /* page coordinates; -1 if none */
