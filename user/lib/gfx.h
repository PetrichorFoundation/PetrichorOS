/* A small 2D drawing library: clipped fills, text, gradients, alpha blending. */
#pragma once
#include "ulib.h"

struct rect {
    int x, y, w, h;
};

#define RGB(r, g, b) (((u32)(r) << 16) | ((u32)(g) << 8) | (u32)(b))

void gfx_set_target(u32 *px, int w, int h);          /* pixels are 0x00RRGGBB, stride = w */
void gfx_set_clip(int x, int y, int w, int h);       /* everything below is clipped to this */
struct rect gfx_get_clip(void);

u32 gfx_mix(u32 a, u32 b, int t);                    /* t = 0..256: a -> b */
void gfx_fill(int x, int y, int w, int h, u32 c);
void gfx_hline(int x, int y, int w, u32 c);
void gfx_vline(int x, int y, int h, u32 c);
void gfx_frame(int x, int y, int w, int h, u32 c);   /* 1px outline */
void gfx_vgrad(int x, int y, int w, int h, u32 top, u32 bottom);
void gfx_blend(int x, int y, int w, int h, u32 c, int alpha);   /* alpha 0..255 */
void gfx_copy(const u32 *src, int x, int y, int w, int h);      /* src has the target's size */
void gfx_blit_argb(const u32 *px, int x, int y, int w, int h);  /* premultiplied 0xAARRGGBB, w*h pixels */

void gfx_blit_a8(const u8 *a, int x, int y, int w, int h, u32 color);   /* color through an 8-bit coverage mask */
void gfx_hgrad(int x, int y, int w, int h, u32 left, u32 right);
void gfx_round_rect(int x, int y, int w, int h, int r, u32 argb, int ring);   /* AA; ring > 0: border only */

void gfx_char(int x, int y, int ch, u32 fg);
int gfx_text(int x, int y, const char *s, u32 fg);              /* returns x after the text */
int gfx_text_n(int x, int y, const char *s, int n, u32 fg);
int gfx_text_width(const char *s);
void gfx_text_scaled(int x, int y, const char *s, u32 fg, int scale);

struct rect rect_union(struct rect a, struct rect b);
struct rect rect_isect(struct rect a, struct rect b);
int rect_hit(struct rect r, int x, int y);
