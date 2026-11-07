/* Image decoders (PNG, JPEG baseline + progressive, GIF) and box-filter scaling. */
#include "web.h"

#define MAX_PIXELS (8 * 1000 * 1000)

void img_free(struct image *im)
{
    if (!im)
        return;
    free(im->px);
    free(im->scaled);
    free(im);
}

static struct image *img_new(int w, int h)
{
    if (w <= 0 || h <= 0 || (long)w * h > MAX_PIXELS)
        return NULL;
    struct image *im = calloc(1, sizeof *im);
    if (!im)
        return NULL;
    im->px = malloc((u32)w * h * 4);
    if (!im->px) {
        free(im);
        return NULL;
    }
    im->w = w;
    im->h = h;
    return im;
}

/* ============================== inflate ============================== */

struct inf {
    const u8 *in;
    int len, pos;
    u32 bitbuf;
    int bitcnt;
    u8 *out;
    int cap, n, err;
};

struct huff {
    short count[16], symbol[288];
};

static int getbits(struct inf *s, int need)
{
    u32 v = s->bitbuf;
    while (s->bitcnt < need) {
        if (s->pos >= s->len) { s->err = 1; return 0; }
        v |= (u32)s->in[s->pos++] << s->bitcnt;
        s->bitcnt += 8;
    }
    s->bitbuf = v >> need;
    s->bitcnt -= need;
    return v & ((1u << need) - 1);
}

static int hdecode(struct inf *s, const struct huff *h)
{
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= getbits(s, 1);
        int count = h->count[len];
        if (code - count < first)
            return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->err) return -1;
    }
    return -1;
}

static int construct(struct huff *h, const short *length, int n)
{
    short offs[16];
    for (int i = 0; i <= 15; i++) h->count[i] = 0;
    for (int i = 0; i < n; i++) h->count[length[i]]++;
    if (h->count[0] == n) return 0;
    int left = 1;
    for (int len = 1; len <= 15; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return left;
    }
    offs[1] = 0;
    for (int len = 1; len < 15; len++) offs[len + 1] = offs[len] + h->count[len];
    for (int i = 0; i < n; i++)
        if (length[i]) h->symbol[offs[length[i]]++] = i;
    return left;
}

static const short LBASE[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
static const short LEXT[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
static const short DBASE[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
static const short DEXT[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

static int codes(struct inf *s, const struct huff *lc, const struct huff *dc)
{
    for (;;) {
        int sym = hdecode(s, lc);
        if (sym < 0) return -1;
        if (sym < 256) {
            if (s->n >= s->cap) return -1;
            s->out[s->n++] = sym;
        } else if (sym == 256) {
            return 0;
        } else {
            sym -= 257;
            if (sym >= 29) return -1;
            int len = LBASE[sym] + getbits(s, LEXT[sym]);
            int ds = hdecode(s, dc);
            if (ds < 0 || ds >= 30) return -1;
            int dist = DBASE[ds] + getbits(s, DEXT[ds]);
            if (s->err || dist > s->n || s->n + len > s->cap) return -1;
            for (int i = 0; i < len; i++, s->n++) s->out[s->n] = s->out[s->n - dist];
        }
    }
}

static int inflate_block(struct inf *s)
{
    static const short ORDER[19] = {16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    int last = getbits(s, 1), type = getbits(s, 2);
    if (type == 0) {
        s->bitbuf = 0; s->bitcnt = 0;
        if (s->pos + 4 > s->len) return -1;
        int len = s->in[s->pos] | s->in[s->pos + 1] << 8;
        s->pos += 4;
        if (s->pos + len > s->len || s->n + len > s->cap) return -1;
        memcpy(s->out + s->n, s->in + s->pos, len);
        s->pos += len; s->n += len;
    } else if (type == 1 || type == 2) {
        static struct huff lc, dc;
        short lengths[320];
        if (type == 1) {
            int i = 0;
            for (; i < 144; i++) lengths[i] = 8;
            for (; i < 256; i++) lengths[i] = 9;
            for (; i < 280; i++) lengths[i] = 7;
            for (; i < 288; i++) lengths[i] = 8;
            construct(&lc, lengths, 288);
            for (i = 0; i < 30; i++) lengths[i] = 5;
            construct(&dc, lengths, 30);
        } else {
            int nlen = getbits(s, 5) + 257, ndist = getbits(s, 5) + 1, ncode = getbits(s, 4) + 4;
            if (nlen > 286 || ndist > 30) return -1;
            int i;
            for (i = 0; i < ncode; i++) lengths[ORDER[i]] = getbits(s, 3);
            for (; i < 19; i++) lengths[ORDER[i]] = 0;
            if (construct(&lc, lengths, 19) != 0) return -1;
            i = 0;
            while (i < nlen + ndist) {
                int sym = hdecode(s, &lc);
                if (sym < 0) return -1;
                if (sym < 16) lengths[i++] = sym;
                else {
                    int prev = 0, rep;
                    if (sym == 16) { if (!i) return -1; prev = lengths[i - 1]; rep = 3 + getbits(s, 2); }
                    else if (sym == 17) rep = 3 + getbits(s, 3);
                    else rep = 11 + getbits(s, 7);
                    if (i + rep > nlen + ndist) return -1;
                    while (rep--) lengths[i++] = prev;
                }
            }
            if (lengths[256] == 0) return -1;
            construct(&lc, lengths, nlen);
            construct(&dc, lengths + nlen, ndist);
        }
        if (codes(s, &lc, &dc)) return -1;
    } else return -1;
    return last ? 1 : 0;
}

/* zlib stream -> out; returns the number of bytes or -1 */
static int zlib_inflate(const u8 *in, int len, u8 *out, int cap)
{
    if (len < 2) return -1;
    struct inf s = {.in = in + 2, .len = len - 2, .out = out, .cap = cap};
    for (;;) {
        int r = inflate_block(&s);
        if (r < 0 || s.err) return s.n > 0 && s.err ? s.n : -1;
        if (r == 1) break;
    }
    return s.n;
}

/* ============================== PNG ============================== */

static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }

static struct image *png_decode(const u8 *d, int len)
{
    if (len < 33 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return NULL;
    int w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    u8 pal[256][4];
    int npal = 0, have_key = 0;
    u16 key[3] = {0};
    u8 *idat = NULL;
    int idat_len = 0;
    for (int p = 8; p + 12 <= len;) {
        u32 cl = be32(d + p);
        const u8 *tag = d + p + 4, *body = d + p + 8;
        if (p + 12 + (long)cl > len) break;
        if (!memcmp(tag, "IHDR", 4) && cl >= 13) {
            w = be32(body); h = be32(body + 4); depth = body[8]; ctype = body[9]; interlace = body[12];
        } else if (!memcmp(tag, "PLTE", 4)) {
            npal = cl / 3 > 256 ? 256 : cl / 3;
            for (int i = 0; i < npal; i++) { pal[i][0] = body[i * 3]; pal[i][1] = body[i * 3 + 1]; pal[i][2] = body[i * 3 + 2]; pal[i][3] = 255; }
        } else if (!memcmp(tag, "tRNS", 4)) {
            if (ctype == 3) for (u32 i = 0; i < cl && i < 256; i++) pal[i][3] = body[i];
            else if (cl >= 2) { have_key = 1; for (int i = 0; i < 3 && (u32)(i * 2 + 1) < cl; i++) key[i] = body[i * 2] << 8 | body[i * 2 + 1]; }
        } else if (!memcmp(tag, "IDAT", 4)) {
            u8 *n = realloc(idat, idat_len + cl);
            if (!n) { free(idat); return NULL; }
            idat = n;
            memcpy(idat + idat_len, body, cl);
            idat_len += cl;
        } else if (!memcmp(tag, "IEND", 4)) break;
        p += 12 + cl;
    }
    int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    int bpp_bits = chans * depth;
    struct image *im = NULL;
    if (!idat || w <= 0 || h <= 0 || (long)w * h > MAX_PIXELS || (ctype != 0 && ctype != 2 && ctype != 3 && ctype != 4 && ctype != 6) ||
        (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16))
        goto done;
    {
        long raw = 0;
        if (!interlace) raw = (long)h * (1 + (((long)w * bpp_bits + 7) >> 3));
        else raw = (long)h * (1 + (((long)w * bpp_bits + 7) >> 3)) + h * 8;
        u8 *buf = malloc(raw);
        if (!buf) goto done;
        int n = zlib_inflate(idat, idat_len, buf, raw);
        if (n <= 0) { free(buf); goto done; }
        im = img_new(w, h);
        if (!im) { free(buf); goto done; }
        memset(im->px, 0, (u32)w * h * 4);
        int bpp = bpp_bits >= 8 ? bpp_bits / 8 : 1;
        static const int sx[7] = {0, 4, 0, 2, 0, 1, 0}, sy[7] = {0, 0, 4, 0, 2, 0, 1}, dx[7] = {8, 8, 4, 4, 2, 2, 1}, dy[7] = {8, 8, 8, 4, 4, 2, 2};
        int npass = interlace ? 7 : 1, off = 0;
        for (int pass = 0; pass < npass; pass++) {
            int pw = interlace ? (w - sx[pass] + dx[pass] - 1) / dx[pass] : w, ph = interlace ? (h - sy[pass] + dy[pass] - 1) / dy[pass] : h;
            if (pw <= 0 || ph <= 0) continue;
            int stride = (pw * bpp_bits + 7) >> 3;
            u8 *prev = NULL;
            for (int y = 0; y < ph; y++) {
                if (off + 1 + stride > n) break;
                u8 *row = buf + off + 1;
                int f = buf[off];
                for (int i = 0; i < stride; i++) {
                    int a = i >= bpp ? row[i - bpp] : 0, b = prev ? prev[i] : 0, c = prev && i >= bpp ? prev[i - bpp] : 0, v = row[i];
                    switch (f) {
                    case 1: v += a; break;
                    case 2: v += b; break;
                    case 3: v += (a + b) / 2; break;
                    case 4: { int p = a + b - c, pa = abs_i(p - a), pb = abs_i(p - b), pc = abs_i(p - c); v += pa <= pb && pa <= pc ? a : pb <= pc ? b : c; break; }
                    }
                    row[i] = v;
                }
                for (int x = 0; x < pw; x++) {
                    u32 r, g, bl, a = 255;
                    if (depth == 8 || depth == 16) {
                        const u8 *px = row + x * chans * (depth / 8);
                        int st = depth / 8;
                        if (ctype == 0) { r = g = bl = px[0]; if (have_key && (depth == 8 ? px[0] == (key[0] & 255) : (px[0] << 8 | px[1]) == key[0])) a = 0; }
                        else if (ctype == 2) { r = px[0]; g = px[st]; bl = px[2 * st]; if (have_key && (depth == 8 ? (px[0] == (key[0] & 255) && px[1] == (key[1] & 255) && px[2] == (key[2] & 255)) : 0)) a = 0; }
                        else if (ctype == 3) { int ix = px[0]; if (ix >= 256) ix = 0; r = pal[ix][0]; g = pal[ix][1]; bl = pal[ix][2]; a = pal[ix][3]; }
                        else if (ctype == 4) { r = g = bl = px[0]; a = px[st]; }
                        else { r = px[0]; g = px[st]; bl = px[2 * st]; a = px[3 * st]; }
                    } else {
                        int per = 8 / depth, v = (row[x / per] >> (8 - depth * (x % per + 1))) & ((1 << depth) - 1);
                        if (ctype == 3) { r = pal[v][0]; g = pal[v][1]; bl = pal[v][2]; a = pal[v][3]; }
                        else { r = g = bl = v * 255 / ((1 << depth) - 1); if (have_key && v == key[0]) a = 0; }
                    }
                    int ox = interlace ? sx[pass] + x * dx[pass] : x, oy = interlace ? sy[pass] + y * dy[pass] : y;
                    im->px[oy * w + ox] = a << 24 | r << 16 | g << 8 | bl;
                }
                prev = row;
                off += 1 + stride;
            }
        }
        free(buf);
    }
done:
    free(idat);
    return im;
}

/* ============================== GIF ============================== */

static struct image *gif_decode(const u8 *d, int len)
{
    if (len < 13 || memcmp(d, "GIF8", 4)) return NULL;
    int sw = d[6] | d[7] << 8, sh = d[8] | d[9] << 8, flags = d[10], p = 13;
    u8 gct[256][3];
    int ngct = 0;
    if (flags & 0x80) {
        ngct = 2 << (flags & 7);
        if (p + ngct * 3 > len) return NULL;
        for (int i = 0; i < ngct; i++) memcpy(gct[i], d + p + i * 3, 3);
        p += ngct * 3;
    }
    int trans = -1;
    while (p < len) {
        int b = d[p++];
        if (b == 0x21) {
            int label = p < len ? d[p++] : 0;
            if (label == 0xF9 && p + 5 < len && d[p] >= 4) { if (d[p + 1] & 1) trans = d[p + 4]; }
            while (p < len && d[p]) p += d[p] + 1;
            p++;
        } else if (b == 0x2C) {
            if (p + 9 > len) return NULL;
            int ix = d[p] | d[p + 1] << 8, iy = d[p + 2] | d[p + 3] << 8, iw = d[p + 4] | d[p + 5] << 8, ih = d[p + 6] | d[p + 7] << 8, lf = d[p + 8];
            p += 9;
            u8 lct[256][3];
            int nl = 0;
            if (lf & 0x80) {
                nl = 2 << (lf & 7);
                if (p + nl * 3 > len) return NULL;
                for (int i = 0; i < nl; i++) memcpy(lct[i], d + p + i * 3, 3);
                p += nl * 3;
            }
            const u8 (*ct)[3] = nl ? lct : gct;
            int nct = nl ? nl : ngct;
            if (p >= len) return NULL;
            int minc = d[p++];
            if (minc < 2 || minc > 11 || iw <= 0 || ih <= 0 || (long)iw * ih > MAX_PIXELS) return NULL;
            u8 *data = malloc(len - p + 1);
            int dl = 0;
            if (!data) return NULL;
            while (p < len && d[p]) { int n = d[p++]; if (p + n > len) n = len - p; memcpy(data + dl, d + p, n); dl += n; p += n; }
            u8 *idx = calloc(iw * ih, 1);
            if (!idx) { free(data); return NULL; }
            /* LZW */
            static u16 prefix[4096];
            static u8 suffix[4096], stack[4097];
            int clear = 1 << minc, eoi = clear + 1, next = clear + 2, size = minc + 1, old = -1, bits = 0, acc = 0, pos = 0, di = 0;
            u8 first = 0;
            for (int i = 0; i < clear; i++) { prefix[i] = 0; suffix[i] = i; }
            while (di < iw * ih) {
                while (bits < size && pos < dl) { acc |= data[pos++] << bits; bits += 8; }
                if (bits < size) break;
                int code = acc & ((1 << size) - 1);
                acc >>= size; bits -= size;
                if (code == clear) { size = minc + 1; next = clear + 2; old = -1; continue; }
                if (code == eoi) break;
                int sp = 0, cur = code;
                if (old < 0) { idx[di++] = suffix[code]; old = code; first = suffix[code]; continue; }
                if (code >= next) { stack[sp++] = first; cur = old; }
                while (cur >= clear && sp < 4096) { stack[sp++] = suffix[cur]; cur = prefix[cur]; }
                first = suffix[cur];
                stack[sp++] = first;
                while (sp && di < iw * ih) idx[di++] = stack[--sp];
                if (next < 4096) {
                    prefix[next] = old; suffix[next] = first; next++;
                    if (next == (1 << size) && size < 12) size++;
                }
                old = code;
            }
            free(data);
            struct image *im = img_new(sw > 0 ? sw : iw, sh > 0 ? sh : ih);
            if (!im) { free(idx); return NULL; }
            memset(im->px, 0, (u32)im->w * im->h * 4);
            int interlaced = lf & 0x40, row = 0, pass = 0;
            static const int start[4] = {0, 4, 2, 1}, step[4] = {8, 8, 4, 2};
            if (interlaced) row = 0;
            for (int y = 0; y < ih; y++) {
                int oy;
                if (interlaced) {
                    oy = row;
                    row += step[pass];
                    while (row >= ih && pass < 3) { pass++; row = start[pass]; }
                } else oy = y;
                for (int x = 0; x < iw; x++) {
                    int c = idx[y * iw + x];
                    int X = ix + x, Y = iy + oy;
                    if (c == trans || c >= nct || X >= im->w || Y >= im->h) continue;
                    im->px[Y * im->w + X] = 0xFF000000u | ct[c][0] << 16 | ct[c][1] << 8 | ct[c][2];
                }
            }
            free(idx);
            return im;
        } else break;
    }
    return NULL;
}

/* ============================== JPEG ============================== */

static const u8 ZZ[64] = {0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5, 12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
                          35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63};

struct jh {
    u8 bits[17], val[256];
    int mincode[17], maxcode[18], valptr[17];
    int ok;
};

struct jcomp {
    int id, h, v, tq, bw, bh, dcpred;                  /* bw/bh: blocks across/down (padded to whole MCUs) */
    int td, ta;
    short *coef;
};

struct jpeg {
    const u8 *d;
    int len, pos;
    int w, h, ncomp, progressive, restart;
    u16 q[4][64];
    struct jh dc[4], ac[4];
    struct jcomp comp[4];
    int hmax, vmax, mcux, mcuy;
    u32 bitbuf;
    int bitcnt, eobrun, hit_marker;
};

static void build_huff(struct jh *h)
{
    int code = 0, k = 0;
    for (int l = 1; l <= 16; l++) {
        h->valptr[l] = k;
        h->mincode[l] = code;
        code += h->bits[l];
        k += h->bits[l];
        h->maxcode[l] = h->bits[l] ? code - 1 : -1;
        code <<= 1;
    }
    h->maxcode[17] = 0x7fffffff;
    h->ok = 1;
}

static int jbit(struct jpeg *j)
{
    if (j->bitcnt == 0) {
        int b = 0;
        if (j->hit_marker || j->pos >= j->len) { j->hit_marker = 1; }
        else {
            b = j->d[j->pos++];
            if (b == 0xFF) {
                int b2 = j->pos < j->len ? j->d[j->pos] : 0xD9;
                if (b2 == 0) j->pos++;
                else { j->pos--; j->hit_marker = 1; b = 0; }
            }
        }
        j->bitbuf = b;
        j->bitcnt = 8;
    }
    j->bitcnt--;
    return j->bitbuf >> j->bitcnt & 1;
}

static int jbits(struct jpeg *j, int n)
{
    int v = 0;
    while (n--) v = v << 1 | jbit(j);
    return v;
}

static int jdecode(struct jpeg *j, const struct jh *h)
{
    int code = 0;
    for (int l = 1; l <= 16; l++) {
        code = code << 1 | jbit(j);
        if (h->maxcode[l] >= 0 && code <= h->maxcode[l] && code >= h->mincode[l])
            return h->val[h->valptr[l] + code - h->mincode[l]];
    }
    return 0;
}

static int extend(int v, int t) { return t && v < (1 << (t - 1)) ? v - (1 << t) + 1 : v; }

static void j_block(struct jpeg *j, struct jcomp *c, short *blk, int ss, int se, int ah, int al)
{
    if (!j->progressive) {
        int t = jdecode(j, &j->dc[c->td]);
        int diff = t ? extend(jbits(j, t), t) : 0;
        c->dcpred += diff;
        blk[0] = c->dcpred;
        for (int k = 1; k < 64; k++) {
            int rs = jdecode(j, &j->ac[c->ta]), s = rs & 15, r = rs >> 4;
            if (!s) { if (r != 15) break; k += 15; continue; }
            k += r;
            if (k > 63) break;
            blk[ZZ[k]] = extend(jbits(j, s), s);
        }
        return;
    }
    if (ss == 0) {                                                  /* DC scans */
        if (ah == 0) {
            int t = jdecode(j, &j->dc[c->td]);
            int diff = t ? extend(jbits(j, t), t) : 0;
            c->dcpred += diff;
            blk[0] = c->dcpred * (1 << al);
        } else if (jbit(j)) blk[0] |= 1 << al;
        return;
    }
    if (ah == 0) {                                                  /* AC first */
        if (j->eobrun) { j->eobrun--; return; }
        for (int k = ss; k <= se; k++) {
            int rs = jdecode(j, &j->ac[c->ta]), s = rs & 15, r = rs >> 4;
            if (!s) {
                if (r < 15) { j->eobrun = (1 << r) - 1; if (r) j->eobrun += jbits(j, r); break; }
                k += 15;
                continue;
            }
            k += r;
            if (k > 63) break;
            blk[ZZ[k]] = extend(jbits(j, s), s) * (1 << al);
        }
        return;
    }
    int p1 = 1 << al, m1 = -1 * (1 << al), k = ss;                   /* AC refinement */
    if (!j->eobrun) {
        for (; k <= se; k++) {
            int rs = jdecode(j, &j->ac[c->ta]), s = rs & 15, r = rs >> 4;
            if (s) s = jbit(j) ? p1 : m1;
            else if (r != 15) { j->eobrun = 1 << r; if (r) j->eobrun += jbits(j, r); break; }
            do {
                short *cf = &blk[ZZ[k]];
                if (*cf) { if (jbit(j) && !(*cf & p1)) *cf += *cf >= 0 ? p1 : m1; }
                else if (--r < 0) break;
                k++;
            } while (k <= se);
            if (s && k <= 63) blk[ZZ[k]] = s;
        }
    }
    if (j->eobrun) {
        for (; k <= se; k++) {
            short *cf = &blk[ZZ[k]];
            if (*cf && jbit(j) && !(*cf & p1)) *cf += *cf >= 0 ? p1 : m1;
        }
        j->eobrun--;
    }
}

static int j_scan(struct jpeg *j, const u8 *sc, int ns, int ss, int se, int ah, int al)
{
    struct jcomp *cs[4];
    for (int i = 0; i < ns; i++) {
        int id = sc[i * 2], tbl = sc[i * 2 + 1];
        cs[i] = NULL;
        for (int c = 0; c < j->ncomp; c++)
            if (j->comp[c].id == id) { cs[i] = &j->comp[c]; cs[i]->td = tbl >> 4 & 3; cs[i]->ta = tbl & 3; }
        if (!cs[i]) return -1;
    }
    for (int c = 0; c < j->ncomp; c++) j->comp[c].dcpred = 0;
    j->bitcnt = 0; j->hit_marker = 0; j->eobrun = 0;
    int rst = 0;
    int bx_n, by_n;
    if (ns == 1) {
        struct jcomp *c = cs[0];
        bx_n = ((j->w * c->h + j->hmax - 1) / j->hmax + 7) / 8;
        by_n = ((j->h * c->v + j->vmax - 1) / j->vmax + 7) / 8;
    } else { bx_n = j->mcux; by_n = j->mcuy; }
    for (int my = 0; my < by_n; my++)
        for (int mx = 0; mx < bx_n; mx++) {
            if (j->restart && rst == j->restart) {
                j->bitcnt = 0; j->hit_marker = 0;
                while (j->pos + 1 < j->len && !(j->d[j->pos] == 0xFF && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7)) {
                    if (j->d[j->pos] == 0xFF && j->d[j->pos + 1] != 0 && j->d[j->pos + 1] != 0xFF) break;
                    j->pos++;
                }
                if (j->pos + 1 < j->len && j->d[j->pos + 1] >= 0xD0 && j->d[j->pos + 1] <= 0xD7) j->pos += 2;
                for (int c = 0; c < j->ncomp; c++) j->comp[c].dcpred = 0;
                j->eobrun = 0;
                rst = 0;
            }
            if (ns == 1) {
                struct jcomp *c = cs[0];
                j_block(j, c, c->coef + ((long)my * c->bw + mx) * 64, ss, se, ah, al);
            } else {
                for (int i = 0; i < ns; i++) {
                    struct jcomp *c = cs[i];
                    for (int v = 0; v < c->v; v++)
                        for (int h = 0; h < c->h; h++)
                            j_block(j, c, c->coef + ((long)(my * c->v + v) * c->bw + mx * c->h + h) * 64, ss, se, ah, al);
                }
            }
            rst++;
        }
    return 0;
}

#define F2F(x) ((int)((x) * 4096 + 0.5))
#define FSH(x) ((x) * 4096)
#define IDCT_1D(s0, s1, s2, s3, s4, s5, s6, s7)                                                  \
    int t0, t1, t2, t3, p1, p2, p3, p4, p5, x0, x1, x2, x3;                                      \
    p2 = s2; p3 = s6;                                                                            \
    p1 = (p2 + p3) * F2F(0.5411961f);                                                            \
    t2 = p1 + p3 * F2F(-1.847759065f);                                                           \
    t3 = p1 + p2 * F2F(0.765366865f);                                                            \
    p2 = s0; p3 = s4;                                                                            \
    t0 = FSH(p2 + p3);                                                                           \
    t1 = FSH(p2 - p3);                                                                           \
    x0 = t0 + t3; x3 = t0 - t3; x1 = t1 + t2; x2 = t1 - t2;                                      \
    t0 = s7; t1 = s5; t2 = s3; t3 = s1;                                                          \
    p3 = t0 + t2; p4 = t1 + t3; p1 = t0 + t3; p2 = t1 + t2;                                      \
    p5 = (p3 + p4) * F2F(1.175875602f);                                                          \
    t0 = t0 * F2F(0.298631336f); t1 = t1 * F2F(2.053119869f);                                    \
    t2 = t2 * F2F(3.072711026f); t3 = t3 * F2F(1.501321110f);                                    \
    p1 = p5 + p1 * F2F(-0.899976223f); p2 = p5 + p2 * F2F(-2.562915447f);                        \
    p3 = p3 * F2F(-1.961570560f); p4 = p4 * F2F(-0.390180644f);                                  \
    t3 += p1 + p4; t2 += p2 + p3; t1 += p2 + p4; t0 += p1 + p3;

static u8 clamp8(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

static void idct(const short *in, const u16 *q, u8 *out, int stride)
{
    int d[64], v[64];
    for (int i = 0; i < 64; i++) d[i] = in[i] * q[i];                   /* q is in natural order */
    for (int i = 0; i < 8; i++) {
        IDCT_1D(d[i], d[i + 8], d[i + 16], d[i + 24], d[i + 32], d[i + 40], d[i + 48], d[i + 56])
        x0 += 512; x1 += 512; x2 += 512; x3 += 512;
        v[i] = (x0 + t3) >> 10; v[i + 56] = (x0 - t3) >> 10; v[i + 8] = (x1 + t2) >> 10; v[i + 48] = (x1 - t2) >> 10;
        v[i + 16] = (x2 + t1) >> 10; v[i + 40] = (x2 - t1) >> 10; v[i + 24] = (x3 + t0) >> 10; v[i + 32] = (x3 - t0) >> 10;
    }
    for (int i = 0; i < 8; i++) {
        u8 *o = out + i * stride;
        IDCT_1D(v[i * 8], v[i * 8 + 1], v[i * 8 + 2], v[i * 8 + 3], v[i * 8 + 4], v[i * 8 + 5], v[i * 8 + 6], v[i * 8 + 7])
        x0 += 65536 + (128 << 17); x1 += 65536 + (128 << 17); x2 += 65536 + (128 << 17); x3 += 65536 + (128 << 17);
        o[0] = clamp8((x0 + t3) >> 17); o[7] = clamp8((x0 - t3) >> 17); o[1] = clamp8((x1 + t2) >> 17); o[6] = clamp8((x1 - t2) >> 17);
        o[2] = clamp8((x2 + t1) >> 17); o[5] = clamp8((x2 - t1) >> 17); o[3] = clamp8((x3 + t0) >> 17); o[4] = clamp8((x3 - t0) >> 17);
    }
}

static struct image *jpeg_decode(const u8 *d, int len)
{
    if (len < 4 || d[0] != 0xFF || d[1] != 0xD8) return NULL;
    struct jpeg *j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->d = d; j->len = len; j->pos = 2;
    struct image *im = NULL;
    int frame = 0, bad = 0;
    while (j->pos + 4 <= len && !bad) {
        if (d[j->pos] != 0xFF) { j->pos++; continue; }
        int m = d[j->pos + 1];
        if (m == 0xFF) { j->pos++; continue; }
        if (m == 0xD8 || (m >= 0xD0 && m <= 0xD7) || m == 0x01 || m == 0) { j->pos += 2; continue; }
        if (m == 0xD9) break;
        int seg = d[j->pos + 2] << 8 | d[j->pos + 3];
        const u8 *s = d + j->pos + 4;
        int sl = seg - 2;
        if (seg < 2 || j->pos + 2 + seg > len) break;
        j->pos += 2 + seg;
        if (m == 0xDB) {
            for (int p = 0; p < sl;) {
                int pq = s[p] >> 4, tq = s[p] & 3;
                p++;
                for (int i = 0; i < 64 && p < sl; i++) {
                    int v = pq ? (s[p] << 8 | s[p + 1]) : s[p];
                    p += pq ? 2 : 1;
                    j->q[tq][ZZ[i]] = v;
                }
            }
        } else if (m == 0xC4) {
            for (int p = 0; p + 17 <= sl;) {
                int tc = s[p] >> 4, th = s[p] & 3, n = 0;
                struct jh *h = tc ? &j->ac[th] : &j->dc[th];
                memset(h, 0, sizeof *h);
                for (int i = 1; i <= 16; i++) { h->bits[i] = s[p + i]; n += s[p + i]; }
                p += 17;
                if (n > 256 || p + n > sl) { bad = 1; break; }
                memcpy(h->val, s + p, n);
                p += n;
                build_huff(h);
            }
        } else if (m == 0xDD) {
            j->restart = s[0] << 8 | s[1];
        } else if (m == 0xC0 || m == 0xC1 || m == 0xC2) {
            if (frame || sl < 6 || s[0] != 8) { bad = 1; break; }
            frame = 1;
            j->progressive = m == 0xC2;
            j->h = s[1] << 8 | s[2];
            j->w = s[3] << 8 | s[4];
            j->ncomp = s[5];
            if (j->ncomp != 1 && j->ncomp != 3) { bad = 1; break; }
            if ((long)j->w * j->h > MAX_PIXELS || j->w <= 0 || j->h <= 0) { bad = 1; break; }
            for (int i = 0; i < j->ncomp; i++) {
                j->comp[i].id = s[6 + i * 3];
                j->comp[i].h = s[7 + i * 3] >> 4;
                j->comp[i].v = s[7 + i * 3] & 15;
                j->comp[i].tq = s[8 + i * 3] & 3;
                if (j->comp[i].h < 1 || j->comp[i].h > 4 || j->comp[i].v < 1 || j->comp[i].v > 4) bad = 1;
                if (j->comp[i].h > j->hmax) j->hmax = j->comp[i].h;
                if (j->comp[i].v > j->vmax) j->vmax = j->comp[i].v;
            }
            if (bad) break;
            j->mcux = (j->w + 8 * j->hmax - 1) / (8 * j->hmax);
            j->mcuy = (j->h + 8 * j->vmax - 1) / (8 * j->vmax);
            for (int i = 0; i < j->ncomp; i++) {
                struct jcomp *c = &j->comp[i];
                c->bw = j->mcux * c->h;
                c->bh = j->mcuy * c->v;
                c->coef = calloc((long)c->bw * c->bh * 64, sizeof(short));
                if (!c->coef) { bad = 1; break; }
            }
        } else if (m == 0xC3 || (m >= 0xC5 && m <= 0xCF && m != 0xC8 && m != 0xCC)) {
            bad = 1;                                                    /* lossless / arithmetic: unsupported */
        } else if (m == 0xDA) {
            if (!frame) { bad = 1; break; }
            int ns = s[0];
            if (ns < 1 || ns > 4 || sl < 1 + ns * 2 + 3) { bad = 1; break; }
            int ss = s[1 + ns * 2], se = s[2 + ns * 2], ah = s[3 + ns * 2] >> 4, al = s[3 + ns * 2] & 15;
            if (!j->progressive) { ss = 0; se = 63; ah = al = 0; }
            if (se > 63) se = 63;
            if (j_scan(j, s + 1, ns, ss, se, ah, al)) { bad = 1; break; }
            while (j->pos + 1 < len && !(d[j->pos] == 0xFF && d[j->pos + 1] != 0 && !(d[j->pos + 1] >= 0xD0 && d[j->pos + 1] <= 0xD7)))
                j->pos++;
        }
    }
    if (!frame || bad) goto out;
    {
        int W = j->w, H = j->h;
        im = img_new(W, H);
        if (!im) goto out;
        u8 *planes[4] = {0};
        int pw[4], ph[4];
        for (int i = 0; i < j->ncomp; i++) {
            struct jcomp *c = &j->comp[i];
            pw[i] = c->bw * 8; ph[i] = c->bh * 8;
            planes[i] = malloc((long)pw[i] * ph[i]);
            if (!planes[i]) { img_free(im); im = NULL; goto planes_done; }
            for (int by = 0; by < c->bh; by++)
                for (int bx = 0; bx < c->bw; bx++)
                    idct(c->coef + ((long)by * c->bw + bx) * 64, j->q[c->tq], planes[i] + (by * 8) * pw[i] + bx * 8, pw[i]);
        }
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int Y[3];
                for (int i = 0; i < j->ncomp; i++) {
                    struct jcomp *c = &j->comp[i];
                    int sx = x * c->h / j->hmax, sy = y * c->v / j->vmax;
                    Y[i] = planes[i][sy * pw[i] + sx];
                }
                if (j->ncomp == 1) { im->px[y * W + x] = 0xFF000000u | Y[0] << 16 | Y[0] << 8 | Y[0]; continue; }
                int cb = Y[1] - 128, cr = Y[2] - 128;
                int r = Y[0] + ((91881 * cr) >> 16), g = Y[0] - ((22554 * cb + 46802 * cr) >> 16), b = Y[0] + ((116130 * cb) >> 16);
                im->px[y * W + x] = 0xFF000000u | clamp8(r) << 16 | clamp8(g) << 8 | clamp8(b);
            }
    planes_done:
        for (int i = 0; i < 4; i++) free(planes[i]);
    }
out:
    for (int i = 0; i < 4; i++) free(j->comp[i].coef);
    free(j);
    return im;
}

/* ============================== entry points ============================== */

struct image *img_decode(const u8 *d, int len)
{
    if (len > 8 && d[0] == 0x89 && d[1] == 'P') return png_decode(d, len);
    if (len > 3 && d[0] == 0xFF && d[1] == 0xD8) return jpeg_decode(d, len);
    if (len > 6 && d[0] == 'G' && d[1] == 'I' && d[2] == 'F') return gif_decode(d, len);
    return NULL;
}

/* Box-filtered copy at w*h, premultiplied (what gfx_blit_argb wants). */
const u32 *img_scaled(struct image *im, int w, int h)
{
    if (w <= 0 || h <= 0 || (long)w * h > 6000000) return NULL;
    if (im->scaled && im->sw == w && im->sh == h) return im->scaled;
    u32 *out = malloc((u32)w * h * 4);
    if (!out) return NULL;
    for (int y = 0; y < h; y++) {
        int y0 = (long)y * im->h / h, y1 = (long)(y + 1) * im->h / h;
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < w; x++) {
            int x0 = (long)x * im->w / w, x1 = (long)(x + 1) * im->w / w;
            if (x1 <= x0) x1 = x0 + 1;
            u32 a = 0, r = 0, g = 0, b = 0, n = 0;
            for (int sy = y0; sy < y1; sy++)
                for (int sx = x0; sx < x1; sx++) {
                    u32 p = im->px[sy * im->w + sx], pa = p >> 24;
                    a += pa; r += (p >> 16 & 255) * pa; g += (p >> 8 & 255) * pa; b += (p & 255) * pa; n++;
                }
            if (a == 0) { out[y * w + x] = 0; continue; }
            /* r,g,b are alpha-weighted sums: premultiplied color = sum / (255 * n) */
            u32 pa = a / n;
            out[y * w + x] = pa << 24 | (r / (255 * n)) << 16 | (g / (255 * n)) << 8 | (b / (255 * n));
        }
    }
    free(im->scaled);
    im->scaled = out;
    im->sw = w;
    im->sh = h;
    return out;
}
