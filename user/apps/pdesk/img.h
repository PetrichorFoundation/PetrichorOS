/* Image decoders (img.c): PNG, JPEG and GIF, with a cached scaled copy.  Used by the image viewer. */
#pragma once
#include "desk.h"

struct image {
    int w, h;
    u32 *px;                                       /* 0xAARRGGBB straight alpha */
    int sw, sh;                                    /* cached scaled copy */
    u32 *scaled;
};

struct image *img_decode(const u8 *data, int len);
const u32 *img_scaled(struct image *im, int w, int h);
void img_free(struct image *im);
