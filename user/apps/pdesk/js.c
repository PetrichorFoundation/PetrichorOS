/* JavaScript for the browser: a tree-walking interpreter for the everyday part of the language.
 *
 * Supported: var/let/const, functions, arrow functions, closures, default and rest parameters, objects,
 * arrays, strings, template literals, classes (with extends and super), prototypes and `new`, `this`,
 * if/for/for-in/for-of/while/do/switch, break/continue, try/catch/finally/throw, typeof, delete, the usual
 * operators (including ?., ??, **, spread in calls and array literals), and a standard library: Object,
 * Array, String, Number, Math, JSON, parseInt/parseFloat, Date.now, console.
 * Not supported: regular expressions (a literal works as a plain substring pattern), getters/setters,
 * destructuring, generators, async/await and promises, labels, eval.
 *
 * Memory comes from a per-page bump allocator that is dropped when the page goes away (no garbage
 * collector), with a cap; a script that runs too long or uses too much memory is stopped. */
#include "jsint.h"
#include <stdarg.h>

struct jsctx *J;

#define MEM_CHUNK  (256 * 1024)
#define MEM_CAP    (96u * 1024 * 1024)
#define INTERN_N   4096
#define MAX_DEPTH  200

struct jsmem {
    struct jsmem *next;
    int used, cap;
    char data[];
};

/* ============================== memory ============================== */

void *jalloc(int n)
{
    n = (n + 15) & ~15;
    struct jsmem *m = J->mem;
    if (!m || m->used + n > m->cap) {
        int cap = n > MEM_CHUNK ? n : MEM_CHUNK;
        if (J->used + cap > MEM_CAP) {
            J->aborted = 1;
            return NULL;
        }
        struct jsmem *nm = malloc(sizeof *nm + cap);
        if (!nm) {
            J->aborted = 1;
            return NULL;
        }
        J->used += cap;
        nm->next = m;
        nm->used = 0;
        nm->cap = cap;
        J->mem = m = nm;
    }
    void *r = m->data + m->used;
    m->used += n;
    memset(r, 0, n);
    return r;
}

/* Out of memory: hand back a scratch block so callers don't crash; the script is stopped right after. */
static void *xalloc(int n)
{
    void *p = jalloc(n);
    if (p)
        return p;
    static char spare[4096] __attribute__((aligned(16)));
    if (n <= (int)sizeof spare) {
        memset(spare, 0, n);
        return spare;
    }
    return NULL;
}

/* ============================== values ============================== */

V undef(void) { V v; v.t = VU; v.n = 0; return v; }
V vnull(void) { V v; v.t = VNULL; v.n = 0; return v; }
V vbool(int b) { V v; v.t = VBOOL; v.n = 0; v.b = b != 0; return v; }
V vnum(double d) { V v; v.t = VNUM; v.n = d; return v; }
V vstr(Str *s) { V v; v.t = VSTR; v.s = s; return v; }
V vobj(Obj *o) { V v; if (!o) return vnull(); v.t = VOBJ; v.o = o; return v; }
V arg(V *a, int n, int i) { return i < n ? a[i] : undef(); }
int is_obj(V v, int cls) { return v.t == VOBJ && v.o->cls == cls; }

static u32 hash_bytes(const char *s, int n)
{
    u32 h = 2166136261u;
    for (int i = 0; i < n; i++)
        h = (h ^ (u8)s[i]) * 16777619u;
    return h;
}

Str *str_new(const char *s, int len)
{
    if (len < 0) len = 0;
    Str *r = xalloc(sizeof *r + len + 1);
    if (!r) {
        static Str empty;
        return &empty;
    }
    r->len = len;
    if (s && len)
        memcpy(r->c, s, len);
    r->c[len] = 0;
    return r;
}

Str *intern(const char *s, int len)
{
    u32 h = hash_bytes(s, len);
    Str **b = &J->intern[h % INTERN_N];
    for (Str *x = *b; x; x = x->inext)
        if (x->hash == h && x->len == len && !memcmp(x->c, s, len))
            return x;
    Str *r = str_new(s, len);
    r->hash = h;
    r->interned = 1;
    r->inext = *b;
    *b = r;
    return r;
}

Str *intern_c(const char *s) { return intern(s, strlen(s)); }
static Str *istr(Str *s) { return s->interned ? s : intern(s->c, s->len); }
V vcstr(const char *s) { return vstr(str_new(s, strlen(s))); }

/* ---- numbers (no libm here) ---- */

static double d_floor(double x)
{
    if (x != x || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    long long i = (long long)x;
    double d = (double)i;
    return d > x ? d - 1 : d;
}
static double d_ceil(double x) { double f = d_floor(x); return f < x ? f + 1 : f; }
static double d_abs(double x) { return x < 0 ? -x : x; }
static double d_sqrt(double x) { double r; __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x)); return r; }
static double d_trunc(double x) { return x < 0 ? d_ceil(x) : d_floor(x); }
static double d_fmod(double a, double b)
{
    if (b == 0 || a != a || b != b) return 0.0 / 0.0;
    double q = d_trunc(a / b);
    double r = a - q * b;
    if (d_abs(r) >= d_abs(b)) r = 0;                       /* precision guard for huge quotients */
    return r;
}
static double d_exp(double x)
{
    if (x > 709) return 1.0 / 0.0;
    if (x < -745) return 0;
    int k = (int)(x / 0.6931471805599453);
    double r = x - k * 0.6931471805599453, term = 1, sum = 1;
    for (int i = 1; i < 30; i++) { term *= r / i; sum += term; }
    while (k > 0) { sum *= 2; k--; }
    while (k < 0) { sum /= 2; k++; }
    return sum;
}
static double d_log(double x)
{
    if (x <= 0) return x == 0 ? -1.0 / 0.0 : 0.0 / 0.0;
    int k = 0;
    while (x > 2) { x /= 2; k++; }
    while (x < 1) { x *= 2; k--; }
    double y = (x - 1) / (x + 1), y2 = y * y, sum = 0, t = y;
    for (int i = 1; i < 60; i += 2) { sum += t / i; t *= y2; }
    return 2 * sum + k * 0.6931471805599453;
}
static double d_pow(double a, double b)
{
    if (b == 0) return 1;
    if (b == d_floor(b) && d_abs(b) < 1e9) {
        double r = 1, base = a;
        long long e = (long long)(b < 0 ? -b : b);
        while (e) { if (e & 1) r *= base; base *= base; e >>= 1; }
        return b < 0 ? 1 / r : r;
    }
    if (a < 0) return 0.0 / 0.0;
    return d_exp(b * d_log(a));
}
static double d_sin(double x)
{
    double pi2 = 6.283185307179586;
    x = d_fmod(x, pi2);
    if (x > 3.141592653589793) x -= pi2;
    if (x < -3.141592653589793) x += pi2;
    double term = x, sum = x;
    for (int i = 1; i < 12; i++) { term *= -x * x / ((2 * i) * (2 * i + 1)); sum += term; }
    return sum;
}
static double d_cos(double x) { return d_sin(x + 1.5707963267948966); }
static double d_atan(double x)
{
    int inv = 0, neg = x < 0;
    if (neg) x = -x;
    if (x > 1) { x = 1 / x; inv = 1; }
    /* argument reduction: atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))) */
    double y = x / (1 + d_sqrt(1 + x * x));
    y = y / (1 + d_sqrt(1 + y * y));
    double t = y, sum = 0;
    for (int i = 0; i < 25; i++) { sum += (i & 1 ? -t : t) / (2 * i + 1); t *= y * y; }
    sum *= 4;
    if (inv) sum = 1.5707963267948966 - sum;
    return neg ? -sum : sum;
}
static double d_atan2(double y, double x)
{
    if (x > 0) return d_atan(y / x);
    if (x < 0) return y >= 0 ? d_atan(y / x) + 3.141592653589793 : d_atan(y / x) - 3.141592653589793;
    return y > 0 ? 1.5707963267948966 : y < 0 ? -1.5707963267948966 : 0;
}

static int is_nan(double d) { return d != d; }
static int is_inf(double d) { return d == 1.0 / 0.0 || d == -1.0 / 0.0; }

/* Shortest-ish formatting: integers exactly, others with up to 15 significant digits. */
static void num_fmt(double d, char *out, int cap)
{
    if (is_nan(d)) { safestrcpy(out, "NaN", cap); return; }
    if (is_inf(d)) { safestrcpy(out, d > 0 ? "Infinity" : "-Infinity", cap); return; }
    if (d == 0) { safestrcpy(out, "0", cap); return; }
    char buf[64];
    int n = 0;
    if (d < 0) { buf[n++] = '-'; d = -d; }
    if (d == d_floor(d) && d < 1e21) {
        char tmp[32];
        int k = 0;
        double x = d;
        while (x >= 1 && k < 30) {
            double q = d_floor(x / 10);
            int digit = (int)(x - q * 10 + 0.5);
            if (digit > 9) digit = 9;
            tmp[k++] = '0' + digit;
            x = q;
        }
        while (k) buf[n++] = tmp[--k];
        buf[n] = 0;
        safestrcpy(out, buf, cap);
        return;
    }
    int e10 = 0;
    double m = d;
    while (m >= 10) { m /= 10; e10++; }
    while (m < 1) { m *= 10; e10--; }
    char dig[20];
    int nd = 0;
    for (int i = 0; i < 17; i++) {
        int dgt = (int)m;
        if (dgt > 9) dgt = 9;
        dig[nd++] = '0' + dgt;
        m = (m - dgt) * 10;
    }
    /* round to 15 significant digits */
    int carry = dig[15] >= '5';
    nd = 15;
    for (int i = nd - 1; i >= 0 && carry; i--) {
        if (dig[i] == '9') dig[i] = '0';
        else { dig[i]++; carry = 0; }
    }
    if (carry) { memmove(dig + 1, dig, nd - 1); dig[0] = '1'; e10++; }
    while (nd > 1 && dig[nd - 1] == '0') nd--;
    if (e10 >= 21 || e10 <= -7) {
        buf[n++] = dig[0];
        if (nd > 1) { buf[n++] = '.'; for (int i = 1; i < nd; i++) buf[n++] = dig[i]; }
        buf[n++] = 'e';
        buf[n++] = e10 < 0 ? '-' : '+';
        int ae = e10 < 0 ? -e10 : e10;
        char t[8]; int k = 0;
        do { t[k++] = '0' + ae % 10; ae /= 10; } while (ae);
        while (k) buf[n++] = t[--k];
    } else if (e10 < 0) {
        buf[n++] = '0'; buf[n++] = '.';
        for (int i = 0; i < -e10 - 1; i++) buf[n++] = '0';
        for (int i = 0; i < nd; i++) buf[n++] = dig[i];
    } else {
        for (int i = 0; i <= e10; i++) buf[n++] = i < nd ? dig[i] : '0';
        if (nd > e10 + 1) { buf[n++] = '.'; for (int i = e10 + 1; i < nd; i++) buf[n++] = dig[i]; }
    }
    buf[n] = 0;
    safestrcpy(out, buf, cap);
}

/* Parses a number; *end gets the first unused character. */
static double num_parse(const char *s, const char **end)
{
    const char *p = s;
    int neg = 0;
    if (*p == '+' || *p == '-') neg = *p++ == '-';
    if (!strncmp(p, "Infinity", 8)) { *end = p + 8; return neg ? -1.0 / 0.0 : 1.0 / 0.0; }
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        double v = 0;
        p += 2;
        const char *st = p;
        for (;; p++) {
            int c = *p, d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'f' ? (c | 32) - 'a' + 10 : -1;
            if (d < 0) break;
            v = v * 16 + d;
        }
        *end = p == st ? s : p;
        return neg ? -v : v;
    }
    double v = 0;
    int any = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); any = 1; }
    if (*p == '.') {
        p++;
        double f = 0.1;
        while (*p >= '0' && *p <= '9') { v += (*p++ - '0') * f; f /= 10; any = 1; }
    }
    if (!any) { *end = s; return 0.0 / 0.0; }
    if (*p == 'e' || *p == 'E') {
        const char *q = p + 1;
        int eneg = 0, e = 0, got = 0;
        if (*q == '+' || *q == '-') eneg = *q++ == '-';
        while (*q >= '0' && *q <= '9') { e = e * 10 + (*q++ - '0'); got = 1; if (e > 400) e = 400; }
        if (got) {
            p = q;
            v *= d_pow(10, eneg ? -e : e);
        }
    }
    *end = p;
    return neg ? -v : v;
}

/* ============================== objects ============================== */

Obj *obj_new(int cls, Obj *proto)
{
    Obj *o = xalloc(sizeof *o);
    if (!o) {
        static Obj dummy;
        memset(&dummy, 0, sizeof dummy);
        return &dummy;
    }
    o->cls = cls;
    o->proto = proto;
    return o;
}

Obj *arr_new(void) { return obj_new(O_ARRAY, J->array_proto); }

static void arr_reserve(Obj *a, int n)
{
    if (n <= a->acap) return;
    int nc = a->acap ? a->acap * 2 : 8;
    while (nc < n) nc *= 2;
    V *na = xalloc(nc * sizeof *na);
    if (!na) return;
    if (a->alen) memcpy(na, a->a, a->alen * sizeof *na);
    a->a = na;
    a->acap = nc;
}

void arr_push(Obj *a, V v)
{
    arr_reserve(a, a->alen + 1);
    if (a->alen < a->acap)
        a->a[a->alen++] = v;
}

static void arr_set(Obj *a, int i, V v)
{
    if (i < 0 || i > 10000000) return;
    if (i >= a->alen) {
        arr_reserve(a, i + 1);
        if (i >= a->acap) return;
        for (int k = a->alen; k < i; k++) a->a[k] = undef();
        a->alen = i + 1;
    }
    a->a[i] = v;
}

struct prop *own_find(Obj *o, Str *k)
{
    for (int i = 0; i < o->np; i++)
        if (o->p[i].k == k)
            return &o->p[i];
    return NULL;
}

void own_put(Obj *o, Str *k, V v)
{
    k = istr(k);
    struct prop *p = own_find(o, k);
    if (p) { p->v = v; return; }
    if (o->np == o->cap) {
        int nc = o->cap ? o->cap * 2 : 4;
        struct prop *np = xalloc(nc * sizeof *np);
        if (!np) return;
        if (o->np) memcpy(np, o->p, o->np * sizeof *np);
        o->p = np;
        o->cap = nc;
    }
    o->p[o->np++] = (struct prop){k, v};
}

static void own_del(Obj *o, Str *k)
{
    for (int i = 0; i < o->np; i++)
        if (o->p[i].k == k) {
            memmove(&o->p[i], &o->p[i + 1], (o->np - i - 1) * sizeof o->p[0]);
            o->np--;
            return;
        }
}

/* "123" -> 123, else -1 */
static int str_index(Str *k)
{
    if (k->len == 0 || k->len > 9) return -1;
    int v = 0;
    for (int i = 0; i < k->len; i++) {
        if (k->c[i] < '0' || k->c[i] > '9') return -1;
        v = v * 10 + k->c[i] - '0';
    }
    if (k->len > 1 && k->c[0] == '0') return -1;
    return v;
}

static Str *S_length, *S_prototype, *S_constructor, *S_message, *S_name, *S_toString, *S_callee;

V get(V o, Str *k)
{
    k = istr(k);
    Obj *proto;
    switch (o.t) {
    case VSTR: {
        if (k == S_length) return vnum(o.s->len);
        int i = str_index(k);
        if (i >= 0) return i < o.s->len ? vstr(str_new(o.s->c + i, 1)) : undef();
        proto = J->string_proto;
        break;
    }
    case VNUM: proto = J->number_proto; break;
    case VBOOL: proto = J->bool_proto; break;
    case VOBJ: {
        Obj *ob = o.o;
        if (ob->cls == O_ARRAY) {
            if (k == S_length) return vnum(ob->alen);
            int i = str_index(k);
            if (i >= 0) return i < ob->alen ? ob->a[i] : undef();
        }
        V hv;
        if (ob->hosted && host_get(ob, k, &hv)) return hv;
        for (Obj *x = ob; x; x = x->proto) {
            struct prop *p = own_find(x, k);
            if (p) return p->v;
            if (x->hosted && x != ob && host_get(x, k, &hv)) return hv;
        }
        if ((ob->cls == O_FUNC || ob->cls == O_NATIVE || ob->cls == O_BOUND) && k == S_prototype) {
            Obj *pr = obj_new(O_PLAIN, J->object_proto);     /* created on first use */
            own_put(pr, S_constructor, o);
            own_put(ob, S_prototype, vobj(pr));
            return vobj(pr);
        }
        return undef();
    }
    case VU: case VNULL:
        return throw_err("TypeError", "cannot read property of null or undefined: ", k->c);
    default:
        return undef();
    }
    for (Obj *x = proto; x; x = x->proto) {
        struct prop *p = own_find(x, k);
        if (p) return p->v;
    }
    return undef();
}

void put(V o, Str *k, V v)
{
    if (o.t != VOBJ) {
        if (o.t == VU || o.t == VNULL)
            throw_err("TypeError", "cannot set property of null or undefined: ", k->c);
        return;
    }
    Obj *ob = o.o;
    if (ob->cls == O_ARRAY) {
        if (k == S_length || (k->len == 6 && !strcmp(k->c, "length"))) {
            int n = (int)to_num(v);
            if (n >= 0 && n < ob->alen) ob->alen = n;
            else if (n > ob->alen) arr_set(ob, n - 1, undef());
            return;
        }
        int i = str_index(k);
        if (i >= 0) { arr_set(ob, i, v); return; }
    }
    if (ob->hosted && host_put(ob, istr(k), v))
        return;
    own_put(ob, k, v);
}

static V get_idx(V o, V key)
{
    if (o.t == VOBJ && o.o->cls == O_ARRAY && key.t == VNUM) {
        double d = key.n;
        int i = (int)d;
        if (i == d && i >= 0) return i < o.o->alen ? o.o->a[i] : undef();
    }
    if (o.t == VSTR && key.t == VNUM) {
        int i = (int)key.n;
        if (i == key.n && i >= 0) return i < o.s->len ? vstr(str_new(o.s->c + i, 1)) : undef();
    }
    return get(o, to_str(key));
}

static void put_idx(V o, V key, V v)
{
    if (o.t == VOBJ && o.o->cls == O_ARRAY && key.t == VNUM) {
        int i = (int)key.n;
        if (i == key.n && i >= 0) { arr_set(o.o, i, v); return; }
    }
    put(o, to_str(key), v);
}

/* ============================== conversions ============================== */

int truthy(V v)
{
    switch (v.t) {
    case VBOOL: return v.b;
    case VNUM: return v.n != 0 && !is_nan(v.n);
    case VSTR: return v.s->len > 0;
    case VOBJ: return 1;
    default: return 0;
    }
}

int is_callable(V v)
{
    return v.t == VOBJ && (v.o->cls == O_FUNC || v.o->cls == O_NATIVE || v.o->cls == O_BOUND);
}

static V to_primitive(V v)
{
    if (v.t != VOBJ) return v;
    V f = get(v, S_toString);
    if (is_callable(f)) {
        V r = call(f, v, NULL, 0);
        if (r.t != VOBJ) return r;
    }
    return vcstr("[object Object]");
}

double to_num(V v)
{
    switch (v.t) {
    case VNUM: return v.n;
    case VBOOL: return v.b;
    case VNULL: return 0;
    case VSTR: {
        const char *p = v.s->c;
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
        if (!*p) return 0;
        const char *e;
        double d = num_parse(p, &e);
        while (*e == ' ' || *e == '\t' || *e == '\n' || *e == '\r') e++;
        return *e ? 0.0 / 0.0 : d;
    }
    case VOBJ: return to_num(to_primitive(v));
    default: return 0.0 / 0.0;
    }
}

static int to_int(V v)
{
    double d = to_num(v);
    if (is_nan(d) || is_inf(d)) return 0;
    if (d > 2147483647.0) return 2147483647;
    if (d < -2147483648.0) return -2147483647 - 1;
    return (int)d;
}

static int to_i32(V v)
{
    double d = to_num(v);
    if (is_nan(d) || is_inf(d)) return 0;
    d = d_trunc(d);
    double m = d_fmod(d, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (int)(unsigned)(long long)m;
}

static Str *join(Obj *a, const char *sep);

Str *to_str(V v)
{
    char buf[64];
    switch (v.t) {
    case VU: return intern_c("undefined");
    case VNULL: return intern_c("null");
    case VBOOL: return intern_c(v.b ? "true" : "false");
    case VNUM: num_fmt(v.n, buf, sizeof buf); return str_new(buf, strlen(buf));
    case VSTR: return v.s;
    default:
        if (v.o->cls == O_ARRAY) return join(v.o, ",");
        if (v.o->cls == O_FUNC || v.o->cls == O_NATIVE) return intern_c("function () { [code] }");
        if (v.o->cls == O_ERROR) {
            Str *n = to_str(get(v, S_name)), *m = to_str(get(v, S_message));
            Str *r = str_new(NULL, n->len + 2 + m->len);
            memcpy(r->c, n->c, n->len);
            memcpy(r->c + n->len, ": ", 2);
            memcpy(r->c + n->len + 2, m->c, m->len);
            return r;
        }
        {
            V p = to_primitive(v);
            return p.t == VOBJ ? intern_c("[object Object]") : to_str(p);
        }
    }
}

const char *cstr(V v) { return to_str(v)->c; }

static Str *concat(Str *a, Str *b)
{
    Str *r = str_new(NULL, a->len + b->len);
    memcpy(r->c, a->c, a->len);
    memcpy(r->c + a->len, b->c, b->len);
    r->c[a->len + b->len] = 0;
    return r;
}

static int str_eq(Str *a, Str *b) { return a == b || (a->len == b->len && !memcmp(a->c, b->c, a->len)); }

static int strict_eq(V a, V b)
{
    if (a.t != b.t) return 0;
    switch (a.t) {
    case VU: case VNULL: return 1;
    case VBOOL: return a.b == b.b;
    case VNUM: return a.n == b.n;
    case VSTR: return str_eq(a.s, b.s);
    default: return a.o == b.o;
    }
}

static int loose_eq(V a, V b)
{
    if (a.t == b.t) return strict_eq(a, b);
    if ((a.t == VU || a.t == VNULL) && (b.t == VU || b.t == VNULL)) return 1;
    if (a.t == VU || a.t == VNULL || b.t == VU || b.t == VNULL) return 0;
    if (a.t == VOBJ) a = to_primitive(a);
    if (b.t == VOBJ) b = to_primitive(b);
    if (a.t == VSTR && b.t == VSTR) return str_eq(a.s, b.s);
    return to_num(a) == to_num(b);
}

static const char *type_of(V v)
{
    switch (v.t) {
    case VU: return "undefined";
    case VNULL: return "object";
    case VBOOL: return "boolean";
    case VNUM: return "number";
    case VSTR: return "string";
    default: return is_callable(v) ? "function" : "object";
    }
}

/* ============================== errors ============================== */

V throw_err(const char *kind, const char *msg, ...)
{
    if (J->thrown) return undef();
    char buf[256];
    safestrcpy(buf, msg, sizeof buf);
    va_list ap;
    va_start(ap, msg);
    const char *extra = va_arg(ap, const char *);
    va_end(ap);
    if (extra && (unsigned long)extra > 4096) {
        int n = strlen(buf);
        safestrcpy(buf + n, extra, sizeof buf - n);
    }
    Obj *e = obj_new(O_ERROR, J->error_proto);
    own_put(e, S_name, vcstr(kind));
    own_put(e, S_message, vcstr(buf));
    J->thrown = 1;
    J->exc = vobj(e);
    return undef();
}

/* ============================== lexer ============================== */

enum { T_EOF, T_NUM, T_STR, T_TPL, T_ID, T_PUNC, T_REGEX, T_ERR };

struct tok {
    u8 t;
    u8 nl;                                     /* a line break came before it (for semicolon insertion) */
    int pos, end, line;
    double num;
    Str *s;                                    /* identifiers, strings, template raw text */
    char p[5];                                 /* punctuator */
};

struct lexer {
    const char *src;
    int len, pos, line;
    struct tok tok, ahead;
    int has_ahead;
    char err[160];
    int failed;
};

static const char *const PUNCS[] = {
    ">>>=", "...", "===", "!==", "**=", "<<=", ">>=", ">>>", "&&=", "||=", "?\?=",
    "=>", "==", "!=", "<=", ">=", "&&", "||", "??", "?.", "++", "--", "+=", "-=", "*=", "/=", "%=", "&=", "|=",
    "^=", "**", "<<", ">>",
    "{", "}", "(", ")", "[", "]", ";", ",", "<", ">", "+", "-", "*", "/", "%", "&", "|", "^", "!", "~", "?",
    ":", "=", ".", NULL};

static int id_start(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '$' || c >= 0x80; }
static int id_char(int c) { return id_start(c) || (c >= '0' && c <= '9'); }

static void lex_fail(struct lexer *L, const char *msg)
{
    if (!L->failed) {
        snprintf(L->err, sizeof L->err, "SyntaxError: %s (line %d)", msg, L->line);
        L->failed = 1;
    }
}

static int utf8_enc(char *o, u32 cp)
{
    if (cp < 0x80) { o[0] = cp; return 1; }
    if (cp < 0x800) { o[0] = 0xC0 | cp >> 6; o[1] = 0x80 | (cp & 63); return 2; }
    if (cp < 0x10000) { o[0] = 0xE0 | cp >> 12; o[1] = 0x80 | (cp >> 6 & 63); o[2] = 0x80 | (cp & 63); return 3; }
    o[0] = 0xF0 | cp >> 18; o[1] = 0x80 | (cp >> 12 & 63); o[2] = 0x80 | (cp >> 6 & 63); o[3] = 0x80 | (cp & 63);
    return 4;
}

static int hexv(int c) { c |= 32; return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1; }

/* Decodes escapes in [s, e) into a new string. */
static Str *unescape(const char *s, const char *e)
{
    char *tmp = malloc((e - s) * 4 + 1);
    if (!tmp) return intern_c("");
    int n = 0;
    while (s < e) {
        if (*s != '\\' || s + 1 >= e) { tmp[n++] = *s++; continue; }
        s++;
        char c = *s++;
        switch (c) {
        case 'n': tmp[n++] = '\n'; break;
        case 't': tmp[n++] = '\t'; break;
        case 'r': tmp[n++] = '\r'; break;
        case 'b': tmp[n++] = '\b'; break;
        case 'f': tmp[n++] = '\f'; break;
        case 'v': tmp[n++] = '\v'; break;
        case '0': tmp[n++] = 0; break;
        case '\n': break;                                 /* line continuation */
        case 'x':
            if (s + 2 <= e && hexv(s[0]) >= 0 && hexv(s[1]) >= 0) { n += utf8_enc(tmp + n, hexv(s[0]) * 16 + hexv(s[1])); s += 2; }
            break;
        case 'u': {
            u32 cp = 0;
            if (s < e && *s == '{') {
                s++;
                while (s < e && *s != '}') cp = cp * 16 + (hexv(*s++) & 15);
                if (s < e) s++;
            } else {
                for (int i = 0; i < 4 && s < e; i++) cp = cp * 16 + (hexv(*s++) & 15);
                if (cp >= 0xD800 && cp < 0xDC00 && s + 6 <= e && s[0] == '\\' && s[1] == 'u') {
                    u32 lo = 0;
                    for (int i = 2; i < 6; i++) lo = lo * 16 + (hexv(s[i]) & 15);
                    if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); s += 6; }
                }
            }
            n += utf8_enc(tmp + n, cp);
            break;
        }
        default: tmp[n++] = c;
        }
    }
    Str *r = str_new(tmp, n);
    free(tmp);
    return r;
}

static void skip_space(struct lexer *L, int *nl)
{
    const char *s = L->src;
    while (L->pos < L->len) {
        char c = s[L->pos];
        if (c == '\n') { *nl = 1; L->line++; L->pos++; }
        else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') L->pos++;
        else if ((u8)c == 0xC2 && L->pos + 1 < L->len && (u8)s[L->pos + 1] == 0xA0) L->pos += 2;
        else if (c == '/' && L->pos + 1 < L->len && s[L->pos + 1] == '/') {
            while (L->pos < L->len && s[L->pos] != '\n') L->pos++;
        } else if (c == '/' && L->pos + 1 < L->len && s[L->pos + 1] == '*') {
            L->pos += 2;
            while (L->pos + 1 < L->len && !(s[L->pos] == '*' && s[L->pos + 1] == '/')) {
                if (s[L->pos] == '\n') { *nl = 1; L->line++; }
                L->pos++;
            }
            L->pos += 2;
        } else if (c == '<' && !strncmp(s + L->pos, "<!--", 4)) {       /* old HTML comment hiding */
            while (L->pos < L->len && s[L->pos] != '\n') L->pos++;
        } else if (c == '-' && *nl && !strncmp(s + L->pos, "-->", 3)) {
            while (L->pos < L->len && s[L->pos] != '\n') L->pos++;
        } else break;
    }
}

static void lex_one(struct lexer *L, struct tok *t)
{
    int nl = 0;
    skip_space(L, &nl);
    memset(t, 0, sizeof *t);
    t->nl = nl;
    t->pos = L->pos;
    t->line = L->line;
    const char *s = L->src;
    if (L->pos >= L->len) { t->t = T_EOF; t->end = L->pos; return; }
    char c = s[L->pos];
    if ((c >= '0' && c <= '9') || (c == '.' && L->pos + 1 < L->len && s[L->pos + 1] >= '0' && s[L->pos + 1] <= '9')) {
        const char *e;
        if (c == '0' && L->pos + 1 < L->len && (s[L->pos + 1] | 32) == 'b') {
            double v = 0;
            int p = L->pos + 2;
            while (p < L->len && (s[p] == '0' || s[p] == '1' || s[p] == '_')) { if (s[p] != '_') v = v * 2 + (s[p] - '0'); p++; }
            t->num = v; e = s + p;
        } else if (c == '0' && L->pos + 1 < L->len && (s[L->pos + 1] | 32) == 'o') {
            double v = 0;
            int p = L->pos + 2;
            while (p < L->len && s[p] >= '0' && s[p] <= '7') v = v * 8 + (s[p++] - '0');
            t->num = v; e = s + p;
        } else {
            char nb[64];
            int k = 0, p = L->pos;
            while (p < L->len && k < 62 && (id_char(s[p]) || s[p] == '.' ||
                   ((s[p] == '+' || s[p] == '-') && p > L->pos && (s[p - 1] | 32) == 'e'))) {
                if (s[p] != '_') nb[k++] = s[p];
                p++;
            }
            nb[k] = 0;
            const char *ne;
            t->num = num_parse(nb, &ne);
            if (*ne == 'n') ne++;                            /* BigInt suffix: treated as a number */
            e = s + p;
        }
        t->t = T_NUM;
        L->pos = e - s;
    } else if (c == '"' || c == '\'') {
        int p = L->pos + 1;
        while (p < L->len && s[p] != c && s[p] != '\n') { if (s[p] == '\\') p++; p++; }
        if (p >= L->len || s[p] != c) { lex_fail(L, "unterminated string"); t->t = T_ERR; return; }
        t->t = T_STR;
        t->s = unescape(s + L->pos + 1, s + p);
        L->pos = p + 1;
    } else if (c == '`') {
        int p = L->pos + 1, depth = 0;
        while (p < L->len) {
            if (s[p] == '\\') { p += 2; continue; }
            if (!depth && s[p] == '`') break;
            if (s[p] == '$' && p + 1 < L->len && s[p + 1] == '{') { depth++; p += 2; continue; }
            if (depth && s[p] == '{') depth++;
            if (depth && s[p] == '}') depth--;
            if (s[p] == '\n') L->line++;
            p++;
        }
        if (p >= L->len) { lex_fail(L, "unterminated template"); t->t = T_ERR; return; }
        t->t = T_TPL;
        t->s = str_new(s + L->pos + 1, p - L->pos - 1);
        L->pos = p + 1;
    } else if (id_start((u8)c) || c == '\\') {
        int p = L->pos;
        while (p < L->len && id_char((u8)s[p])) p++;
        t->t = T_ID;
        t->s = intern(s + L->pos, p - L->pos);
        L->pos = p;
        if (p == t->pos) { lex_fail(L, "unexpected character"); t->t = T_ERR; L->pos++; }
    } else if (c == '#' && L->pos + 1 < L->len && id_start((u8)s[L->pos + 1])) {   /* private names: plain names */
        int p = L->pos + 1;
        while (p < L->len && id_char((u8)s[p])) p++;
        t->t = T_ID;
        t->s = intern(s + L->pos, p - L->pos);
        L->pos = p;
    } else {
        for (int i = 0; PUNCS[i]; i++) {
            int n = strlen(PUNCS[i]);
            if (L->pos + n <= L->len && !strncmp(s + L->pos, PUNCS[i], n)) {
                if (!strcmp(PUNCS[i], "?.") && L->pos + 2 < L->len && s[L->pos + 2] >= '0' && s[L->pos + 2] <= '9')
                    continue;                                /* a ? .5 : b */
                t->t = T_PUNC;
                memcpy(t->p, PUNCS[i], n + 1);
                L->pos += n;
                t->end = L->pos;
                return;
            }
        }
        char m[48];
        snprintf(m, sizeof m, "unexpected character '%c'", c);
        lex_fail(L, m);
        t->t = T_ERR;
        L->pos++;
    }
    t->end = L->pos;
}

static void next(struct lexer *L)
{
    if (L->has_ahead) { L->tok = L->ahead; L->has_ahead = 0; return; }
    lex_one(L, &L->tok);
}

static struct tok *peek2(struct lexer *L)
{
    if (!L->has_ahead) {
        int save_pos = L->pos, save_line = L->line;
        lex_one(L, &L->ahead);
        L->has_ahead = 1;
        (void)save_pos; (void)save_line;
    }
    return &L->ahead;
}

/* A regular-expression literal starting at the current '/' or '/=' token. */
static void lex_regex(struct lexer *L)
{
    const char *s = L->src;
    int p = L->tok.pos + 1, cls = 0;
    while (p < L->len && s[p] != '\n') {
        if (s[p] == '\\') { p += 2; continue; }
        if (s[p] == '[') cls = 1;
        else if (s[p] == ']') cls = 0;
        else if (s[p] == '/' && !cls) break;
        p++;
    }
    if (p >= L->len || s[p] != '/') { lex_fail(L, "unterminated regular expression"); return; }
    int body_end = p++;
    while (p < L->len && id_char((u8)s[p])) p++;
    L->tok.t = T_REGEX;
    L->tok.s = str_new(s + L->tok.pos + 1, body_end - L->tok.pos - 1);
    L->tok.num = 0;
    for (int i = body_end + 1; i < p; i++) if (s[i] == 'g') L->tok.num = 1;
    L->tok.end = p;
    L->pos = p;
    L->has_ahead = 0;
}

/* ============================== AST ============================== */

enum {
    N_NUM, N_STR, N_TPL, N_ID, N_THIS, N_ARR, N_OBJ, N_FN, N_CALL, N_NEW, N_DOT, N_IDX, N_UN, N_BIN, N_AND, N_OR,
    N_NULLISH, N_ASSIGN, N_UPD, N_COND, N_SEQ, N_TYPEOF, N_DELETE, N_VOID, N_SPREAD, N_REGEX, N_CLASS, N_SUPER,
    N_UNDEF,
    S_VAR, S_EXPR, S_BLOCK, S_IF, S_FOR, S_FORIN, S_FOROF, S_WHILE, S_DO, S_RET, S_BRK, S_CONT, S_THROW, S_TRY,
    S_SWITCH, S_EMPTY, S_FUNC, S_CLASS,
};

enum { OP_ADD = 1, OP_SUB, OP_MUL, OP_DIV, OP_MOD, OP_EXP, OP_EQ, OP_NE, OP_SEQ, OP_SNE, OP_LT, OP_GT, OP_LE, OP_GE,
       OP_BAND, OP_BOR, OP_XOR, OP_SHL, OP_SHR, OP_USHR, OP_IN, OP_INSTANCEOF, OP_NEG, OP_POS, OP_NOT, OP_BNOT,
       OP_AND, OP_OR, OP_NULLISH };

struct ast {
    u8 k, op, flags;                           /* N_FN flags: 1 arrow, 2 expression body, 4 has rest param,
                                                  8 method; N_DOT/N_IDX/N_CALL: 1 optional chain;
                                                  S_VAR: kind 0 var 1 let 2 const; N_UPD: 1 prefix */
    int line;
    struct ast *a, *b, *c, *d;
    struct ast **l;
    int n;
    double num;
    Str *s;
};

struct parser {
    struct lexer L;
    int in_func;
};

static struct ast *mk(struct parser *P, int k)
{
    struct ast *a = xalloc(sizeof *a);
    if (!a) {
        static struct ast dummy;
        memset(&dummy, 0, sizeof dummy);
        dummy.k = N_UNDEF;
        P->L.failed = 1;
        return &dummy;
    }
    a->k = k;
    a->line = P->L.tok.line;
    return a;
}

struct alist {
    struct ast **v;
    int n, cap;
};

static void al_add(struct alist *l, struct ast *a)
{
    if (l->n == l->cap) {
        int nc = l->cap ? l->cap * 2 : 4;
        struct ast **nv = xalloc(nc * sizeof *nv);
        if (!nv) return;
        if (l->n) memcpy(nv, l->v, l->n * sizeof *nv);
        l->v = nv;
        l->cap = nc;
    }
    l->v[l->n++] = a;
}

static int is_p(struct parser *P, const char *p) { return P->L.tok.t == T_PUNC && !strcmp(P->L.tok.p, p); }
static int is_kw(struct parser *P, const char *w) { return P->L.tok.t == T_ID && !strcmp(P->L.tok.s->c, w); }

static void syntax(struct parser *P, const char *what)
{
    if (P->L.failed) return;
    char m[120];
    if (P->L.tok.t == T_EOF)
        snprintf(m, sizeof m, "%s, found the end of the script", what);
    else if (P->L.tok.t == T_ID)
        snprintf(m, sizeof m, "%s, found '%s'", what, P->L.tok.s->c);
    else if (P->L.tok.t == T_PUNC)
        snprintf(m, sizeof m, "%s, found '%s'", what, P->L.tok.p);
    else
        snprintf(m, sizeof m, "%s", what);
    lex_fail(&P->L, m);
}

static int accept(struct parser *P, const char *p)
{
    if (is_p(P, p)) { next(&P->L); return 1; }
    return 0;
}

static void expect(struct parser *P, const char *p)
{
    if (!accept(P, p)) {
        char m[40];
        snprintf(m, sizeof m, "expected '%s'", p);
        syntax(P, m);
    }
}

static void semi(struct parser *P)
{
    if (accept(P, ";")) return;
    if (is_p(P, "}") || P->L.tok.t == T_EOF || P->L.tok.nl) return;
    syntax(P, "expected ';'");
}

static const char *const RESERVED[] = {"var", "let", "const", "function", "return", "if", "else", "for", "while", "do",
    "break", "continue", "new", "delete", "typeof", "instanceof", "in", "of", "this", "null", "true", "false",
    "undefined", "throw", "try", "catch", "finally", "switch", "case", "default", "class", "extends", "super", "void",
    "yield", "async", "await", "import", "export", NULL};

static struct ast *expr(struct parser *P);
static struct ast *assign_expr(struct parser *P);
static struct ast *statement(struct parser *P);
static struct ast *func_rest(struct parser *P, Str *name, int arrow_params, int flags);
static struct ast *class_expr(struct parser *P, int is_stmt);

static struct ast *ident(struct parser *P)
{
    if (P->L.tok.t != T_ID) { syntax(P, "expected a name"); return mk(P, N_UNDEF); }
    struct ast *a = mk(P, N_ID);
    a->s = P->L.tok.s;
    next(&P->L);
    return a;
}

/* Name in a property position: identifiers, keywords, strings, numbers. */
static Str *prop_name(struct parser *P)
{
    Str *s;
    if (P->L.tok.t == T_ID || P->L.tok.t == T_STR) s = istr(P->L.tok.s);
    else if (P->L.tok.t == T_NUM) s = istr(to_str(vnum(P->L.tok.num)));
    else { syntax(P, "expected a property name"); return intern_c("?"); }
    next(&P->L);
    return s;
}

/* Is the '(' at the current token the start of an arrow function's parameter list? */
static int arrow_ahead(struct parser *P)
{
    const char *s = P->L.src;
    int p = P->L.tok.pos, depth = 0;
    for (; p < P->L.len; p++) {
        char c = s[p];
        if (c == '"' || c == '\'' || c == '`') {
            char q = c;
            for (p++; p < P->L.len && s[p] != q; p++) if (s[p] == '\\') p++;
            continue;
        }
        if (c == '(' || c == '[' || c == '{') depth++;
        else if (c == ')' || c == ']' || c == '}') { if (--depth == 0) { p++; break; } }
    }
    while (p < P->L.len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r')) p++;
    return p + 1 < P->L.len && s[p] == '=' && s[p + 1] == '>';
}

static struct ast *template(struct parser *P, Str *raw)
{
    struct ast *t = mk(P, N_TPL);
    struct alist parts = {0};
    const char *s = raw->c, *e = raw->c + raw->len, *lit = s;
    while (s < e) {
        if (*s == '\\') { s += 2; continue; }
        if (*s == '$' && s + 1 < e && s[1] == '{') {
            struct ast *str = mk(P, N_STR);
            str->s = unescape(lit, s);
            al_add(&parts, str);
            const char *x = s + 2;
            int depth = 1;
            const char *q = x;
            while (q < e && depth) {
                if (*q == '"' || *q == '\'' || *q == '`') {
                    char qc = *q++;
                    while (q < e && *q != qc) { if (*q == '\\') q++; q++; }
                } else if (*q == '{') depth++;
                else if (*q == '}') depth--;
                if (depth) q++;
            }
            struct parser sub = {0};
            sub.L.src = x;
            sub.L.len = q - x;
            sub.L.line = P->L.tok.line;
            next(&sub.L);
            struct ast *ex = expr(&sub);
            if (sub.L.failed && !P->L.failed) { P->L.failed = 1; safestrcpy(P->L.err, sub.L.err, sizeof P->L.err); }
            al_add(&parts, ex);
            s = q < e ? q + 1 : q;
            lit = s;
            continue;
        }
        s++;
    }
    struct ast *str = mk(P, N_STR);
    str->s = unescape(lit, e);
    al_add(&parts, str);
    t->l = parts.v;
    t->n = parts.n;
    return t;
}

static struct ast *object_lit(struct parser *P)
{
    struct ast *o = mk(P, N_OBJ);
    struct alist props = {0};
    next(&P->L);                                               /* { */
    while (!is_p(P, "}") && !P->L.failed) {
        struct ast *pr = mk(P, N_UNDEF);                        /* a = key (N_STR or computed expr), b = value */
        if (accept(P, "...")) {
            pr->k = N_SPREAD;
            pr->a = assign_expr(P);
        } else {
            int is_get = 0, is_set = 0;
            if ((is_kw(P, "get") || is_kw(P, "set")) && peek2(&P->L)->t != T_PUNC) {
                is_get = is_kw(P, "get");
                is_set = !is_get;
                next(&P->L);
            } else if (is_kw(P, "async") && peek2(&P->L)->t == T_ID) {
                next(&P->L);
            }
            if (accept(P, "[")) {
                pr->a = assign_expr(P);
                expect(P, "]");
            } else {
                Str *k;
                int shorthand = P->L.tok.t == T_ID;
                k = prop_name(P);
                pr->a = mk(P, N_STR);
                pr->a->s = k;
                if (shorthand && (is_p(P, ",") || is_p(P, "}"))) {
                    struct ast *id = mk(P, N_ID);
                    id->s = k;
                    pr->b = id;
                }
            }
            if (!pr->b) {
                if (is_p(P, "(")) {
                    pr->b = func_rest(P, pr->a->k == N_STR ? pr->a->s : NULL, 0, 8);
                    if (is_get) pr->op = 1;                     /* getter: called on read */
                    if (is_set) pr->op = 2;
                } else {
                    expect(P, ":");
                    pr->b = assign_expr(P);
                }
            }
            pr->k = N_UNDEF;
        }
        al_add(&props, pr);
        if (!accept(P, ",")) break;
    }
    expect(P, "}");
    o->l = props.v;
    o->n = props.n;
    return o;
}

static struct ast *primary(struct parser *P)
{
    struct tok *t = &P->L.tok;
    struct ast *a;
    switch (t->t) {
    case T_NUM: a = mk(P, N_NUM); a->num = t->num; next(&P->L); return a;
    case T_STR: a = mk(P, N_STR); a->s = t->s; next(&P->L); return a;
    case T_TPL: { Str *raw = t->s; a = template(P, raw); next(&P->L); return a; }
    case T_ID: {
        const char *w = t->s->c;
        if (!strcmp(w, "function")) {
            next(&P->L);
            accept(P, "*");
            Str *name = P->L.tok.t == T_ID && !is_p(P, "(") ? P->L.tok.s : NULL;
            if (name) next(&P->L);
            return func_rest(P, name, 0, 0);
        }
        if (!strcmp(w, "async") && (peek2(&P->L)->t == T_ID || (peek2(&P->L)->t == T_PUNC && !strcmp(peek2(&P->L)->p, "(")))) {
            next(&P->L);                                         /* async functions run synchronously */
            return primary(P);
        }
        if (!strcmp(w, "class")) return class_expr(P, 0);
        if (!strcmp(w, "this")) { next(&P->L); return mk(P, N_THIS); }
        if (!strcmp(w, "super")) { next(&P->L); return mk(P, N_SUPER); }
        if (!strcmp(w, "null")) { next(&P->L); a = mk(P, N_NUM); a->k = N_UNDEF; a->op = 1; return a; }
        if (!strcmp(w, "true") || !strcmp(w, "false")) { a = mk(P, N_NUM); a->k = N_UNDEF; a->op = w[0] == 't' ? 3 : 2; next(&P->L); return a; }
        if (!strcmp(w, "new")) {
            next(&P->L);
            if (is_p(P, ".")) { next(&P->L); next(&P->L); return mk(P, N_UNDEF); }   /* new.target */
            struct ast *n = mk(P, N_NEW);
            struct ast *callee = primary(P);
            for (;;) {                                             /* member accesses bind tighter than new */
                if (accept(P, ".")) {
                    struct ast *d = mk(P, N_DOT);
                    d->a = callee;
                    d->s = prop_name(P);
                    callee = d;
                } else if (accept(P, "[")) {
                    struct ast *d = mk(P, N_IDX);
                    d->a = callee;
                    d->b = expr(P);
                    expect(P, "]");
                    callee = d;
                } else break;
            }
            n->a = callee;
            struct alist args = {0};
            if (accept(P, "(")) {
                while (!is_p(P, ")") && !P->L.failed) {
                    if (accept(P, "...")) { struct ast *sp = mk(P, N_SPREAD); sp->a = assign_expr(P); al_add(&args, sp); }
                    else al_add(&args, assign_expr(P));
                    if (!accept(P, ",")) break;
                }
                expect(P, ")");
            }
            n->l = args.v;
            n->n = args.n;
            return n;
        }
        if (peek2(&P->L)->t == T_PUNC && !strcmp(peek2(&P->L)->p, "=>") && !peek2(&P->L)->nl) {
            Str *pname = t->s;                                     /* x => ... */
            next(&P->L);
            next(&P->L);                                           /* => */
            struct ast *f = func_rest(P, NULL, 1, 1);
            struct ast *param = mk(P, N_ID);
            param->s = pname;
            struct ast **pl = xalloc(sizeof *pl);
            if (pl) pl[0] = param;
            f->l = pl;
            f->n = pl ? 1 : 0;
            return f;
        }
        return ident(P);
    }
    case T_PUNC:
        if (is_p(P, "(")) {
            if (arrow_ahead(P))
                return func_rest(P, NULL, 2, 1);
            next(&P->L);
            a = expr(P);
            expect(P, ")");
            return a;
        }
        if (is_p(P, "[")) {
            next(&P->L);
            a = mk(P, N_ARR);
            struct alist el = {0};
            while (!is_p(P, "]") && !P->L.failed) {
                if (is_p(P, ",")) { next(&P->L); al_add(&el, mk(P, N_UNDEF)); continue; }
                if (accept(P, "...")) { struct ast *sp = mk(P, N_SPREAD); sp->a = assign_expr(P); al_add(&el, sp); }
                else al_add(&el, assign_expr(P));
                if (!accept(P, ",")) break;
            }
            expect(P, "]");
            a->l = el.v;
            a->n = el.n;
            return a;
        }
        if (is_p(P, "{")) return object_lit(P);
        if (is_p(P, "/") || is_p(P, "/=")) {
            lex_regex(&P->L);
            a = mk(P, N_REGEX);
            a->s = P->L.tok.s;
            a->num = P->L.tok.num;
            next(&P->L);
            return a;
        }
        break;
    default: break;
    }
    syntax(P, "expected an expression");
    next(&P->L);
    return mk(P, N_UNDEF);
}

/* Parses parameters (unless arrow_params says they were taken: 1 = single name already consumed) and a body. */
static struct ast *func_rest(struct parser *P, Str *name, int arrow_params, int flags)
{
    struct ast *f = mk(P, N_FN);
    f->s = name;
    f->flags = flags & ~2;
    struct alist params = {0};
    if (arrow_params == 1) {
        /* the caller consumed `x =>`: the parameter name is the token before '=>' */
    } else {
        expect(P, "(");
        while (!is_p(P, ")") && !P->L.failed) {
            struct ast *pm;
            if (accept(P, "...")) {
                pm = ident(P);
                f->flags |= 4;
            } else if (is_p(P, "{") || is_p(P, "[")) {
                syntax(P, "destructuring parameters are not supported");
                return f;
            } else {
                pm = ident(P);
            }
            if (accept(P, "=")) pm->a = assign_expr(P);            /* default value */
            al_add(&params, pm);
            if (!accept(P, ",")) break;
        }
        expect(P, ")");
        if (arrow_params == 2)
            expect(P, "=>");
    }
    if (arrow_params != 1) {
        f->l = params.v;
        f->n = params.n;
    }
    if ((flags & 1) && !is_p(P, "{")) {
        f->flags |= 2;
        f->a = assign_expr(P);
        return f;
    }
    expect(P, "{");
    struct ast *body = mk(P, S_BLOCK);
    struct alist st = {0};
    int saved = P->in_func;
    P->in_func = 1;
    while (!is_p(P, "}") && P->L.tok.t != T_EOF && !P->L.failed)
        al_add(&st, statement(P));
    P->in_func = saved;
    expect(P, "}");
    body->l = st.v;
    body->n = st.n;
    f->a = body;
    return f;
}

static struct ast *class_expr(struct parser *P, int is_stmt)
{
    next(&P->L);                                                  /* class */
    struct ast *c = mk(P, N_CLASS);
    if (P->L.tok.t == T_ID && !is_kw(P, "extends")) {
        c->s = P->L.tok.s;
        next(&P->L);
    } else if (is_stmt) {
        syntax(P, "expected a class name");
    }
    if (is_kw(P, "extends")) {
        next(&P->L);
        c->b = assign_expr(P);
    }
    expect(P, "{");
    struct alist members = {0};
    while (!is_p(P, "}") && P->L.tok.t != T_EOF && !P->L.failed) {
        if (accept(P, ";")) continue;
        struct ast *m = mk(P, N_UNDEF);                          /* a = name (N_STR), b = function; op: 1 static, flags: 1 get 2 set 4 field */
        if (is_kw(P, "static") && !(peek2(&P->L)->t == T_PUNC && !strcmp(peek2(&P->L)->p, "("))) { next(&P->L); m->op = 1; }
        if (is_kw(P, "async") && peek2(&P->L)->t == T_ID) next(&P->L);
        if ((is_kw(P, "get") || is_kw(P, "set")) && peek2(&P->L)->t == T_ID) {
            m->flags = is_kw(P, "get") ? 1 : 2;
            next(&P->L);
        }
        accept(P, "*");
        m->a = mk(P, N_STR);
        if (accept(P, "[")) { m->a = assign_expr(P); expect(P, "]"); }
        else m->a->s = prop_name(P);
        if (is_p(P, "(")) {
            m->b = func_rest(P, m->a->k == N_STR ? m->a->s : NULL, 0, 8);
        } else {                                                   /* field */
            m->flags = 4;
            if (accept(P, "=")) m->b = assign_expr(P);
            semi(P);
        }
        al_add(&members, m);
    }
    expect(P, "}");
    c->l = members.v;
    c->n = members.n;
    if (is_stmt) {
        struct ast *s = mk(P, S_CLASS);
        s->a = c;
        s->s = c->s;
        return s;
    }
    return c;
}

static struct ast *call_args(struct parser *P, struct ast *callee, int optional)
{
    struct ast *c = mk(P, N_CALL);
    c->a = callee;
    c->flags = optional;
    struct alist args = {0};
    next(&P->L);                                                  /* ( */
    while (!is_p(P, ")") && !P->L.failed) {
        if (accept(P, "...")) { struct ast *sp = mk(P, N_SPREAD); sp->a = assign_expr(P); al_add(&args, sp); }
        else al_add(&args, assign_expr(P));
        if (!accept(P, ",")) break;
    }
    expect(P, ")");
    c->l = args.v;
    c->n = args.n;
    return c;
}

static struct ast *postfix(struct parser *P)
{
    struct ast *a = primary(P);
    for (;;) {
        if (P->L.failed) return a;
        if (is_p(P, ".")) {
            next(&P->L);
            struct ast *d = mk(P, N_DOT);
            d->a = a;
            d->s = prop_name(P);
            a = d;
        } else if (is_p(P, "?.")) {
            next(&P->L);
            if (is_p(P, "(")) { a = call_args(P, a, 1); continue; }
            struct ast *d;
            if (accept(P, "[")) { d = mk(P, N_IDX); d->a = a; d->b = expr(P); expect(P, "]"); }
            else { d = mk(P, N_DOT); d->a = a; d->s = prop_name(P); }
            d->flags = 1;
            a = d;
        } else if (is_p(P, "[")) {
            next(&P->L);
            struct ast *d = mk(P, N_IDX);
            d->a = a;
            d->b = expr(P);
            expect(P, "]");
            a = d;
        } else if (is_p(P, "(")) {
            a = call_args(P, a, 0);
        } else if (P->L.tok.t == T_TPL) {                          /* tagged template: called with the string */
            struct ast *c = mk(P, N_CALL);
            c->a = a;
            struct ast **l = xalloc(sizeof *l);
            if (l) l[0] = template(P, P->L.tok.s);
            next(&P->L);
            c->l = l;
            c->n = l ? 1 : 0;
            a = c;
        } else if ((is_p(P, "++") || is_p(P, "--")) && !P->L.tok.nl) {
            struct ast *u = mk(P, N_UPD);
            u->op = P->L.tok.p[0] == '+' ? OP_ADD : OP_SUB;
            u->a = a;
            next(&P->L);
            a = u;
        } else if (is_p(P, "!") && 0) {
        } else {
            return a;
        }
    }
}

static struct ast *unary(struct parser *P)
{
    struct tok *t = &P->L.tok;
    if (t->t == T_PUNC) {
        int op = 0;
        if (!strcmp(t->p, "-")) op = OP_NEG;
        else if (!strcmp(t->p, "+")) op = OP_POS;
        else if (!strcmp(t->p, "!")) op = OP_NOT;
        else if (!strcmp(t->p, "~")) op = OP_BNOT;
        if (op) {
            next(&P->L);
            struct ast *u = mk(P, N_UN);
            u->op = op;
            u->a = unary(P);
            return u;
        }
        if (!strcmp(t->p, "++") || !strcmp(t->p, "--")) {
            struct ast *u = mk(P, N_UPD);
            u->op = t->p[0] == '+' ? OP_ADD : OP_SUB;
            u->flags = 1;
            next(&P->L);
            u->a = unary(P);
            return u;
        }
    } else if (t->t == T_ID) {
        const char *w = t->s->c;
        int k = !strcmp(w, "typeof") ? N_TYPEOF : !strcmp(w, "delete") ? N_DELETE : !strcmp(w, "void") ? N_VOID : -1;
        if (!strcmp(w, "await")) { next(&P->L); return unary(P); }
        if (k >= 0) {
            next(&P->L);
            struct ast *u = mk(P, k);
            u->a = unary(P);
            return u;
        }
    }
    return postfix(P);
}

static int binop(struct parser *P, int *prec, int no_in)
{
    struct tok *t = &P->L.tok;
    static const struct { const char *p; int op, prec; } T[] = {
        {"??", OP_NULLISH, 1}, {"||", OP_OR, 2}, {"&&", OP_AND, 3}, {"|", OP_BOR, 4}, {"^", OP_XOR, 5},
        {"&", OP_BAND, 6}, {"==", OP_EQ, 7}, {"!=", OP_NE, 7}, {"===", OP_SEQ, 7}, {"!==", OP_SNE, 7},
        {"<", OP_LT, 8}, {">", OP_GT, 8}, {"<=", OP_LE, 8}, {">=", OP_GE, 8}, {"<<", OP_SHL, 9},
        {">>", OP_SHR, 9}, {">>>", OP_USHR, 9}, {"+", OP_ADD, 10}, {"-", OP_SUB, 10}, {"*", OP_MUL, 11},
        {"/", OP_DIV, 11}, {"%", OP_MOD, 11}, {"**", OP_EXP, 12}, {NULL, 0, 0}};
    if (t->t == T_PUNC) {
        for (int i = 0; T[i].p; i++)
            if (!strcmp(t->p, T[i].p)) { *prec = T[i].prec; return T[i].op; }
    } else if (t->t == T_ID) {
        if (!strcmp(t->s->c, "instanceof")) { *prec = 8; return OP_INSTANCEOF; }
        if (!strcmp(t->s->c, "in") && !no_in) { *prec = 8; return OP_IN; }
    }
    return 0;
}

static int g_no_in;                                               /* parsing a for(... in ...) head */

static struct ast *binary(struct parser *P, int min)
{
    struct ast *l = unary(P);
    for (;;) {
        int prec, op = binop(P, &prec, g_no_in);
        if (!op || prec < min) return l;
        next(&P->L);
        struct ast *r = binary(P, op == OP_EXP ? prec : prec + 1);
        struct ast *b = mk(P, op == OP_AND ? N_AND : op == OP_OR ? N_OR : op == OP_NULLISH ? N_NULLISH : N_BIN);
        b->op = op;
        b->a = l;
        b->b = r;
        l = b;
    }
}

static struct ast *assign_expr(struct parser *P)
{
    if (is_kw(P, "yield")) { next(&P->L); }
    struct ast *l = binary(P, 1);
    if (accept(P, "?")) {
        struct ast *c = mk(P, N_COND);
        c->a = l;
        int saved = g_no_in;
        g_no_in = 0;
        c->b = assign_expr(P);
        g_no_in = saved;
        expect(P, ":");
        c->c = assign_expr(P);
        return c;
    }
    if (P->L.tok.t == T_PUNC) {
        static const struct { const char *p; int op; } A[] = {
            {"=", 0}, {"+=", OP_ADD}, {"-=", OP_SUB}, {"*=", OP_MUL}, {"/=", OP_DIV}, {"%=", OP_MOD},
            {"**=", OP_EXP}, {"<<=", OP_SHL}, {">>=", OP_SHR}, {">>>=", OP_USHR}, {"&=", OP_BAND}, {"|=", OP_BOR},
            {"^=", OP_XOR}, {"&&=", OP_AND}, {"||=", OP_OR}, {"?\?=", OP_NULLISH}, {NULL, 0}};
        for (int i = 0; A[i].p; i++)
            if (!strcmp(P->L.tok.p, A[i].p)) {
                if (l->k != N_ID && l->k != N_DOT && l->k != N_IDX) {
                    syntax(P, "can't assign to this");
                    return l;
                }
                next(&P->L);
                struct ast *a = mk(P, N_ASSIGN);
                a->op = A[i].op;
                a->a = l;
                a->b = assign_expr(P);
                return a;
            }
    }
    return l;
}

static struct ast *expr(struct parser *P)
{
    struct ast *e = assign_expr(P);
    if (!is_p(P, ",")) return e;
    struct ast *s = mk(P, N_SEQ);
    struct alist l = {0};
    al_add(&l, e);
    while (accept(P, ","))
        al_add(&l, assign_expr(P));
    s->l = l.v;
    s->n = l.n;
    return s;
}

static struct ast *var_decl(struct parser *P, int kind)
{
    struct ast *v = mk(P, S_VAR);
    v->flags = kind;
    struct alist l = {0};
    do {
        if (is_p(P, "{") || is_p(P, "[")) { syntax(P, "destructuring is not supported"); return v; }
        struct ast *id = ident(P);
        if (accept(P, "=")) id->a = assign_expr(P);
        al_add(&l, id);
    } while (accept(P, ","));
    v->l = l.v;
    v->n = l.n;
    return v;
}

static struct ast *block(struct parser *P)
{
    struct ast *b = mk(P, S_BLOCK);
    struct alist st = {0};
    expect(P, "{");
    while (!is_p(P, "}") && P->L.tok.t != T_EOF && !P->L.failed)
        al_add(&st, statement(P));
    expect(P, "}");
    b->l = st.v;
    b->n = st.n;
    return b;
}

static struct ast *statement(struct parser *P)
{
    struct tok *t = &P->L.tok;
    struct ast *s;
    if (t->t == T_PUNC) {
        if (is_p(P, "{")) return block(P);
        if (accept(P, ";")) return mk(P, S_EMPTY);
    }
    if (t->t == T_ID) {
        const char *w = t->s->c;
        if (!strcmp(w, "var") || (!strcmp(w, "let") && peek2(&P->L)->t == T_ID) || !strcmp(w, "const") ||
            (!strcmp(w, "let") && peek2(&P->L)->t == T_PUNC && (!strcmp(peek2(&P->L)->p, "[") || !strcmp(peek2(&P->L)->p, "{")))) {
            int kind = w[0] == 'v' ? 0 : w[0] == 'l' ? 1 : 2;
            next(&P->L);
            s = var_decl(P, kind);
            semi(P);
            return s;
        }
        if (!strcmp(w, "function") || (!strcmp(w, "async") && peek2(&P->L)->t == T_ID && !strcmp(peek2(&P->L)->s->c, "function"))) {
            if (w[0] == 'a') next(&P->L);
            next(&P->L);
            accept(P, "*");
            s = mk(P, S_FUNC);
            s->s = P->L.tok.t == T_ID ? P->L.tok.s : intern_c("anonymous");
            if (P->L.tok.t == T_ID) next(&P->L);
            s->a = func_rest(P, s->s, 0, 0);
            return s;
        }
        if (!strcmp(w, "class")) return class_expr(P, 1);
        if (!strcmp(w, "if")) {
            next(&P->L);
            s = mk(P, S_IF);
            expect(P, "(");
            s->a = expr(P);
            expect(P, ")");
            s->b = statement(P);
            if (is_kw(P, "else")) { next(&P->L); s->c = statement(P); }
            return s;
        }
        if (!strcmp(w, "while")) {
            next(&P->L);
            s = mk(P, S_WHILE);
            expect(P, "(");
            s->a = expr(P);
            expect(P, ")");
            s->b = statement(P);
            return s;
        }
        if (!strcmp(w, "do")) {
            next(&P->L);
            s = mk(P, S_DO);
            s->b = statement(P);
            if (!is_kw(P, "while")) syntax(P, "expected 'while'");
            next(&P->L);
            expect(P, "(");
            s->a = expr(P);
            expect(P, ")");
            accept(P, ";");
            return s;
        }
        if (!strcmp(w, "for")) {
            next(&P->L);
            if (is_kw(P, "await")) next(&P->L);
            expect(P, "(");
            struct ast *init = NULL;
            int kind = -1;
            g_no_in = 1;
            if (is_kw(P, "var") || is_kw(P, "let") || is_kw(P, "const")) {
                kind = P->L.tok.s->c[0] == 'v' ? 0 : P->L.tok.s->c[0] == 'l' ? 1 : 2;
                next(&P->L);
                init = var_decl(P, kind);
            } else if (!is_p(P, ";")) {
                init = expr(P);
            }
            g_no_in = 0;
            if (init && (is_kw(P, "of") || is_kw(P, "in"))) {
                s = mk(P, is_kw(P, "of") ? S_FOROF : S_FORIN);
                next(&P->L);
                s->a = init;                                       /* S_VAR with one name, or an lvalue */
                s->b = is_kw(P, "of") ? assign_expr(P) : expr(P);
                expect(P, ")");
                s->c = statement(P);
                return s;
            }
            s = mk(P, S_FOR);
            s->a = init;
            expect(P, ";");
            if (!is_p(P, ";")) s->b = expr(P);
            expect(P, ";");
            if (!is_p(P, ")")) s->c = expr(P);
            expect(P, ")");
            s->d = statement(P);
            return s;
        }
        if (!strcmp(w, "return")) {
            next(&P->L);
            s = mk(P, S_RET);
            if (!is_p(P, ";") && !is_p(P, "}") && P->L.tok.t != T_EOF && !P->L.tok.nl)
                s->a = expr(P);
            semi(P);
            return s;
        }
        if (!strcmp(w, "break") || !strcmp(w, "continue")) {
            s = mk(P, w[0] == 'b' ? S_BRK : S_CONT);
            next(&P->L);
            if (P->L.tok.t == T_ID && !P->L.tok.nl) next(&P->L);      /* labels are ignored */
            semi(P);
            return s;
        }
        if (!strcmp(w, "throw")) {
            next(&P->L);
            s = mk(P, S_THROW);
            s->a = expr(P);
            semi(P);
            return s;
        }
        if (!strcmp(w, "try")) {
            next(&P->L);
            s = mk(P, S_TRY);
            s->a = block(P);
            if (is_kw(P, "catch")) {
                next(&P->L);
                if (accept(P, "(")) {
                    if (P->L.tok.t == T_ID) { s->s = P->L.tok.s; next(&P->L); }
                    expect(P, ")");
                }
                s->b = block(P);
            }
            if (is_kw(P, "finally")) {
                next(&P->L);
                s->c = block(P);
            }
            return s;
        }
        if (!strcmp(w, "switch")) {
            next(&P->L);
            s = mk(P, S_SWITCH);
            expect(P, "(");
            s->a = expr(P);
            expect(P, ")");
            expect(P, "{");
            struct alist cases = {0};
            while (!is_p(P, "}") && P->L.tok.t != T_EOF && !P->L.failed) {
                struct ast *c = mk(P, S_BLOCK);                   /* a = test (NULL: default), l = body */
                if (is_kw(P, "case")) { next(&P->L); c->a = expr(P); }
                else if (is_kw(P, "default")) next(&P->L);
                else { syntax(P, "expected 'case'"); break; }
                expect(P, ":");
                struct alist body = {0};
                while (!is_kw(P, "case") && !is_kw(P, "default") && !is_p(P, "}") && P->L.tok.t != T_EOF && !P->L.failed)
                    al_add(&body, statement(P));
                c->l = body.v;
                c->n = body.n;
                al_add(&cases, c);
            }
            expect(P, "}");
            s->l = cases.v;
            s->n = cases.n;
            return s;
        }
        if ((!strcmp(w, "import") || !strcmp(w, "export")) && !(peek2(&P->L)->t == T_PUNC && !strcmp(peek2(&P->L)->p, "("))) {
            if (!strcmp(w, "export")) {
                next(&P->L);
                if (is_kw(P, "default")) next(&P->L);
                return statement(P);
            }
            syntax(P, "modules (import) are not supported");
            return mk(P, S_EMPTY);
        }
        if (peek2(&P->L)->t == T_PUNC && !strcmp(peek2(&P->L)->p, ":") ) {   /* label: */
            int is_reserved = 0;
            for (int i = 0; RESERVED[i]; i++) if (!strcmp(w, RESERVED[i])) is_reserved = 1;
            if (!is_reserved) { next(&P->L); next(&P->L); return statement(P); }
        }
    }
    s = mk(P, S_EXPR);
    s->a = expr(P);
    semi(P);
    return s;
}

static struct ast *parse_program(const char *src, int len, char *err, int errcap)
{
    struct parser P = {0};
    P.L.src = src;
    P.L.len = len;
    P.L.line = 1;
    next(&P.L);
    struct ast *prog = mk(&P, S_BLOCK);
    struct alist st = {0};
    while (P.L.tok.t != T_EOF && !P.L.failed && !J->aborted)
        al_add(&st, statement(&P));
    prog->l = st.v;
    prog->n = st.n;
    if (P.L.failed) {
        safestrcpy(err, P.L.err, errcap);
        return NULL;
    }
    return prog;
}

/* ============================== environments ============================== */

static struct env *env_new(struct env *up)
{
    struct env *e = xalloc(sizeof *e);
    if (!e) {
        static struct env dummy;
        return &dummy;
    }
    e->up = up;
    return e;
}

static void env_def(struct env *e, Str *k, V v)
{
    k = istr(k);
    for (int i = 0; i < e->n; i++)
        if (e->v[i].k == k) { e->v[i].v = v; return; }
    if (e->n == e->cap) {
        int nc = e->cap ? e->cap * 2 : 4;
        struct prop *nv = xalloc(nc * sizeof *nv);
        if (!nv) return;
        if (e->n) memcpy(nv, e->v, e->n * sizeof *nv);
        e->v = nv;
        e->cap = nc;
    }
    e->v[e->n++] = (struct prop){k, v};
}

static struct prop *env_find(struct env *e, Str *k)
{
    for (; e; e = e->up)
        for (int i = 0; i < e->n; i++)
            if (e->v[i].k == k) return &e->v[i];
    return NULL;
}

static V lookup(struct env *e, Str *k)
{
    struct prop *p = env_find(e, k);
    if (p) return p->v;
    struct prop *g = own_find(J->global, k);
    if (g) return g->v;
    V hv;
    if (host_get(J->global, k, &hv)) return hv;
    return throw_err("ReferenceError", "not defined: ", k->c);
}

static void assign_var(struct env *e, Str *k, V v)
{
    struct prop *p = env_find(e, k);
    if (p) { p->v = v; return; }
    put(vobj(J->global), k, v);                                    /* sloppy mode: implicit global */
}

static V this_of(struct env *e)
{
    for (; e; e = e->up)
        if (e->has_this) return e->thisv;
    return vobj(J->global);
}

static Obj *func_of(struct env *e)
{
    for (; e; e = e->up)
        if (e->has_this) return e->func;
    return NULL;
}

/* ============================== evaluation ============================== */

enum { C_NORM, C_RET, C_BRK, C_CONT, C_THROW };

static V eval(struct ast *e, struct env *env);
static int run_stmt(struct ast *s, struct env *env);
static void hoist(struct ast *s, struct env *fenv, struct env *benv, int top);

#define CHK(v) do { if (J->thrown) return undef(); } while (0)

static int tick(void)
{
    if (++J->steps > J->step_limit && !J->thrown) {
        throw_err("Error", "the script ran too long and was stopped", NULL);
        J->aborted = 1;
        return 1;
    }
    if (J->aborted && !J->thrown) {
        throw_err("Error", "out of memory: scripts are stopped on this page", NULL);
        return 1;
    }
    return J->thrown;
}

static Obj *make_func(struct ast *f, struct env *env)
{
    Obj *o = obj_new(O_FUNC, J->func_proto);
    o->fn = f;
    o->env = env;
    if (f->s) own_put(o, S_name, vstr(f->s));
    return o;
}

static V call_script(Obj *fo, V thisv, V *args, int n, Obj *new_target)
{
    struct ast *f = fo->fn;
    struct env *e = env_new(fo->env);
    if (!(f->flags & 1)) {                                         /* arrows keep the outer this */
        e->has_this = 1;
        e->thisv = thisv;
        e->func = fo;
    }
    for (int i = 0; i < f->n; i++) {
        struct ast *pm = f->l[i];
        if ((f->flags & 4) && i == f->n - 1) {
            Obj *rest = arr_new();
            for (int k = i; k < n; k++) arr_push(rest, args[k]);
            env_def(e, pm->s, vobj(rest));
            break;
        }
        V v = i < n ? args[i] : undef();
        if (v.t == VU && pm->a) {
            v = eval(pm->a, e);
            CHK(v);
        }
        env_def(e, pm->s, v);
    }
    if (!(f->flags & 1)) {
        Obj *argo = arr_new();
        for (int k = 0; k < n; k++) arr_push(argo, args[k]);
        env_def(e, intern_c("arguments"), vobj(argo));
    }
    if (f->flags & 2) return eval(f->a, e);
    hoist(f->a, e, e, 1);
    J->retval = undef();
    int c = run_stmt(f->a, e);
    if (c == C_THROW) return undef();
    if (c == C_RET) { V r = J->retval; J->retval = undef(); return r; }
    (void)new_target;
    return undef();
}

V call(V f, V thisv, V *args, int n)
{
    if (!is_callable(f)) return throw_err("TypeError", "not a function", NULL);
    if (J->depth > MAX_DEPTH) return throw_err("RangeError", "too much recursion", NULL);
    if (tick()) return undef();
    J->depth++;
    V r;
    Obj *o = f.o;
    if (o->cls == O_NATIVE) {
        Obj *saved = J->global ? NULL : NULL;
        (void)saved;
        extern Obj *g_callee;
        Obj *prev = g_callee;
        g_callee = o;
        r = o->native(thisv, args, n);
        g_callee = prev;
    } else if (o->cls == O_BOUND) {
        V *all = xalloc((n + o->alen + 1) * sizeof *all);
        int k = 0;
        for (int i = 0; i < o->alen; i++) all[k++] = o->a[i];
        for (int i = 0; i < n; i++) all[k++] = args[i];
        J->depth--;
        return call(o->data, o->bthis, all, k);
    } else {
        r = call_script(o, thisv, args, n, NULL);
    }
    J->depth--;
    return r;
}

Obj *g_callee;                                                    /* the native being called (for its data) */

static V construct(V f, V *args, int n)
{
    if (!is_callable(f)) return throw_err("TypeError", "not a constructor", NULL);
    V protov = get(f, S_prototype);
    Obj *o = obj_new(O_PLAIN, protov.t == VOBJ ? protov.o : J->object_proto);
    if (f.o->cls == O_NATIVE) {
        J->retval = undef();
        extern int g_constructing;
        g_constructing = 1;
        V r = call(f, vobj(o), args, n);
        g_constructing = 0;
        return r.t == VOBJ ? r : vobj(o);
    }
    V r = call(f, vobj(o), args, n);
    CHK(r);
    return r.t == VOBJ ? r : vobj(o);
}

int g_constructing;

/* Collects call arguments, expanding spreads. */
static int eval_args(struct ast **l, int n, struct env *env, V **out)
{
    int cap = n + 4, k = 0;
    V *a = xalloc(cap * sizeof *a);
    if (!a) return -1;
    for (int i = 0; i < n; i++) {
        if (l[i]->k == N_SPREAD) {
            V s = eval(l[i]->a, env);
            if (J->thrown) return -1;
            if (s.t == VOBJ && s.o->cls == O_ARRAY) {
                if (k + s.o->alen > cap) {
                    int nc = (k + s.o->alen) * 2;
                    V *na = xalloc(nc * sizeof *na);
                    if (!na) return -1;
                    memcpy(na, a, k * sizeof *a);
                    a = na;
                    cap = nc;
                }
                for (int j = 0; j < s.o->alen; j++) a[k++] = s.o->a[j];
            } else if (s.t == VSTR) {
                for (int j = 0; j < s.s->len; j++) {
                    if (k == cap) break;
                    a[k++] = vstr(str_new(s.s->c + j, 1));
                }
            }
            continue;
        }
        if (k == cap) {
            V *na = xalloc(cap * 2 * sizeof *na);
            if (!na) return -1;
            memcpy(na, a, k * sizeof *a);
            a = na;
            cap *= 2;
        }
        a[k++] = eval(l[i], env);
        if (J->thrown) return -1;
    }
    *out = a;
    return k;
}

static V arith(int op, V a, V b)
{
    if (op == OP_ADD) {
        if (a.t == VNUM && b.t == VNUM) return vnum(a.n + b.n);
        V pa = to_primitive(a), pb = to_primitive(b);
        if (pa.t == VSTR || pb.t == VSTR) return vstr(concat(to_str(pa), to_str(pb)));
        return vnum(to_num(pa) + to_num(pb));
    }
    switch (op) {
    case OP_SUB: return vnum(to_num(a) - to_num(b));
    case OP_MUL: return vnum(to_num(a) * to_num(b));
    case OP_DIV: return vnum(to_num(a) / to_num(b));
    case OP_MOD: return vnum(d_fmod(to_num(a), to_num(b)));
    case OP_EXP: return vnum(d_pow(to_num(a), to_num(b)));
    case OP_EQ: return vbool(loose_eq(a, b));
    case OP_NE: return vbool(!loose_eq(a, b));
    case OP_SEQ: return vbool(strict_eq(a, b));
    case OP_SNE: return vbool(!strict_eq(a, b));
    case OP_LT: case OP_GT: case OP_LE: case OP_GE: {
        V pa = to_primitive(a), pb = to_primitive(b);
        int c;
        if (pa.t == VSTR && pb.t == VSTR) {
            c = strcmp(pa.s->c, pb.s->c);
        } else {
            double x = to_num(pa), y = to_num(pb);
            if (is_nan(x) || is_nan(y)) return vbool(0);
            c = x < y ? -1 : x > y ? 1 : 0;
        }
        return vbool(op == OP_LT ? c < 0 : op == OP_GT ? c > 0 : op == OP_LE ? c <= 0 : c >= 0);
    }
    case OP_BAND: return vnum(to_i32(a) & to_i32(b));
    case OP_BOR: return vnum(to_i32(a) | to_i32(b));
    case OP_XOR: return vnum(to_i32(a) ^ to_i32(b));
    case OP_SHL: return vnum((int)((unsigned)to_i32(a) << (to_i32(b) & 31)));
    case OP_SHR: return vnum(to_i32(a) >> (to_i32(b) & 31));
    case OP_USHR: return vnum((double)((unsigned)to_i32(a) >> (to_i32(b) & 31)));
    case OP_IN: {
        if (b.t != VOBJ) return throw_err("TypeError", "'in' needs an object", NULL);
        Str *k = istr(to_str(a));
        if (b.o->cls == O_ARRAY) { int i = str_index(k); if (i >= 0) return vbool(i < b.o->alen); if (k == S_length) return vbool(1); }
        for (Obj *x = b.o; x; x = x->proto) if (own_find(x, k)) return vbool(1);
        V hv;
        if (b.o->hosted && host_get(b.o, k, &hv)) return vbool(hv.t != VU);
        return vbool(0);
    }
    case OP_INSTANCEOF: {
        if (!is_callable(b)) return throw_err("TypeError", "right side of instanceof is not callable", NULL);
        if (a.t != VOBJ) return vbool(0);
        V p = get(b, S_prototype);
        if (p.t != VOBJ) return vbool(0);
        for (Obj *x = a.o->proto; x; x = x->proto) if (x == p.o) return vbool(1);
        return vbool(0);
    }
    }
    return undef();
}

static V regexp_new(Str *src, int global);

static void define_class_members(Obj *ctor, Obj *proto, struct ast *c, struct env *env);

static V eval_class(struct ast *c, struct env *env)
{
    V parent = undef();
    if (c->b) {
        parent = eval(c->b, env);
        CHK(parent);
        if (parent.t != VNULL && !is_callable(parent)) return throw_err("TypeError", "class extends a non-constructor", NULL);
    }
    struct ast *ctor_fn = NULL;
    for (int i = 0; i < c->n; i++)
        if (c->l[i]->a->k == N_STR && !strcmp(c->l[i]->a->s->c, "constructor") && !c->l[i]->op && c->l[i]->b)
            ctor_fn = c->l[i]->b;
    struct env *cenv = env_new(env);
    Obj *ctor;
    if (ctor_fn) {
        ctor = make_func(ctor_fn, cenv);
    } else {
        /* default constructor: calls the parent's with the same arguments */
        static struct ast empty_body = {.k = S_BLOCK};
        static struct ast dflt = {.k = N_FN, .flags = 0};
        dflt.a = &empty_body;
        ctor = make_func(&dflt, cenv);
        ctor->cls = O_FUNC;
        own_put(ctor, intern_c("__default_ctor"), vbool(1));
    }
    if (c->s) {
        own_put(ctor, S_name, vstr(c->s));
        env_def(cenv, c->s, vobj(ctor));
    }
    Obj *proto = obj_new(O_PLAIN, J->object_proto);
    if (parent.t == VOBJ) {
        V pp = get(parent, S_prototype);
        proto->proto = pp.t == VOBJ ? pp.o : NULL;
        ctor->proto = parent.o;                                    /* static inheritance */
        own_put(ctor, intern_c("__parent"), parent);
    } else if (parent.t == VNULL) {
        proto->proto = NULL;
    }
    own_put(proto, S_constructor, vobj(ctor));
    own_put(ctor, S_prototype, vobj(proto));
    ctor->home = ctor;
    define_class_members(ctor, proto, c, cenv);
    return vobj(ctor);
}

static void define_class_members(Obj *ctor, Obj *proto, struct ast *c, struct env *env)
{
    struct alist fields = {0};
    for (int i = 0; i < c->n; i++) {
        struct ast *m = c->l[i];
        Str *name;
        if (m->a->k == N_STR) name = m->a->s;
        else { V k = eval(m->a, env); if (J->thrown) return; name = istr(to_str(k)); }
        if (m->flags == 4) {                                       /* field: set on each new instance */
            if (m->op) { V v = m->b ? eval(m->b, env) : undef(); own_put(ctor, name, v); }
            else al_add(&fields, m);
            continue;
        }
        if (!m->op && !strcmp(name->c, "constructor")) continue;
        Obj *fn = make_func(m->b, env);
        fn->home = ctor;
        Obj *target = m->op ? ctor : proto;
        if (m->flags == 1 || m->flags == 2) {
            Obj *acc = obj_new(O_PLAIN, NULL);                     /* accessor: stored under a marker */
            own_put(acc, intern_c(m->flags == 1 ? "get" : "set"), vobj(fn));
            struct prop *old = own_find(target, istr(concat(intern_c("__acc_"), name)));
            if (old && old->v.t == VOBJ) { own_put(old->v.o, intern_c(m->flags == 1 ? "get" : "set"), vobj(fn)); continue; }
            own_put(target, istr(concat(intern_c("__acc_"), name)), vobj(acc));
            continue;
        }
        own_put(target, name, vobj(fn));
    }
    if (fields.n) {
        Obj *arr = arr_new();
        for (int i = 0; i < fields.n; i++) {
            Obj *rec = obj_new(O_PLAIN, NULL);
            own_put(rec, S_name, vstr(fields.v[i]->a->s ? fields.v[i]->a->s : intern_c("?")));
            if (fields.v[i]->b) {
                static struct ast wrapper_proto;
                (void)wrapper_proto;
                struct ast *w = xalloc(sizeof *w);
                if (w) { w->k = N_FN; w->flags = 1 | 2; w->a = fields.v[i]->b; }
                Obj *fn = make_func(w, env);
                own_put(rec, intern_c("init"), vobj(fn));
            }
            arr_push(arr, vobj(rec));
        }
        own_put(ctor, intern_c("__fields"), vobj(arr));
    }
}

/* Runs a class's field initialisers on a new instance (for every class in the chain, base first). */
static void init_fields(Obj *ctor, V thisv)
{
    struct prop *pp = own_find(ctor, intern_c("__parent"));
    (void)pp;
    struct prop *f = own_find(ctor, intern_c("__fields"));
    if (!f || f->v.t != VOBJ) return;
    Obj *arr = f->v.o;
    for (int i = 0; i < arr->alen; i++) {
        V rec = arr->a[i];
        Str *name = istr(to_str(get(rec, S_name)));
        V init = get(rec, intern_c("init"));
        V v = undef();
        if (is_callable(init)) {
            struct env *e = env_new(init.o->env);
            e->has_this = 1;
            e->thisv = thisv;
            Obj *fo = init.o;
            struct env *saved = fo->env;
            fo->env = e;
            v = call(init, thisv, NULL, 0);
            fo->env = saved;
            if (J->thrown) return;
        }
        put(thisv, name, v);
    }
}

/* Looks for an accessor `__acc_name` along the prototype chain. */
static int accessor_get(V o, Str *k, V *out)
{
    if (o.t != VOBJ) return 0;
    Str *ak = NULL;
    for (Obj *x = o.o; x; x = x->proto) {
        if (!x->np) continue;
        if (!ak) ak = istr(concat(intern_c("__acc_"), k));
        struct prop *p = own_find(x, ak);
        if (p && p->v.t == VOBJ) {
            V g = get(p->v, intern_c("get"));
            *out = is_callable(g) ? call(g, o, NULL, 0) : undef();
            return 1;
        }
    }
    return 0;
}

static int accessor_set(V o, Str *k, V v)
{
    if (o.t != VOBJ) return 0;
    Str *ak = NULL;
    for (Obj *x = o.o; x; x = x->proto) {
        if (!x->np) continue;
        if (!ak) ak = istr(concat(intern_c("__acc_"), k));
        struct prop *p = own_find(x, ak);
        if (p && p->v.t == VOBJ) {
            V s = get(p->v, intern_c("set"));
            if (is_callable(s)) call(s, o, &v, 1);
            return 1;
        }
    }
    return 0;
}

static int has_accessors;                                         /* any class defined a getter or setter */

static V member_get(V o, Str *k)
{
    if (has_accessors && o.t == VOBJ) {
        V r;
        if (!own_find(o.o, k) && accessor_get(o, k, &r)) return r;
    }
    return get(o, k);
}

static void member_put(V o, Str *k, V v)
{
    if (has_accessors && o.t == VOBJ && accessor_set(o, k, v)) return;
    put(o, k, v);
}

/* Assigns to an lvalue AST. */
static void store(struct ast *t, struct env *env, V v)
{
    if (t->k == N_ID) { assign_var(env, t->s, v); return; }
    if (t->k == N_DOT) {
        V o = eval(t->a, env);
        if (J->thrown) return;
        member_put(o, t->s, v);
        return;
    }
    if (t->k == N_IDX) {
        V o = eval(t->a, env);
        if (J->thrown) return;
        V k = eval(t->b, env);
        if (J->thrown) return;
        if (k.t == VNUM) put_idx(o, k, v);
        else member_put(o, istr(to_str(k)), v);
        return;
    }
    if (t->k == S_VAR && t->n) {
        Str *name = t->l[0]->s;
        if (t->flags) {
            /* let/const binding in for-in/of: defined in the loop's env by the caller */
        }
        assign_var(env, name, v);
        return;
    }
    throw_err("SyntaxError", "invalid assignment target", NULL);
}

static V super_parent(struct env *env)
{
    Obj *f = func_of(env);
    if (!f || !f->home) return undef();
    struct prop *p = own_find(f->home, intern_c("__parent"));
    return p ? p->v : undef();
}

static V eval(struct ast *e, struct env *env)
{
    if (tick()) return undef();
    switch (e->k) {
    case N_NUM: return vnum(e->num);
    case N_STR: return vstr(e->s);
    case N_UNDEF:
        return e->op == 1 ? vnull() : e->op == 2 ? vbool(0) : e->op == 3 ? vbool(1) : undef();
    case N_REGEX: return regexp_new(e->s, e->num != 0);
    case N_TPL: {
        Str *acc = intern_c("");
        for (int i = 0; i < e->n; i++) {
            V v = eval(e->l[i], env);
            CHK(v);
            acc = concat(acc, to_str(v));
        }
        return vstr(acc);
    }
    case N_ID: return lookup(env, e->s);
    case N_THIS: return this_of(env);
    case N_SUPER: return super_parent(env);
    case N_ARR: {
        Obj *a = arr_new();
        for (int i = 0; i < e->n; i++) {
            if (e->l[i]->k == N_SPREAD) {
                V s = eval(e->l[i]->a, env);
                CHK(s);
                if (s.t == VOBJ && s.o->cls == O_ARRAY)
                    for (int j = 0; j < s.o->alen; j++) arr_push(a, s.o->a[j]);
                else if (s.t == VSTR)
                    for (int j = 0; j < s.s->len; j++) arr_push(a, vstr(str_new(s.s->c + j, 1)));
                continue;
            }
            V v = eval(e->l[i], env);
            CHK(v);
            arr_push(a, v);
        }
        return vobj(a);
    }
    case N_OBJ: {
        Obj *o = obj_new(O_PLAIN, J->object_proto);
        for (int i = 0; i < e->n; i++) {
            struct ast *pr = e->l[i];
            if (pr->k == N_SPREAD) {
                V s = eval(pr->a, env);
                CHK(s);
                if (s.t == VOBJ) {
                    for (int j = 0; j < s.o->np; j++) own_put(o, s.o->p[j].k, s.o->p[j].v);
                    if (s.o->cls == O_ARRAY)
                        for (int j = 0; j < s.o->alen; j++) own_put(o, istr(to_str(vnum(j))), s.o->a[j]);
                }
                continue;
            }
            Str *k;
            if (pr->a->k == N_STR) k = pr->a->s;
            else { V kv = eval(pr->a, env); CHK(kv); k = istr(to_str(kv)); }
            V v = eval(pr->b, env);
            CHK(v);
            if (pr->op) {                                          /* getter / setter in a literal */
                has_accessors = 1;
                Str *ak = istr(concat(intern_c("__acc_"), k));
                struct prop *old = own_find(o, ak);
                Obj *acc = old && old->v.t == VOBJ ? old->v.o : obj_new(O_PLAIN, NULL);
                own_put(acc, intern_c(pr->op == 1 ? "get" : "set"), v);
                own_put(o, ak, vobj(acc));
                continue;
            }
            if (v.t == VOBJ && v.o->cls == O_FUNC && pr->b->k == N_FN && !pr->b->s)
                own_put(v.o, S_name, vstr(k));
            own_put(o, k, v);
        }
        return vobj(o);
    }
    case N_FN: return vobj(make_func(e, env));
    case N_CLASS: {
        for (int i = 0; i < e->n; i++) if (e->l[i]->flags == 1 || e->l[i]->flags == 2) has_accessors = 1;
        return eval_class(e, env);
    }
    case N_DOT: {
        V o = eval(e->a, env);
        CHK(o);
        if ((e->flags & 1) && (o.t == VU || o.t == VNULL)) return undef();
        if (e->a->k == N_SUPER) {                                  /* super.method: from the parent's prototype */
            V pp = o.t == VOBJ ? get(o, S_prototype) : undef();
            return pp.t == VOBJ ? get(pp, e->s) : undef();
        }
        return member_get(o, e->s);
    }
    case N_IDX: {
        V o = eval(e->a, env);
        CHK(o);
        if ((e->flags & 1) && (o.t == VU || o.t == VNULL)) return undef();
        V k = eval(e->b, env);
        CHK(k);
        if (k.t == VNUM) return get_idx(o, k);
        return member_get(o, istr(to_str(k)));
    }
    case N_CALL: {
        V thisv = undef(), f;
        struct ast *c = e->a;
        if (c->k == N_SUPER) {                                      /* super(...) in a constructor */
            V parent = super_parent(env);
            V *args;
            int n = eval_args(e->l, e->n, env, &args);
            if (n < 0) return undef();
            V self = this_of(env);
            if (is_callable(parent)) {
                if (parent.o->cls == O_NATIVE) {
                    V r = call(parent, self, args, n);
                    (void)r;
                } else {
                    call(parent, self, args, n);
                    CHK(self);
                }
                init_fields(parent.o, self);
            }
            Obj *f2 = func_of(env);
            if (f2 && f2->home) init_fields(f2->home, self);
            return undef();
        }
        if (c->k == N_DOT || c->k == N_IDX) {
            V o = eval(c->a, env);
            CHK(o);
            if ((c->flags & 1) && (o.t == VU || o.t == VNULL)) return undef();
            if (c->a->k == N_SUPER) {
                V pp = o.t == VOBJ ? get(o, S_prototype) : undef();
                thisv = this_of(env);
                if (c->k == N_DOT) f = pp.t == VOBJ ? get(pp, c->s) : undef();
                else { V k = eval(c->b, env); CHK(k); f = pp.t == VOBJ ? get(pp, istr(to_str(k))) : undef(); }
            } else {
                thisv = o;
                if (c->k == N_DOT) f = member_get(o, c->s);
                else { V k = eval(c->b, env); CHK(k); f = k.t == VNUM ? get_idx(o, k) : member_get(o, istr(to_str(k))); }
            }
            CHK(f);
            if (!is_callable(f)) {
                if (e->flags & 1 && (f.t == VU || f.t == VNULL)) return undef();
                return throw_err("TypeError", "not a function: ", c->k == N_DOT ? c->s->c : "(computed)");
            }
        } else {
            f = eval(c, env);
            CHK(f);
            if ((e->flags & 1) && (f.t == VU || f.t == VNULL)) return undef();
            if (!is_callable(f))
                return throw_err("TypeError", "not a function: ", c->k == N_ID ? c->s->c : "(expression)");
        }
        V *args;
        int n = eval_args(e->l, e->n, env, &args);
        if (n < 0) return undef();
        return call(f, thisv, args, n);
    }
    case N_NEW: {
        V f = eval(e->a, env);
        CHK(f);
        V *args;
        int n = eval_args(e->l, e->n, env, &args);
        if (n < 0) return undef();
        if (!is_callable(f)) return throw_err("TypeError", "not a constructor: ", e->a->k == N_ID ? e->a->s->c : "(expression)");
        if (f.o->cls == O_FUNC && f.o->home == f.o) {              /* a class */
            V protov = get(f, S_prototype);
            Obj *o = obj_new(O_PLAIN, protov.t == VOBJ ? protov.o : J->object_proto);
            V self = vobj(o);
            struct prop *parent = own_find(f.o, intern_c("__parent"));
            if (own_find(f.o, intern_c("__default_ctor"))) {
                if (parent && is_callable(parent->v)) {
                    if (parent->v.o->cls == O_FUNC && parent->v.o->home == parent->v.o) {
                        /* run the parent class as a constructor on this object */
                        V r = call(parent->v, self, args, n);
                        (void)r;
                        init_fields(parent->v.o, self);
                    } else {
                        call(parent->v, self, args, n);
                    }
                }
                init_fields(f.o, self);
                return self;
            }
            if (!parent) init_fields(f.o, self);
            V r = call(f, self, args, n);
            CHK(r);
            return r.t == VOBJ ? r : self;
        }
        return construct(f, args, n);
    }
    case N_UN: {
        V v = eval(e->a, env);
        CHK(v);
        switch (e->op) {
        case OP_NEG: return vnum(-to_num(v));
        case OP_POS: return vnum(to_num(v));
        case OP_NOT: return vbool(!truthy(v));
        default: return vnum(~to_i32(v));
        }
    }
    case N_TYPEOF: {
        if (e->a->k == N_ID && !env_find(env, e->a->s) && !own_find(J->global, e->a->s)) {
            V hv;
            if (!host_get(J->global, e->a->s, &hv)) return vcstr("undefined");
            return vcstr(type_of(hv));
        }
        V v = eval(e->a, env);
        CHK(v);
        return vcstr(type_of(v));
    }
    case N_VOID: { eval(e->a, env); return undef(); }
    case N_DELETE: {
        if (e->a->k == N_DOT || e->a->k == N_IDX) {
            V o = eval(e->a->a, env);
            CHK(o);
            Str *k;
            if (e->a->k == N_DOT) k = e->a->s;
            else { V kv = eval(e->a->b, env); CHK(kv); k = istr(to_str(kv)); }
            if (o.t == VOBJ) {
                if (o.o->cls == O_ARRAY) {
                    int i = str_index(k);
                    if (i >= 0 && i < o.o->alen) { o.o->a[i] = undef(); return vbool(1); }
                }
                own_del(o.o, k);
            }
        }
        return vbool(1);
    }
    case N_BIN: {
        V a = eval(e->a, env);
        CHK(a);
        V b = eval(e->b, env);
        CHK(b);
        return arith(e->op, a, b);
    }
    case N_AND: { V a = eval(e->a, env); CHK(a); return truthy(a) ? eval(e->b, env) : a; }
    case N_OR: { V a = eval(e->a, env); CHK(a); return truthy(a) ? a : eval(e->b, env); }
    case N_NULLISH: { V a = eval(e->a, env); CHK(a); return a.t == VU || a.t == VNULL ? eval(e->b, env) : a; }
    case N_COND: { V c = eval(e->a, env); CHK(c); return eval(truthy(c) ? e->b : e->c, env); }
    case N_SEQ: {
        V v = undef();
        for (int i = 0; i < e->n; i++) { v = eval(e->l[i], env); CHK(v); }
        return v;
    }
    case N_ASSIGN: {
        V v;
        if (e->op) {
            V cur = eval(e->a, env);
            CHK(cur);
            if (e->op == OP_AND) { if (!truthy(cur)) return cur; v = eval(e->b, env); }
            else if (e->op == OP_OR) { if (truthy(cur)) return cur; v = eval(e->b, env); }
            else if (e->op == OP_NULLISH) { if (cur.t != VU && cur.t != VNULL) return cur; v = eval(e->b, env); }
            else { V r = eval(e->b, env); CHK(r); v = arith(e->op, cur, r); }
        } else {
            v = eval(e->b, env);
            if (v.t == VOBJ && v.o->cls == O_FUNC && e->a->k == N_ID && e->b->k == N_FN && !e->b->s)
                own_put(v.o, S_name, vstr(e->a->s));
        }
        CHK(v);
        store(e->a, env, v);
        CHK(v);
        return v;
    }
    case N_UPD: {
        V cur = eval(e->a, env);
        CHK(cur);
        double old = to_num(cur);
        V nv = vnum(e->op == OP_ADD ? old + 1 : old - 1);
        store(e->a, env, nv);
        return (e->flags & 1) ? nv : vnum(old);
    }
    case N_SPREAD:
        return throw_err("SyntaxError", "unexpected '...'", NULL);
    }
    return undef();
}

/* ---- statements ---- */

/* Declares `var`s and function declarations of a body up front (functions in their block). */
static void hoist(struct ast *s, struct env *fenv, struct env *benv, int top)
{
    if (!s) return;
    switch (s->k) {
    case S_VAR:
        if (s->flags == 0)
            for (int i = 0; i < s->n; i++)
                if (!env_find(fenv, s->l[i]->s) || 1) {
                    int found = 0;
                    for (int k = 0; k < fenv->n; k++) if (fenv->v[k].k == s->l[i]->s) found = 1;
                    if (!found) env_def(fenv, s->l[i]->s, undef());
                }
        break;
    case S_FUNC:
        if (top) env_def(benv, s->s, vobj(make_func(s->a, benv)));
        break;
    case S_BLOCK:
        for (int i = 0; i < s->n; i++) hoist(s->l[i], fenv, benv, top);
        break;
    case S_IF: hoist(s->b, fenv, benv, 0); hoist(s->c, fenv, benv, 0); break;
    case S_FOR: hoist(s->a, fenv, benv, 0); hoist(s->d, fenv, benv, 0); break;
    case S_FORIN: case S_FOROF: hoist(s->a, fenv, benv, 0); hoist(s->c, fenv, benv, 0); break;
    case S_WHILE: case S_DO: hoist(s->b, fenv, benv, 0); break;
    case S_TRY: hoist(s->a, fenv, benv, 0); hoist(s->b, fenv, benv, 0); hoist(s->c, fenv, benv, 0); break;
    case S_SWITCH:
        for (int i = 0; i < s->n; i++)
            for (int k = 0; k < s->l[i]->n; k++) hoist(s->l[i]->l[k], fenv, benv, 0);
        break;
    }
}

static int needs_scope(struct ast *b)
{
    for (int i = 0; i < b->n; i++) {
        int k = b->l[i]->k;
        if ((k == S_VAR && b->l[i]->flags) || k == S_FUNC || k == S_CLASS) return 1;
    }
    return 0;
}

static int exec_list(struct ast **l, int n, struct env *env)
{
    for (int i = 0; i < n; i++) {
        int c = run_stmt(l[i], env);
        if (c != C_NORM) return c;
    }
    return C_NORM;
}

static void bind_loop_var(struct ast *decl, struct env *env, V v)
{
    if (decl->k == S_VAR) {
        if (decl->flags) env_def(env, decl->l[0]->s, v);
        else assign_var(env, decl->l[0]->s, v);
    } else {
        store(decl, env, v);
    }
}

static int run_stmt(struct ast *s, struct env *env)
{
    if (tick()) return C_THROW;
    switch (s->k) {
    case S_EMPTY: return C_NORM;
    case S_EXPR: eval(s->a, env); return J->thrown ? C_THROW : C_NORM;
    case S_VAR:
        for (int i = 0; i < s->n; i++) {
            struct ast *d = s->l[i];
            V v = undef();
            if (d->a) {
                v = eval(d->a, env);
                if (J->thrown) return C_THROW;
                if (v.t == VOBJ && v.o->cls == O_FUNC && (d->a->k == N_FN || d->a->k == N_CLASS) && !own_find(v.o, S_name))
                    own_put(v.o, S_name, vstr(d->s));
            } else if (!s->flags) {
                continue;                                          /* `var x;` keeps the hoisted value */
            }
            if (s->flags) env_def(env, d->s, v);
            else assign_var(env, d->s, v);
        }
        return C_NORM;
    case S_FUNC: {
        struct prop *p = env_find(env, s->s);
        if (!p) env_def(env, s->s, vobj(make_func(s->a, env)));
        return C_NORM;
    }
    case S_CLASS: {
        V c = eval(s->a, env);
        if (J->thrown) return C_THROW;
        env_def(env, s->s, c);
        return C_NORM;
    }
    case S_BLOCK: {
        struct env *e = env;
        if (needs_scope(s)) {
            e = env_new(env);
            for (int i = 0; i < s->n; i++)
                if (s->l[i]->k == S_FUNC) env_def(e, s->l[i]->s, vobj(make_func(s->l[i]->a, e)));
        }
        return exec_list(s->l, s->n, e);
    }
    case S_IF: {
        V c = eval(s->a, env);
        if (J->thrown) return C_THROW;
        if (truthy(c)) return run_stmt(s->b, env);
        return s->c ? run_stmt(s->c, env) : C_NORM;
    }
    case S_WHILE:
        for (;;) {
            V c = eval(s->a, env);
            if (J->thrown) return C_THROW;
            if (!truthy(c)) return C_NORM;
            int r = run_stmt(s->b, env);
            if (r == C_BRK) return C_NORM;
            if (r == C_RET || r == C_THROW) return r;
        }
    case S_DO:
        for (;;) {
            int r = run_stmt(s->b, env);
            if (r == C_BRK) return C_NORM;
            if (r == C_RET || r == C_THROW) return r;
            V c = eval(s->a, env);
            if (J->thrown) return C_THROW;
            if (!truthy(c)) return C_NORM;
        }
    case S_FOR: {
        struct env *e = env;
        int per_iter = s->a && s->a->k == S_VAR && s->a->flags;
        if (per_iter) e = env_new(env);
        if (s->a) {
            if (s->a->k == S_VAR) { if (run_stmt(s->a, e) == C_THROW) return C_THROW; }
            else { eval(s->a, e); if (J->thrown) return C_THROW; }
        }
        for (;;) {
            if (s->b) {
                V c = eval(s->b, e);
                if (J->thrown) return C_THROW;
                if (!truthy(c)) return C_NORM;
            }
            struct env *body_env = e;
            if (per_iter) {                                       /* closures capture this iteration's copy */
                body_env = env_new(env);
                for (int i = 0; i < e->n; i++) env_def(body_env, e->v[i].k, e->v[i].v);
            }
            int r = run_stmt(s->d, body_env);
            if (per_iter)
                for (int i = 0; i < e->n; i++) { struct prop *p = env_find(body_env, e->v[i].k); if (p) e->v[i].v = p->v; }
            if (r == C_BRK) return C_NORM;
            if (r == C_RET || r == C_THROW) return r;
            if (s->c) { eval(s->c, e); if (J->thrown) return C_THROW; }
        }
    }
    case S_FORIN: case S_FOROF: {
        V o = eval(s->b, env);
        if (J->thrown) return C_THROW;
        Obj *items = arr_new();
        if (s->k == S_FORIN) {
            if (o.t == VOBJ) {
                if (o.o->cls == O_ARRAY)
                    for (int i = 0; i < o.o->alen; i++) arr_push(items, vstr(to_str(vnum(i))));
                for (Obj *x = o.o; x && x != J->object_proto && x != J->array_proto; x = x->proto)
                    for (int i = 0; i < x->np; i++)
                        if (strncmp(x->p[i].k->c, "__", 2)) arr_push(items, vstr(x->p[i].k));
            } else if (o.t == VSTR) {
                for (int i = 0; i < o.s->len; i++) arr_push(items, vstr(to_str(vnum(i))));
            }
        } else {
            if (o.t == VOBJ && o.o->cls == O_ARRAY) {
                for (int i = 0; i < o.o->alen; i++) arr_push(items, o.o->a[i]);
            } else if (o.t == VSTR) {
                const char *p = o.s->c, *e = p + o.s->len;
                while (p < e) {
                    int l = (u8)*p < 0x80 ? 1 : (u8)*p < 0xE0 ? 2 : (u8)*p < 0xF0 ? 3 : 4;
                    if (p + l > e) l = e - p;
                    arr_push(items, vstr(str_new(p, l)));
                    p += l;
                }
            } else if (o.t == VOBJ) {
                V ent = get(o, intern_c("entries"));                  /* Map-like objects of ours */
                if (is_callable(ent)) {
                    V list = call(ent, o, NULL, 0);
                    if (list.t == VOBJ && list.o->cls == O_ARRAY)
                        for (int i = 0; i < list.o->alen; i++) arr_push(items, list.o->a[i]);
                } else {
                    throw_err("TypeError", "object is not iterable", NULL);
                    return C_THROW;
                }
            } else {
                throw_err("TypeError", "value is not iterable", NULL);
                return C_THROW;
            }
        }
        for (int i = 0; i < items->alen; i++) {
            struct env *e = env_new(env);
            bind_loop_var(s->a, e, items->a[i]);
            if (J->thrown) return C_THROW;
            int r = run_stmt(s->c, e);
            if (r == C_BRK) return C_NORM;
            if (r == C_RET || r == C_THROW) return r;
        }
        return C_NORM;
    }
    case S_RET:
        J->retval = s->a ? eval(s->a, env) : undef();
        return J->thrown ? C_THROW : C_RET;
    case S_BRK: return C_BRK;
    case S_CONT: return C_CONT;
    case S_THROW: {
        V v = eval(s->a, env);
        if (J->thrown) return C_THROW;
        J->thrown = 1;
        J->exc = v;
        return C_THROW;
    }
    case S_TRY: {
        int r = run_stmt(s->a, env);
        if (r == C_THROW && s->b && !J->aborted) {
            V ex = J->exc;
            J->thrown = 0;
            J->exc = undef();
            struct env *e = env_new(env);
            if (s->s) env_def(e, s->s, ex);
            r = run_stmt(s->b, e);
        }
        if (s->c) {
            int saved_thrown = J->thrown;
            V saved_exc = J->exc, saved_ret = J->retval;
            J->thrown = 0;
            int f = run_stmt(s->c, env);
            if (f != C_NORM) return f;
            J->thrown = saved_thrown;
            J->exc = saved_exc;
            J->retval = saved_ret;
        }
        return r;
    }
    case S_SWITCH: {
        V d = eval(s->a, env);
        if (J->thrown) return C_THROW;
        struct env *e = env_new(env);
        int start = -1;
        for (int i = 0; i < s->n && start < 0; i++) {
            if (!s->l[i]->a) continue;
            V c = eval(s->l[i]->a, e);
            if (J->thrown) return C_THROW;
            if (strict_eq(d, c)) start = i;
        }
        if (start < 0)
            for (int i = 0; i < s->n; i++) if (!s->l[i]->a) start = i;
        if (start < 0) return C_NORM;
        for (int i = start; i < s->n; i++) {
            int r = exec_list(s->l[i]->l, s->l[i]->n, e);
            if (r == C_BRK) return C_NORM;
            if (r != C_NORM) return r;
        }
        return C_NORM;
    }
    }
    return C_NORM;
}

/* ============================== standard library ============================== */

Obj *native(const char *name, native_fn fn, int nargs_unused)
{
    Obj *o = obj_new(O_NATIVE, J->func_proto);
    o->native = fn;
    if (name) own_put(o, S_name, vcstr(name));
    return o;
}

void defn(Obj *o, const char *name, native_fn fn) { own_put(o, intern_c(name), vobj(native(name, fn, 0))); }

#define A(i) arg(args, n, i)
#define NATIVE(name) static V name(V self, V *args, int n)

/* ---- Object ---- */

NATIVE(obj_keys)
{
    Obj *r = arr_new();
    V o = A(0);
    if (o.t != VOBJ) return vobj(r);
    if (o.o->cls == O_ARRAY)
        for (int i = 0; i < o.o->alen; i++) arr_push(r, vstr(to_str(vnum(i))));
    for (int i = 0; i < o.o->np; i++)
        if (strncmp(o.o->p[i].k->c, "__", 2)) arr_push(r, vstr(o.o->p[i].k));
    return vobj(r);
}

NATIVE(obj_values)
{
    Obj *r = arr_new();
    V o = A(0);
    if (o.t != VOBJ) return vobj(r);
    if (o.o->cls == O_ARRAY)
        for (int i = 0; i < o.o->alen; i++) arr_push(r, o.o->a[i]);
    for (int i = 0; i < o.o->np; i++)
        if (strncmp(o.o->p[i].k->c, "__", 2)) arr_push(r, o.o->p[i].v);
    return vobj(r);
}

NATIVE(obj_entries)
{
    Obj *r = arr_new();
    V o = A(0);
    if (o.t != VOBJ) return vobj(r);
    for (int i = 0; i < o.o->np; i++) {
        if (!strncmp(o.o->p[i].k->c, "__", 2)) continue;
        Obj *pair = arr_new();
        arr_push(pair, vstr(o.o->p[i].k));
        arr_push(pair, o.o->p[i].v);
        arr_push(r, vobj(pair));
    }
    return vobj(r);
}

NATIVE(obj_assign)
{
    V t = A(0);
    if (t.t != VOBJ) return t;
    for (int i = 1; i < n; i++)
        if (args[i].t == VOBJ)
            for (int k = 0; k < args[i].o->np; k++) put(t, args[i].o->p[k].k, args[i].o->p[k].v);
    return t;
}

NATIVE(obj_create)
{
    V p = A(0);
    return vobj(obj_new(O_PLAIN, p.t == VOBJ ? p.o : NULL));
}

NATIVE(obj_getproto)
{
    V o = A(0);
    if (o.t == VOBJ) return vobj(o.o->proto);
    if (o.t == VSTR) return vobj(J->string_proto);
    if (o.t == VNUM) return vobj(J->number_proto);
    return vnull();
}

NATIVE(obj_setproto)
{
    V o = A(0), p = A(1);
    if (o.t == VOBJ) o.o->proto = p.t == VOBJ ? p.o : NULL;
    return o;
}

NATIVE(obj_freeze) { return A(0); }

NATIVE(obj_defprop)
{
    V o = A(0), k = A(1), d = A(2);
    if (o.t != VOBJ || d.t != VOBJ) return o;
    Str *key = istr(to_str(k));
    V v = get(d, intern_c("value"));
    V g = get(d, intern_c("get")), s = get(d, intern_c("set"));
    if (is_callable(g) || is_callable(s)) {
        has_accessors = 1;
        Obj *acc = obj_new(O_PLAIN, NULL);
        if (is_callable(g)) own_put(acc, intern_c("get"), g);
        if (is_callable(s)) own_put(acc, intern_c("set"), s);
        own_put(o.o, istr(concat(intern_c("__acc_"), key)), vobj(acc));
        return o;
    }
    put(o, key, v);
    return o;
}

NATIVE(obj_hasown)
{
    if (self.t != VOBJ) return vbool(0);
    Str *k = istr(to_str(A(0)));
    if (self.o->cls == O_ARRAY) { int i = str_index(k); if (i >= 0) return vbool(i < self.o->alen); }
    return vbool(own_find(self.o, k) != NULL);
}

NATIVE(obj_tostring)
{
    if (self.t == VOBJ && self.o->cls == O_ARRAY) return vstr(join(self.o, ","));
    if (self.t == VOBJ && self.o->cls == O_ERROR) return vstr(to_str(self));
    return vcstr("[object Object]");
}

NATIVE(obj_ctor)
{
    V v = A(0);
    if (v.t == VOBJ) return v;
    return vobj(obj_new(O_PLAIN, J->object_proto));
}

NATIVE(obj_is) { return vbool(strict_eq(A(0), A(1)) || (A(0).t == VNUM && A(1).t == VNUM && is_nan(A(0).n) && is_nan(A(1).n))); }

/* ---- Function ---- */

NATIVE(fn_call) { return call(self, A(0), n > 1 ? args + 1 : NULL, n > 1 ? n - 1 : 0); }

NATIVE(fn_apply)
{
    V list = A(1);
    if (list.t == VOBJ && list.o->cls == O_ARRAY) return call(self, A(0), list.o->a, list.o->alen);
    return call(self, A(0), NULL, 0);
}

NATIVE(fn_bind)
{
    if (!is_callable(self)) return throw_err("TypeError", "bind on a non-function", NULL);
    Obj *b = obj_new(O_BOUND, J->func_proto);
    b->data = self;
    b->bthis = A(0);
    for (int i = 1; i < n; i++) {
        arr_reserve(b, b->alen + 1);
        if (b->alen < b->acap) b->a[b->alen++] = args[i];
    }
    return vobj(b);
}

/* ---- Array ---- */

static Str *join(Obj *a, const char *sep)
{
    if (J->depth > MAX_DEPTH) return intern_c("");
    J->depth++;
    int sl = strlen(sep), total = 0;
    Str **parts = xalloc((a->alen + 1) * sizeof *parts);
    if (!parts) { J->depth--; return intern_c(""); }
    for (int i = 0; i < a->alen; i++) {
        V v = a->a[i];
        parts[i] = v.t == VU || v.t == VNULL ? intern_c("") : to_str(v);
        total += parts[i]->len + (i ? sl : 0);
    }
    Str *r = str_new(NULL, total);
    int o = 0;
    for (int i = 0; i < a->alen; i++) {
        if (i) { memcpy(r->c + o, sep, sl); o += sl; }
        memcpy(r->c + o, parts[i]->c, parts[i]->len);
        o += parts[i]->len;
    }
    r->c[o] = 0;
    J->depth--;
    return r;
}

static Obj *this_arr(V self)
{
    if (self.t == VOBJ && self.o->cls == O_ARRAY) return self.o;
    throw_err("TypeError", "not an array", NULL);
    return NULL;
}

#define ARR Obj *a = this_arr(self); if (!a) return undef()

NATIVE(arr_push_n) { ARR; for (int i = 0; i < n; i++) arr_push(a, args[i]); return vnum(a->alen); }
NATIVE(arr_pop) { ARR; return a->alen ? a->a[--a->alen] : undef(); }
NATIVE(arr_shift)
{
    ARR;
    if (!a->alen) return undef();
    V v = a->a[0];
    memmove(a->a, a->a + 1, (a->alen - 1) * sizeof *a->a);
    a->alen--;
    return v;
}
NATIVE(arr_unshift)
{
    ARR;
    arr_reserve(a, a->alen + n);
    if (a->alen + n > a->acap) return vnum(a->alen);
    memmove(a->a + n, a->a, a->alen * sizeof *a->a);
    for (int i = 0; i < n; i++) a->a[i] = args[i];
    a->alen += n;
    return vnum(a->alen);
}
NATIVE(arr_join) { ARR; return vstr(join(a, A(0).t == VU ? "," : cstr(A(0)))); }

static int rel_index(V v, int len, int dflt)
{
    if (v.t == VU) return dflt;
    int i = to_int(v);
    if (i < 0) i += len;
    if (i < 0) i = 0;
    if (i > len) i = len;
    return i;
}

NATIVE(arr_slice)
{
    ARR;
    int s = rel_index(A(0), a->alen, 0), e = rel_index(A(1), a->alen, a->alen);
    Obj *r = arr_new();
    for (int i = s; i < e; i++) arr_push(r, a->a[i]);
    return vobj(r);
}

NATIVE(arr_splice)
{
    ARR;
    int s = rel_index(A(0), a->alen, 0);
    int dc = n < 2 ? a->alen - s : to_int(A(1));
    if (dc < 0) dc = 0;
    if (dc > a->alen - s) dc = a->alen - s;
    Obj *removed = arr_new();
    for (int i = 0; i < dc; i++) arr_push(removed, a->a[s + i]);
    int ins = n > 2 ? n - 2 : 0;
    int nl = a->alen - dc + ins;
    arr_reserve(a, nl);
    if (nl > a->acap) return vobj(removed);
    memmove(a->a + s + ins, a->a + s + dc, (a->alen - s - dc) * sizeof *a->a);
    for (int i = 0; i < ins; i++) a->a[s + i] = args[2 + i];
    a->alen = nl;
    return vobj(removed);
}

NATIVE(arr_indexof)
{
    if (self.t == VSTR) return undef();
    ARR;
    for (int i = rel_index(A(1), a->alen, 0); i < a->alen; i++)
        if (strict_eq(a->a[i], A(0))) return vnum(i);
    return vnum(-1);
}

NATIVE(arr_lastindexof)
{
    ARR;
    for (int i = a->alen - 1; i >= 0; i--)
        if (strict_eq(a->a[i], A(0))) return vnum(i);
    return vnum(-1);
}

NATIVE(arr_includes)
{
    ARR;
    for (int i = 0; i < a->alen; i++)
        if (strict_eq(a->a[i], A(0)) || (A(0).t == VNUM && a->a[i].t == VNUM && is_nan(A(0).n) && is_nan(a->a[i].n)))
            return vbool(1);
    return vbool(0);
}

NATIVE(arr_concat)
{
    ARR;
    Obj *r = arr_new();
    for (int i = 0; i < a->alen; i++) arr_push(r, a->a[i]);
    for (int k = 0; k < n; k++) {
        if (args[k].t == VOBJ && args[k].o->cls == O_ARRAY)
            for (int i = 0; i < args[k].o->alen; i++) arr_push(r, args[k].o->a[i]);
        else
            arr_push(r, args[k]);
    }
    return vobj(r);
}

NATIVE(arr_reverse)
{
    ARR;
    for (int i = 0, j = a->alen - 1; i < j; i++, j--) { V t = a->a[i]; a->a[i] = a->a[j]; a->a[j] = t; }
    return self;
}

/* Callback helpers: f(element, index, array). */
static V cb3(V f, V thisv, V el, int i, V arr)
{
    V args3[3] = {el, vnum(i), arr};
    return call(f, thisv, args3, 3);
}

#define ITER_START ARR; V f = A(0); if (!is_callable(f)) return throw_err("TypeError", "callback is not a function", NULL)

NATIVE(arr_foreach) { ITER_START; for (int i = 0; i < a->alen; i++) { cb3(f, A(1), a->a[i], i, self); if (J->thrown) break; } return undef(); }
NATIVE(arr_map)
{
    ITER_START;
    Obj *r = arr_new();
    for (int i = 0; i < a->alen; i++) { V v = cb3(f, A(1), a->a[i], i, self); if (J->thrown) break; arr_push(r, v); }
    return vobj(r);
}
NATIVE(arr_filter)
{
    ITER_START;
    Obj *r = arr_new();
    for (int i = 0; i < a->alen; i++) { V el = a->a[i]; V v = cb3(f, A(1), el, i, self); if (J->thrown) break; if (truthy(v)) arr_push(r, el); }
    return vobj(r);
}
NATIVE(arr_find)
{
    ITER_START;
    for (int i = 0; i < a->alen; i++) { V v = cb3(f, A(1), a->a[i], i, self); if (J->thrown) break; if (truthy(v)) return a->a[i]; }
    return undef();
}
NATIVE(arr_findindex)
{
    ITER_START;
    for (int i = 0; i < a->alen; i++) { V v = cb3(f, A(1), a->a[i], i, self); if (J->thrown) break; if (truthy(v)) return vnum(i); }
    return vnum(-1);
}
NATIVE(arr_some)
{
    ITER_START;
    for (int i = 0; i < a->alen; i++) { V v = cb3(f, A(1), a->a[i], i, self); if (J->thrown) break; if (truthy(v)) return vbool(1); }
    return vbool(0);
}
NATIVE(arr_every)
{
    ITER_START;
    for (int i = 0; i < a->alen; i++) { V v = cb3(f, A(1), a->a[i], i, self); if (J->thrown) break; if (!truthy(v)) return vbool(0); }
    return vbool(1);
}
NATIVE(arr_reduce)
{
    ITER_START;
    int i = 0;
    V acc;
    if (n > 1) acc = A(1);
    else if (a->alen) acc = a->a[i++];
    else return throw_err("TypeError", "reduce of an empty array with no initial value", NULL);
    for (; i < a->alen; i++) {
        V a4[4] = {acc, a->a[i], vnum(i), self};
        acc = call(f, undef(), a4, 4);
        if (J->thrown) break;
    }
    return acc;
}

static V sort_fn;
static int sort_cmp(V x, V y)
{
    if (x.t == VU) return y.t == VU ? 0 : 1;
    if (y.t == VU) return -1;
    if (is_callable(sort_fn)) {
        V a2[2] = {x, y};
        double d = to_num(call(sort_fn, undef(), a2, 2));
        return d < 0 ? -1 : d > 0 ? 1 : 0;
    }
    return strcmp(to_str(x)->c, to_str(y)->c);
}

NATIVE(arr_sort)
{
    ARR;
    V saved = sort_fn;
    sort_fn = A(0);
    /* merge sort: stable, O(n log n) */
    int len = a->alen;
    V *tmp = xalloc((len + 1) * sizeof *tmp);
    if (!tmp) return self;
    for (int w = 1; w < len && !J->thrown; w *= 2) {
        for (int lo = 0; lo < len; lo += 2 * w) {
            int mid = lo + w < len ? lo + w : len, hi = lo + 2 * w < len ? lo + 2 * w : len;
            int i = lo, j = mid, k = lo;
            while (i < mid && j < hi) tmp[k++] = sort_cmp(a->a[j], a->a[i]) < 0 ? a->a[j++] : a->a[i++];
            while (i < mid) tmp[k++] = a->a[i++];
            while (j < hi) tmp[k++] = a->a[j++];
        }
        memcpy(a->a, tmp, len * sizeof *tmp);
    }
    sort_fn = saved;
    return self;
}

NATIVE(arr_fill)
{
    ARR;
    int s = rel_index(A(1), a->alen, 0), e = rel_index(A(2), a->alen, a->alen);
    for (int i = s; i < e; i++) a->a[i] = A(0);
    return self;
}

NATIVE(arr_flat)
{
    ARR;
    Obj *r = arr_new();
    for (int i = 0; i < a->alen; i++) {
        V v = a->a[i];
        if (v.t == VOBJ && v.o->cls == O_ARRAY) for (int k = 0; k < v.o->alen; k++) arr_push(r, v.o->a[k]);
        else arr_push(r, v);
    }
    return vobj(r);
}

NATIVE(arr_entries)
{
    ARR;
    Obj *r = arr_new();
    for (int i = 0; i < a->alen; i++) {
        Obj *p = arr_new();
        arr_push(p, vnum(i));
        arr_push(p, a->a[i]);
        arr_push(r, vobj(p));
    }
    return vobj(r);
}

NATIVE(arr_keys)
{
    ARR;
    Obj *r = arr_new();
    for (int i = 0; i < a->alen; i++) arr_push(r, vnum(i));
    return vobj(r);
}

NATIVE(arr_isarray) { return vbool(A(0).t == VOBJ && A(0).o->cls == O_ARRAY); }

NATIVE(arr_from)
{
    V src = A(0), f = A(1);
    Obj *r = arr_new();
    if (src.t == VOBJ && src.o->cls == O_ARRAY) {
        for (int i = 0; i < src.o->alen; i++) arr_push(r, src.o->a[i]);
    } else if (src.t == VSTR) {
        for (int i = 0; i < src.s->len; i++) arr_push(r, vstr(str_new(src.s->c + i, 1)));
    } else if (src.t == VOBJ) {
        int len = to_int(get(src, S_length));
        for (int i = 0; i < len && i < 1000000; i++) arr_push(r, get_idx(src, vnum(i)));
    }
    if (is_callable(f))
        for (int i = 0; i < r->alen; i++) { r->a[i] = cb3(f, undef(), r->a[i], i, vobj(r)); if (J->thrown) break; }
    return vobj(r);
}

NATIVE(arr_of) { Obj *r = arr_new(); for (int i = 0; i < n; i++) arr_push(r, args[i]); return vobj(r); }

NATIVE(arr_ctor)
{
    Obj *r = arr_new();
    if (n == 1 && args[0].t == VNUM) { int len = to_int(args[0]); for (int i = 0; i < len && i < 10000000; i++) arr_push(r, undef()); }
    else for (int i = 0; i < n; i++) arr_push(r, args[i]);
    return vobj(r);
}

/* ---- String ---- */

static Str *this_str(V self) { return to_str(self); }

NATIVE(str_charat)
{
    Str *s = this_str(self);
    int i = to_int(A(0));
    return vstr(i >= 0 && i < s->len ? str_new(s->c + i, 1) : intern_c(""));
}
NATIVE(str_charcodeat)
{
    Str *s = this_str(self);
    int i = to_int(A(0));
    return i >= 0 && i < s->len ? vnum((u8)s->c[i]) : vnum(0.0 / 0.0);
}
NATIVE(str_fromcharcode)
{
    char buf[512];
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof buf - 4; i++) o += utf8_enc(buf + o, (u32)to_int(args[i]) & 0x1FFFFF);
    return vstr(str_new(buf, o));
}

static int find_sub(Str *h, Str *nd, int from)
{
    if (from < 0) from = 0;
    if (nd->len == 0) return from <= h->len ? from : h->len;
    for (int i = from; i + nd->len <= h->len; i++)
        if (!memcmp(h->c + i, nd->c, nd->len)) return i;
    return -1;
}

static Str *pattern_of(V v, int *global)
{
    *global = 0;
    if (v.t == VOBJ && v.o->cls == O_REGEXP) {
        *global = truthy(get(v, intern_c("global")));
        return to_str(get(v, intern_c("source")));
    }
    return to_str(v);
}

NATIVE(str_indexof) { return vnum(find_sub(this_str(self), to_str(A(0)), to_int(A(1)))); }
NATIVE(str_lastindexof)
{
    Str *h = this_str(self), *nd = to_str(A(0));
    for (int i = h->len - nd->len; i >= 0; i--) if (!memcmp(h->c + i, nd->c, nd->len)) return vnum(i);
    return vnum(-1);
}
NATIVE(str_includes) { int g; return vbool(find_sub(this_str(self), pattern_of(A(0), &g), to_int(A(1))) >= 0); }
NATIVE(str_startswith)
{
    Str *h = this_str(self), *nd = to_str(A(0));
    int p = to_int(A(1));
    return vbool(p >= 0 && p + nd->len <= h->len && !memcmp(h->c + p, nd->c, nd->len));
}
NATIVE(str_endswith)
{
    Str *h = this_str(self), *nd = to_str(A(0));
    return vbool(nd->len <= h->len && !memcmp(h->c + h->len - nd->len, nd->c, nd->len));
}
NATIVE(str_slice)
{
    Str *s = this_str(self);
    int a = rel_index(A(0), s->len, 0), b = rel_index(A(1), s->len, s->len);
    return vstr(b > a ? str_new(s->c + a, b - a) : intern_c(""));
}
NATIVE(str_substring)
{
    Str *s = this_str(self);
    int a = A(0).t == VU ? 0 : to_int(A(0)), b = A(1).t == VU ? s->len : to_int(A(1));
    if (a < 0) a = 0;
    if (b < 0) b = 0;
    if (a > s->len) a = s->len;
    if (b > s->len) b = s->len;
    if (a > b) { int t = a; a = b; b = t; }
    return vstr(str_new(s->c + a, b - a));
}
NATIVE(str_substr)
{
    Str *s = this_str(self);
    int a = rel_index(A(0), s->len, 0), l = A(1).t == VU ? s->len - a : to_int(A(1));
    if (l < 0) l = 0;
    if (a + l > s->len) l = s->len - a;
    return vstr(str_new(s->c + a, l));
}
static V case_conv(V self, int up)
{
    Str *s = this_str(self), *r = str_new(s->c, s->len);
    for (int i = 0; i < r->len; i++) {
        char c = r->c[i];
        if (up && c >= 'a' && c <= 'z') r->c[i] = c - 32;
        if (!up && c >= 'A' && c <= 'Z') r->c[i] = c + 32;
    }
    return vstr(r);
}
NATIVE(str_upper) { return case_conv(self, 1); }
NATIVE(str_lower) { return case_conv(self, 0); }
static int ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
NATIVE(str_trim)
{
    Str *s = this_str(self);
    int a = 0, b = s->len;
    while (a < b && ws(s->c[a])) a++;
    while (b > a && ws(s->c[b - 1])) b--;
    return vstr(str_new(s->c + a, b - a));
}
NATIVE(str_trimstart) { Str *s = this_str(self); int a = 0; while (a < s->len && ws(s->c[a])) a++; return vstr(str_new(s->c + a, s->len - a)); }
NATIVE(str_trimend) { Str *s = this_str(self); int b = s->len; while (b > 0 && ws(s->c[b - 1])) b--; return vstr(str_new(s->c, b)); }

NATIVE(str_split)
{
    Str *s = this_str(self);
    Obj *r = arr_new();
    if (A(0).t == VU) { arr_push(r, vstr(s)); return vobj(r); }
    int g;
    Str *sep = pattern_of(A(0), &g);
    int limit = A(1).t == VU ? 0x7fffffff : to_int(A(1));
    if (sep->len == 0) {
        for (int i = 0; i < s->len && r->alen < limit; i++) arr_push(r, vstr(str_new(s->c + i, 1)));
        return vobj(r);
    }
    int p = 0;
    for (;;) {
        if (r->alen >= limit) break;
        int f = find_sub(s, sep, p);
        if (f < 0) { arr_push(r, vstr(str_new(s->c + p, s->len - p))); break; }
        arr_push(r, vstr(str_new(s->c + p, f - p)));
        p = f + sep->len;
    }
    return vobj(r);
}

static Str *replace_impl(Str *s, V pat, V rep, int all)
{
    int g;
    Str *nd = pattern_of(pat, &g);
    if (g) all = 1;
    Str *acc = intern_c("");
    int p = 0;
    for (;;) {
        int f = find_sub(s, nd, p);
        if (f < 0 || (nd->len == 0 && f >= s->len && p > 0)) break;
        acc = concat(acc, str_new(s->c + p, f - p));
        Str *with;
        if (is_callable(rep)) {
            V a1 = vstr(str_new(s->c + f, nd->len));
            with = to_str(call(rep, undef(), &a1, 1));
        } else {
            with = to_str(rep);
        }
        acc = concat(acc, with);
        p = f + nd->len;
        if (nd->len == 0) {
            if (p < s->len) acc = concat(acc, str_new(s->c + p, 1));
            p++;
        }
        if (!all || J->thrown) break;
    }
    if (p < s->len) acc = concat(acc, str_new(s->c + p, s->len - p));
    return acc;
}

NATIVE(str_replace) { return vstr(replace_impl(this_str(self), A(0), A(1), 0)); }
NATIVE(str_replaceall) { return vstr(replace_impl(this_str(self), A(0), A(1), 1)); }
NATIVE(str_repeat)
{
    Str *s = this_str(self);
    int c = to_int(A(0));
    if (c < 0 || (long)c * s->len > 10000000) return throw_err("RangeError", "invalid repeat count", NULL);
    Str *r = str_new(NULL, s->len * c);
    for (int i = 0; i < c; i++) memcpy(r->c + i * s->len, s->c, s->len);
    return vstr(r);
}
static V pad(V self, V *args, int n, int start)
{
    Str *s = this_str(self);
    int target = to_int(A(0));
    Str *f = A(1).t == VU ? intern_c(" ") : to_str(A(1));
    if (target <= s->len || !f->len || target > 1000000) return vstr(s);
    Str *fill = str_new(NULL, target - s->len);
    for (int i = 0; i < fill->len; i++) fill->c[i] = f->c[i % f->len];
    return vstr(start ? concat(fill, s) : concat(s, fill));
}
NATIVE(str_padstart) { return pad(self, args, n, 1); }
NATIVE(str_padend) { return pad(self, args, n, 0); }
NATIVE(str_concat) { Str *s = this_str(self); for (int i = 0; i < n; i++) s = concat(s, to_str(args[i])); return vstr(s); }
NATIVE(str_tostring) { return vstr(this_str(self)); }
NATIVE(str_ctor) { return n ? vstr(to_str(args[0])) : vstr(intern_c("")); }
NATIVE(str_localecompare) { return vnum(strcmp(this_str(self)->c, to_str(A(0))->c)); }
NATIVE(str_match)
{
    int g;
    Str *s = this_str(self), *nd = pattern_of(A(0), &g);
    int f = find_sub(s, nd, 0);
    if (f < 0) return vnull();
    Obj *r = arr_new();
    arr_push(r, vstr(nd));
    own_put(r, intern_c("index"), vnum(f));
    return vobj(r);
}

/* ---- RegExp (substring patterns only) ---- */

static int has_meta(Str *s)
{
    for (int i = 0; i < s->len; i++)
        if (strchr("\\^$.|?*+()[]{}", s->c[i])) return 1;
    return 0;
}

static V regexp_new(Str *src, int global)
{
    Obj *r = obj_new(O_REGEXP, J->regexp_proto);
    Str *plain = src;
    if (has_meta(src)) {                                        /* keep escaped literal characters, drop the rest */
        char *t = malloc(src->len + 1);
        int o = 0;
        int meta = 0;
        for (int i = 0; t && i < src->len; i++) {
            if (src->c[i] == '\\' && i + 1 < src->len && strchr("\\^$.|?*+()[]{}/-", src->c[i + 1])) { t[o++] = src->c[++i]; continue; }
            if (strchr("\\^$.|?*+()[]{}", src->c[i])) { meta = 1; continue; }
            t[o++] = src->c[i];
        }
        plain = t ? str_new(t, o) : src;
        free(t);
        own_put(r, intern_c("__approximate"), vbool(meta));
    }
    own_put(r, intern_c("source"), vstr(plain));
    own_put(r, intern_c("global"), vbool(global));
    return vobj(r);
}

NATIVE(re_ctor) { return regexp_new(to_str(A(0)), A(1).t == VSTR && strchr(A(1).s->c, 'g') != NULL); }
NATIVE(re_test) { int g; return vbool(find_sub(to_str(A(0)), pattern_of(self, &g), 0) >= 0); }

/* ---- Number / Math ---- */

NATIVE(num_ctor) { return vnum(n ? to_num(args[0]) : 0); }
NATIVE(num_tofixed)
{
    double d = to_num(self);
    int digits = to_int(A(0));
    if (digits < 0) digits = 0;
    if (digits > 20) digits = 20;
    if (is_nan(d) || is_inf(d) || d_abs(d) >= 1e21) return vstr(to_str(vnum(d)));
    int neg = d < 0;
    if (neg) d = -d;
    double scale = d_pow(10, digits);
    double v = d_floor(d * scale + 0.5);
    char ip[40], out[80];
    double whole = d_floor(v / scale);
    num_fmt(whole, ip, sizeof ip);
    int o = 0;
    if (neg && v != 0) out[o++] = '-';
    for (char *p = ip; *p; p++) out[o++] = *p;
    if (digits) {
        out[o++] = '.';
        double frac = v - whole * scale;
        char fd[24];
        for (int i = digits - 1; i >= 0; i--) {
            double q = d_floor(frac / 10);
            fd[i] = '0' + (int)(frac - q * 10 + 0.5);
            if (fd[i] > '9') fd[i] = '9';
            frac = q;
        }
        for (int i = 0; i < digits; i++) out[o++] = fd[i];
    }
    out[o] = 0;
    return vcstr(out);
}
NATIVE(num_tostring)
{
    double d = to_num(self);
    int radix = A(0).t == VU ? 10 : to_int(A(0));
    if (radix == 10 || radix < 2 || radix > 36 || d != d_floor(d) || d_abs(d) > 9e15) return vstr(to_str(vnum(d)));
    char buf[80];
    int o = 79, neg = d < 0;
    buf[o] = 0;
    long long v = (long long)(neg ? -d : d);
    do { int r = v % radix; buf[--o] = r < 10 ? '0' + r : 'a' + r - 10; v /= radix; } while (v);
    if (neg) buf[--o] = '-';
    return vcstr(buf + o);
}
NATIVE(num_isinteger) { return vbool(A(0).t == VNUM && !is_inf(A(0).n) && A(0).n == d_floor(A(0).n)); }
NATIVE(g_isnan) { return vbool(is_nan(to_num(A(0)))); }
NATIVE(g_isfinite) { double d = to_num(A(0)); return vbool(!is_nan(d) && !is_inf(d)); }
NATIVE(g_parseint)
{
    const char *s = cstr(A(0));
    while (ws(*s)) s++;
    int radix = A(1).t == VU ? 0 : to_int(A(1)), neg = 0;
    if (*s == '-' || *s == '+') neg = *s++ == '-';
    if ((radix == 0 || radix == 16) && s[0] == '0' && (s[1] | 32) == 'x') { s += 2; radix = 16; }
    if (radix == 0) radix = 10;
    double v = 0;
    int any = 0;
    for (;; s++) {
        int c = *s, d = c >= '0' && c <= '9' ? c - '0' : (c | 32) >= 'a' && (c | 32) <= 'z' ? (c | 32) - 'a' + 10 : 99;
        if (d >= radix) break;
        v = v * radix + d;
        any = 1;
    }
    return any ? vnum(neg ? -v : v) : vnum(0.0 / 0.0);
}
NATIVE(g_parsefloat)
{
    const char *s = cstr(A(0)), *e;
    while (ws(*s)) s++;
    double d = num_parse(s, &e);
    return e == s ? vnum(0.0 / 0.0) : vnum(d);
}

static u32 rng_state = 0x9E3779B9;
NATIVE(m_random)
{
    rng_state ^= rng_state << 13; rng_state ^= rng_state >> 17; rng_state ^= rng_state << 5;
    return vnum((rng_state >> 8) / 16777216.0);
}
NATIVE(m_floor) { return vnum(d_floor(to_num(A(0)))); }
NATIVE(m_ceil) { return vnum(d_ceil(to_num(A(0)))); }
NATIVE(m_round) { return vnum(d_floor(to_num(A(0)) + 0.5)); }
NATIVE(m_trunc) { return vnum(d_trunc(to_num(A(0)))); }
NATIVE(m_abs) { return vnum(d_abs(to_num(A(0)))); }
NATIVE(m_sqrt) { double d = to_num(A(0)); return vnum(d < 0 ? 0.0 / 0.0 : d_sqrt(d)); }
NATIVE(m_pow) { return vnum(d_pow(to_num(A(0)), to_num(A(1)))); }
NATIVE(m_sin) { return vnum(d_sin(to_num(A(0)))); }
NATIVE(m_cos) { return vnum(d_cos(to_num(A(0)))); }
NATIVE(m_tan) { double x = to_num(A(0)); return vnum(d_sin(x) / d_cos(x)); }
NATIVE(m_atan) { return vnum(d_atan(to_num(A(0)))); }
NATIVE(m_atan2) { return vnum(d_atan2(to_num(A(0)), to_num(A(1)))); }
NATIVE(m_exp) { return vnum(d_exp(to_num(A(0)))); }
NATIVE(m_log) { return vnum(d_log(to_num(A(0)))); }
NATIVE(m_sign) { double d = to_num(A(0)); return vnum(d > 0 ? 1 : d < 0 ? -1 : d); }
NATIVE(m_hypot) { double s = 0; for (int i = 0; i < n; i++) { double d = to_num(args[i]); s += d * d; } return vnum(d_sqrt(s)); }
NATIVE(m_max)
{
    double r = -1.0 / 0.0;
    for (int i = 0; i < n; i++) { double d = to_num(args[i]); if (is_nan(d)) return vnum(d); if (d > r) r = d; }
    return vnum(r);
}
NATIVE(m_min)
{
    double r = 1.0 / 0.0;
    for (int i = 0; i < n; i++) { double d = to_num(args[i]); if (is_nan(d)) return vnum(d); if (d < r) r = d; }
    return vnum(r);
}

/* ---- JSON ---- */

static void json_str(Str *s, char **buf, int *len, int *cap);

static void jb_put(char **buf, int *len, int *cap, const char *s, int n)
{
    if (*len + n + 1 > *cap) {
        int nc = (*cap ? *cap * 2 : 256);
        while (nc < *len + n + 1) nc *= 2;
        char *nb = realloc(*buf, nc);
        if (!nb) return;
        *buf = nb;
        *cap = nc;
    }
    memcpy(*buf + *len, s, n);
    *len += n;
}

static void json_val(V v, char **buf, int *len, int *cap, int depth)
{
    if (depth > 64) { jb_put(buf, len, cap, "null", 4); return; }
    switch (v.t) {
    case VNULL: case VU: jb_put(buf, len, cap, "null", 4); return;
    case VBOOL: jb_put(buf, len, cap, v.b ? "true" : "false", v.b ? 4 : 5); return;
    case VNUM: {
        if (is_nan(v.n) || is_inf(v.n)) { jb_put(buf, len, cap, "null", 4); return; }
        Str *s = to_str(v);
        jb_put(buf, len, cap, s->c, s->len);
        return;
    }
    case VSTR: json_str(v.s, buf, len, cap); return;
    default:
        if (is_callable(v)) { jb_put(buf, len, cap, "null", 4); return; }
        if (v.o->cls == O_ARRAY) {
            jb_put(buf, len, cap, "[", 1);
            for (int i = 0; i < v.o->alen; i++) {
                if (i) jb_put(buf, len, cap, ",", 1);
                json_val(v.o->a[i], buf, len, cap, depth + 1);
            }
            jb_put(buf, len, cap, "]", 1);
            return;
        }
        jb_put(buf, len, cap, "{", 1);
        int first = 1;
        for (int i = 0; i < v.o->np; i++) {
            V pv = v.o->p[i].v;
            if (pv.t == VU || is_callable(pv) || !strncmp(v.o->p[i].k->c, "__", 2)) continue;
            if (!first) jb_put(buf, len, cap, ",", 1);
            first = 0;
            json_str(v.o->p[i].k, buf, len, cap);
            jb_put(buf, len, cap, ":", 1);
            json_val(pv, buf, len, cap, depth + 1);
        }
        jb_put(buf, len, cap, "}", 1);
    }
}

static void json_str(Str *s, char **buf, int *len, int *cap)
{
    jb_put(buf, len, cap, "\"", 1);
    for (int i = 0; i < s->len; i++) {
        char c = s->c[i];
        char esc[8];
        if (c == '"' || c == '\\') { esc[0] = '\\'; esc[1] = c; jb_put(buf, len, cap, esc, 2); }
        else if (c == '\n') jb_put(buf, len, cap, "\\n", 2);
        else if (c == '\r') jb_put(buf, len, cap, "\\r", 2);
        else if (c == '\t') jb_put(buf, len, cap, "\\t", 2);
        else if ((u8)c < 32) { snprintf(esc, sizeof esc, "\\u%04x", (u8)c); jb_put(buf, len, cap, esc, 6); }
        else jb_put(buf, len, cap, &c, 1);
    }
    jb_put(buf, len, cap, "\"", 1);
}

NATIVE(json_stringify)
{
    if (A(0).t == VU) return undef();
    char *buf = NULL;
    int len = 0, cap = 0;
    json_val(A(0), &buf, &len, &cap, 0);
    V r = vstr(str_new(buf ? buf : "", len));
    free(buf);
    return r;
}

struct jp { const char *s, *e; int err; };

static void jp_ws(struct jp *p) { while (p->s < p->e && ws(*p->s)) p->s++; }

static V jp_val(struct jp *p, int depth)
{
    jp_ws(p);
    if (p->s >= p->e || depth > 200) { p->err = 1; return undef(); }
    char c = *p->s;
    if (c == '{') {
        p->s++;
        Obj *o = obj_new(O_PLAIN, J->object_proto);
        jp_ws(p);
        if (p->s < p->e && *p->s == '}') { p->s++; return vobj(o); }
        for (;;) {
            jp_ws(p);
            V k = jp_val(p, depth + 1);
            if (p->err || k.t != VSTR) { p->err = 1; return undef(); }
            jp_ws(p);
            if (p->s >= p->e || *p->s != ':') { p->err = 1; return undef(); }
            p->s++;
            V v = jp_val(p, depth + 1);
            if (p->err) return undef();
            own_put(o, istr(k.s), v);
            jp_ws(p);
            if (p->s < p->e && *p->s == ',') { p->s++; continue; }
            if (p->s < p->e && *p->s == '}') { p->s++; return vobj(o); }
            p->err = 1;
            return undef();
        }
    }
    if (c == '[') {
        p->s++;
        Obj *a = arr_new();
        jp_ws(p);
        if (p->s < p->e && *p->s == ']') { p->s++; return vobj(a); }
        for (;;) {
            V v = jp_val(p, depth + 1);
            if (p->err) return undef();
            arr_push(a, v);
            jp_ws(p);
            if (p->s < p->e && *p->s == ',') { p->s++; continue; }
            if (p->s < p->e && *p->s == ']') { p->s++; return vobj(a); }
            p->err = 1;
            return undef();
        }
    }
    if (c == '"') {
        const char *st = ++p->s;
        while (p->s < p->e && *p->s != '"') { if (*p->s == '\\') p->s++; p->s++; }
        if (p->s >= p->e) { p->err = 1; return undef(); }
        V r = vstr(unescape(st, p->s));
        p->s++;
        return r;
    }
    if (!strncmp(p->s, "true", 4)) { p->s += 4; return vbool(1); }
    if (!strncmp(p->s, "false", 5)) { p->s += 5; return vbool(0); }
    if (!strncmp(p->s, "null", 4)) { p->s += 4; return vnull(); }
    const char *end;
    double d = num_parse(p->s, &end);
    if (end == p->s) { p->err = 1; return undef(); }
    p->s = end;
    return vnum(d);
}

NATIVE(json_parse)
{
    Str *s = to_str(A(0));
    struct jp p = {s->c, s->c + s->len, 0};
    V v = jp_val(&p, 0);
    jp_ws(&p);
    if (p.err || p.s != p.e) return throw_err("SyntaxError", "JSON.parse: bad JSON", NULL);
    return v;
}

/* ---- errors, console, misc ---- */

NATIVE(err_ctor)
{
    Obj *e = obj_new(O_ERROR, J->error_proto);
    if (self.t == VOBJ && g_constructing) { e = self.o; e->cls = O_ERROR; }
    V nm = g_callee ? get(vobj(g_callee), S_name) : vcstr("Error");
    own_put(e, S_name, nm);
    own_put(e, S_message, A(0).t == VU ? vcstr("") : vstr(to_str(A(0))));
    return vobj(e);
}

static void log_append(const char *s)
{
    int l = strlen(J->log);
    if (l > (int)sizeof J->log - 200) { J->log[0] = 0; l = 0; }
    snprintf(J->log + l, sizeof J->log - l, "%s\n", s);
}

NATIVE(con_log)
{
    char line[600];
    int o = 0;
    for (int i = 0; i < n && o < (int)sizeof line - 2; i++) {
        Str *s;
        if (args[i].t == VOBJ && !is_callable(args[i]) && args[i].o->cls != O_ERROR) {
            char *buf = NULL;
            int len = 0, cap = 0;
            json_val(args[i], &buf, &len, &cap, 0);
            s = str_new(buf ? buf : "", len);
            free(buf);
        } else {
            s = to_str(args[i]);
        }
        if (i) line[o++] = ' ';
        int c = s->len < (int)sizeof line - 2 - o ? s->len : (int)sizeof line - 2 - o;
        memcpy(line + o, s->c, c);
        o += c;
    }
    line[o] = 0;
    log_append(line);
    fprintf(2, "[js] %s\n", line);
    return undef();
}

NATIVE(date_now) { return vnum((double)J->now_ms + 1700000000000.0); }

NATIVE(date_ctor)
{
    Obj *d = obj_new(O_PLAIN, J->object_proto);
    struct rtc_time t;
    gettime(&t);
    own_put(d, intern_c("__y"), vnum(t.year));
    own_put(d, intern_c("__mo"), vnum(t.month - 1));
    own_put(d, intern_c("__d"), vnum(t.day));
    own_put(d, intern_c("__h"), vnum(t.hour));
    own_put(d, intern_c("__mi"), vnum(t.min));
    own_put(d, intern_c("__s"), vnum(t.sec));
    struct prop *pp = own_find(J->global, intern_c("Date"));
    if (pp && pp->v.t == VOBJ) {
        V proto = get(pp->v, S_prototype);
        if (proto.t == VOBJ) d->proto = proto.o;
    }
    if (self.t == VOBJ && g_constructing) { self.o->np = d->np; self.o->cap = d->cap; self.o->p = d->p; return self; }
    return vobj(d);
}

#define DATE_GETTER(name, field) NATIVE(name) { return get(self, intern_c(field)); }
DATE_GETTER(date_year, "__y")
DATE_GETTER(date_month, "__mo")
DATE_GETTER(date_date, "__d")
DATE_GETTER(date_hours, "__h")
DATE_GETTER(date_minutes, "__mi")
DATE_GETTER(date_seconds, "__s")
NATIVE(date_gettime) { return date_now(self, args, n); }
NATIVE(date_tostring)
{
    char b[64];
    snprintf(b, sizeof b, "%04d-%02d-%02d %02d:%02d:%02d", to_int(get(self, intern_c("__y"))), to_int(get(self, intern_c("__mo"))) + 1,
             to_int(get(self, intern_c("__d"))), to_int(get(self, intern_c("__h"))), to_int(get(self, intern_c("__mi"))),
             to_int(get(self, intern_c("__s"))));
    return vcstr(b);
}

NATIVE(bool_ctor) { return vbool(truthy(A(0))); }

/* Map/Set: kept simple, insertion-ordered arrays of entries. */
static Obj *map_entries(V self)
{
    V e = get(self, intern_c("__entries"));
    return e.t == VOBJ ? e.o : NULL;
}
static int map_find(Obj *ents, V k)
{
    for (int i = 0; i < ents->alen; i++) {
        V pair = ents->a[i];
        if (pair.t == VOBJ && pair.o->alen && strict_eq(pair.o->a[0], k)) return i;
    }
    return -1;
}
static V map_init(V self, V *args, int n, int is_set)
{
    Obj *m = self.t == VOBJ && g_constructing ? self.o : obj_new(O_PLAIN, J->object_proto);
    Obj *ents = arr_new();
    own_put(m, intern_c("__entries"), vobj(ents));
    V src = A(0);
    if (src.t == VOBJ && src.o->cls == O_ARRAY)
        for (int i = 0; i < src.o->alen; i++) {
            Obj *pair = arr_new();
            if (is_set) { arr_push(pair, src.o->a[i]); arr_push(pair, src.o->a[i]); }
            else if (src.o->a[i].t == VOBJ && src.o->a[i].o->cls == O_ARRAY) { arr_push(pair, get_idx(src.o->a[i], vnum(0))); arr_push(pair, get_idx(src.o->a[i], vnum(1))); }
            if (map_find(ents, pair->alen ? pair->a[0] : undef()) < 0) arr_push(ents, vobj(pair));
        }
    return vobj(m);
}
NATIVE(map_ctor) { return map_init(self, args, n, 0); }
NATIVE(set_ctor) { return map_init(self, args, n, 1); }
NATIVE(map_get) { Obj *e = map_entries(self); int i = e ? map_find(e, A(0)) : -1; return i >= 0 ? e->a[i].o->a[1] : undef(); }
NATIVE(map_has) { Obj *e = map_entries(self); return vbool(e && map_find(e, A(0)) >= 0); }
NATIVE(map_set)
{
    Obj *e = map_entries(self);
    if (!e) return self;
    int i = map_find(e, A(0));
    if (i >= 0) { e->a[i].o->a[1] = A(1); return self; }
    Obj *pair = arr_new();
    arr_push(pair, A(0));
    arr_push(pair, A(1));
    arr_push(e, vobj(pair));
    return self;
}
NATIVE(set_add) { V a2[2] = {A(0), A(0)}; return map_set(self, a2, 2); }
NATIVE(map_delete)
{
    Obj *e = map_entries(self);
    int i = e ? map_find(e, A(0)) : -1;
    if (i < 0) return vbool(0);
    memmove(e->a + i, e->a + i + 1, (e->alen - i - 1) * sizeof *e->a);
    e->alen--;
    return vbool(1);
}
NATIVE(map_clear) { Obj *e = map_entries(self); if (e) e->alen = 0; return undef(); }
NATIVE(map_size_fn) { Obj *e = map_entries(self); return vnum(e ? e->alen : 0); }
NATIVE(map_entries_fn) { Obj *e = map_entries(self); Obj *r = arr_new(); for (int i = 0; e && i < e->alen; i++) arr_push(r, e->a[i]); return vobj(r); }
NATIVE(map_keys) { Obj *e = map_entries(self); Obj *r = arr_new(); for (int i = 0; e && i < e->alen; i++) arr_push(r, e->a[i].o->a[0]); return vobj(r); }
NATIVE(map_values) { Obj *e = map_entries(self); Obj *r = arr_new(); for (int i = 0; e && i < e->alen; i++) arr_push(r, e->a[i].o->a[1]); return vobj(r); }
NATIVE(map_foreach)
{
    Obj *e = map_entries(self);
    V f = A(0);
    if (!is_callable(f)) return undef();
    for (int i = 0; e && i < e->alen; i++) {
        V a3[3] = {e->a[i].o->a[1], e->a[i].o->a[0], self};
        call(f, undef(), a3, 3);
        if (J->thrown) break;
    }
    return undef();
}

/* ============================== setup ============================== */

static Obj *ctor(const char *name, native_fn fn, Obj *proto)
{
    Obj *c = native(name, fn, 0);
    if (proto) {
        own_put(c, S_prototype, vobj(proto));
        own_put(proto, S_constructor, vobj(c));
    }
    own_put(J->global, intern_c(name), vobj(c));
    return c;
}

static void stdlib_init(void)
{
    S_length = intern_c("length");
    S_prototype = intern_c("prototype");
    S_constructor = intern_c("constructor");
    S_message = intern_c("message");
    S_name = intern_c("name");
    S_toString = intern_c("toString");
    S_callee = intern_c("callee");

    J->object_proto = obj_new(O_PLAIN, NULL);
    J->func_proto = obj_new(O_PLAIN, J->object_proto);
    J->array_proto = obj_new(O_PLAIN, J->object_proto);
    J->string_proto = obj_new(O_PLAIN, J->object_proto);
    J->number_proto = obj_new(O_PLAIN, J->object_proto);
    J->bool_proto = obj_new(O_PLAIN, J->object_proto);
    J->error_proto = obj_new(O_PLAIN, J->object_proto);
    J->regexp_proto = obj_new(O_PLAIN, J->object_proto);
    J->global = obj_new(O_PLAIN, J->object_proto);
    J->global->hosted = 1;
    Obj *g = J->global;

    defn(J->object_proto, "hasOwnProperty", obj_hasown);
    defn(J->object_proto, "toString", obj_tostring);
    Obj *objc = ctor("Object", obj_ctor, J->object_proto);
    defn(objc, "keys", obj_keys);
    defn(objc, "values", obj_values);
    defn(objc, "entries", obj_entries);
    defn(objc, "assign", obj_assign);
    defn(objc, "create", obj_create);
    defn(objc, "getPrototypeOf", obj_getproto);
    defn(objc, "setPrototypeOf", obj_setproto);
    defn(objc, "freeze", obj_freeze);
    defn(objc, "seal", obj_freeze);
    defn(objc, "defineProperty", obj_defprop);
    defn(objc, "is", obj_is);

    defn(J->func_proto, "call", fn_call);
    defn(J->func_proto, "apply", fn_apply);
    defn(J->func_proto, "bind", fn_bind);
    ctor("Function", obj_ctor, J->func_proto);

    Obj *ap = J->array_proto;
    defn(ap, "push", arr_push_n); defn(ap, "pop", arr_pop); defn(ap, "shift", arr_shift); defn(ap, "unshift", arr_unshift);
    defn(ap, "join", arr_join); defn(ap, "slice", arr_slice); defn(ap, "splice", arr_splice); defn(ap, "indexOf", arr_indexof);
    defn(ap, "lastIndexOf", arr_lastindexof); defn(ap, "includes", arr_includes); defn(ap, "concat", arr_concat);
    defn(ap, "reverse", arr_reverse); defn(ap, "forEach", arr_foreach); defn(ap, "map", arr_map); defn(ap, "filter", arr_filter);
    defn(ap, "find", arr_find); defn(ap, "findIndex", arr_findindex); defn(ap, "some", arr_some); defn(ap, "every", arr_every);
    defn(ap, "reduce", arr_reduce); defn(ap, "sort", arr_sort); defn(ap, "fill", arr_fill); defn(ap, "flat", arr_flat);
    defn(ap, "entries", arr_entries); defn(ap, "keys", arr_keys); defn(ap, "values", arr_slice); defn(ap, "toString", obj_tostring);
    Obj *arrc = ctor("Array", arr_ctor, ap);
    defn(arrc, "isArray", arr_isarray);
    defn(arrc, "from", arr_from);
    defn(arrc, "of", arr_of);

    Obj *sp = J->string_proto;
    defn(sp, "charAt", str_charat); defn(sp, "charCodeAt", str_charcodeat); defn(sp, "codePointAt", str_charcodeat);
    defn(sp, "indexOf", str_indexof); defn(sp, "lastIndexOf", str_lastindexof); defn(sp, "includes", str_includes);
    defn(sp, "startsWith", str_startswith); defn(sp, "endsWith", str_endswith); defn(sp, "slice", str_slice);
    defn(sp, "substring", str_substring); defn(sp, "substr", str_substr); defn(sp, "toUpperCase", str_upper);
    defn(sp, "toLowerCase", str_lower); defn(sp, "toLocaleUpperCase", str_upper); defn(sp, "toLocaleLowerCase", str_lower);
    defn(sp, "trim", str_trim); defn(sp, "trimStart", str_trimstart); defn(sp, "trimEnd", str_trimend); defn(sp, "split", str_split);
    defn(sp, "replace", str_replace); defn(sp, "replaceAll", str_replaceall); defn(sp, "repeat", str_repeat);
    defn(sp, "padStart", str_padstart); defn(sp, "padEnd", str_padend); defn(sp, "concat", str_concat);
    defn(sp, "toString", str_tostring); defn(sp, "valueOf", str_tostring); defn(sp, "localeCompare", str_localecompare);
    defn(sp, "match", str_match);
    Obj *strc = ctor("String", str_ctor, sp);
    defn(strc, "fromCharCode", str_fromcharcode);
    defn(strc, "fromCodePoint", str_fromcharcode);

    defn(J->regexp_proto, "test", re_test);
    ctor("RegExp", re_ctor, J->regexp_proto);

    defn(J->number_proto, "toFixed", num_tofixed);
    defn(J->number_proto, "toString", num_tostring);
    defn(J->number_proto, "toLocaleString", num_tostring);
    Obj *numc = ctor("Number", num_ctor, J->number_proto);
    defn(numc, "isInteger", num_isinteger);
    defn(numc, "isNaN", g_isnan);
    defn(numc, "isFinite", g_isfinite);
    defn(numc, "parseFloat", g_parsefloat);
    defn(numc, "parseInt", g_parseint);
    own_put(numc, intern_c("MAX_SAFE_INTEGER"), vnum(9007199254740991.0));
    own_put(numc, intern_c("MIN_SAFE_INTEGER"), vnum(-9007199254740991.0));
    own_put(numc, intern_c("EPSILON"), vnum(2.220446049250313e-16));
    own_put(numc, intern_c("MAX_VALUE"), vnum(1.7976931348623157e308));
    own_put(numc, intern_c("POSITIVE_INFINITY"), vnum(1.0 / 0.0));
    own_put(numc, intern_c("NEGATIVE_INFINITY"), vnum(-1.0 / 0.0));
    ctor("Boolean", bool_ctor, J->bool_proto);

    defn(J->error_proto, "toString", obj_tostring);
    own_put(J->error_proto, S_name, vcstr("Error"));
    own_put(J->error_proto, S_message, vcstr(""));
    static const char *const errs[] = {"Error", "TypeError", "RangeError", "SyntaxError", "ReferenceError", NULL};
    for (int i = 0; errs[i]; i++) ctor(errs[i], err_ctor, i ? obj_new(O_PLAIN, J->error_proto) : J->error_proto);

    Obj *math = obj_new(O_PLAIN, J->object_proto);
    own_put(g, intern_c("Math"), vobj(math));
    own_put(math, intern_c("PI"), vnum(3.141592653589793));
    own_put(math, intern_c("E"), vnum(2.718281828459045));
    own_put(math, intern_c("SQRT2"), vnum(1.4142135623730951));
    own_put(math, intern_c("LN2"), vnum(0.6931471805599453));
    defn(math, "random", m_random); defn(math, "floor", m_floor); defn(math, "ceil", m_ceil); defn(math, "round", m_round);
    defn(math, "trunc", m_trunc); defn(math, "abs", m_abs); defn(math, "sqrt", m_sqrt); defn(math, "pow", m_pow);
    defn(math, "min", m_min); defn(math, "max", m_max); defn(math, "sin", m_sin); defn(math, "cos", m_cos);
    defn(math, "tan", m_tan); defn(math, "atan", m_atan); defn(math, "atan2", m_atan2); defn(math, "exp", m_exp);
    defn(math, "log", m_log); defn(math, "sign", m_sign); defn(math, "hypot", m_hypot);

    Obj *json = obj_new(O_PLAIN, J->object_proto);
    own_put(g, intern_c("JSON"), vobj(json));
    defn(json, "stringify", json_stringify);
    defn(json, "parse", json_parse);

    Obj *con = obj_new(O_PLAIN, J->object_proto);
    own_put(g, intern_c("console"), vobj(con));
    defn(con, "log", con_log); defn(con, "info", con_log); defn(con, "warn", con_log); defn(con, "error", con_log);
    defn(con, "debug", con_log);

    Obj *dp = obj_new(O_PLAIN, J->object_proto);
    defn(dp, "getFullYear", date_year); defn(dp, "getMonth", date_month); defn(dp, "getDate", date_date);
    defn(dp, "getHours", date_hours); defn(dp, "getMinutes", date_minutes); defn(dp, "getSeconds", date_seconds);
    defn(dp, "getTime", date_gettime); defn(dp, "valueOf", date_gettime); defn(dp, "toString", date_tostring);
    defn(dp, "toLocaleTimeString", date_tostring); defn(dp, "toLocaleDateString", date_tostring);
    defn(dp, "toLocaleString", date_tostring); defn(dp, "toISOString", date_tostring);
    Obj *datec = ctor("Date", date_ctor, dp);
    defn(datec, "now", date_now);

    Obj *mp = obj_new(O_PLAIN, J->object_proto);
    defn(mp, "get", map_get); defn(mp, "set", map_set); defn(mp, "has", map_has); defn(mp, "delete", map_delete);
    defn(mp, "clear", map_clear); defn(mp, "entries", map_entries_fn); defn(mp, "keys", map_keys);
    defn(mp, "values", map_values); defn(mp, "forEach", map_foreach); defn(mp, "size", map_size_fn);
    ctor("Map", map_ctor, mp);
    Obj *setp = obj_new(O_PLAIN, J->object_proto);
    defn(setp, "add", set_add); defn(setp, "has", map_has); defn(setp, "delete", map_delete); defn(setp, "clear", map_clear);
    defn(setp, "entries", map_entries_fn); defn(setp, "values", map_keys); defn(setp, "keys", map_keys);
    defn(setp, "forEach", map_foreach); defn(setp, "size", map_size_fn);
    ctor("Set", set_ctor, setp);

    defn(g, "parseInt", g_parseint);
    defn(g, "parseFloat", g_parsefloat);
    defn(g, "isNaN", g_isnan);
    defn(g, "isFinite", g_isfinite);
    own_put(g, intern_c("NaN"), vnum(0.0 / 0.0));
    own_put(g, intern_c("Infinity"), vnum(1.0 / 0.0));
    own_put(g, intern_c("undefined"), undef());
    own_put(g, intern_c("globalThis"), vobj(g));
}

/* ============================== the API (web.h) ============================== */

static struct jsctx *ctx_of(struct page *p) { return p->js; }

void js_page_start(struct page *p)
{
    js_page_end(p);
    struct jsctx *c = calloc(1, sizeof *c);
    if (!c) return;
    c->intern = calloc(INTERN_N, sizeof *c->intern);
    if (!c->intern) { free(c); return; }
    c->page = p;
    c->step_limit = 30000000;
    c->next_id = 1;
    p->js = c;
    J = c;
    rng_state ^= uptime() * 2654435761u;
    stdlib_init();
    dom_init_globals(c->global);
}

void js_page_end(struct page *p)
{
    struct jsctx *c = p->js;
    if (!c) return;
    for (struct jsmem *m = c->mem; m;) {
        struct jsmem *nx = m->next;
        free(m);
        m = nx;
    }
    free(c->lis);
    free(c->intern);
    free(c);
    p->js = NULL;
    if (J == c) J = NULL;
    for (struct node *n = p->root; n;) {                        /* forget the node <-> object links */
        n->js = NULL;
        if (n->first) { n = n->first; continue; }
        while (n && !n->next) n = n->parent;
        if (n) n = n->next;
    }
}

static void report(const char *where)
{
    V e = J->exc;
    char line[300];
    Str *s = to_str(e);
    if (e.t == VOBJ && e.o->cls == O_ERROR) {
        Str *nm = to_str(get(e, S_name)), *msg = to_str(get(e, S_message));
        snprintf(line, sizeof line, "%s: %s: %s", where, nm->c, msg->c);
    } else {
        snprintf(line, sizeof line, "%s: uncaught %s", where, s->c);
    }
    J->thrown = 0;
    J->exc = undef();
    log_append(line);
    fprintf(2, "[js] %s\n", line);
}

static int enter(struct page *p)
{
    if (!p->js) return 0;
    J = ctx_of(p);
    if (J->aborted) return 0;
    if (J->depth == 0) {                                          /* not nested inside a running script */
        J->steps = 0;
        J->thrown = 0;
    }
    J->now_ms = uptime() * 10;
    return 1;
}

void js_run(struct page *p, const char *src, int len, const char *name)
{
    if (!enter(p)) return;
    char err[200];
    struct ast *prog = parse_program(src, len, err, sizeof err);
    if (!prog) {
        char line[300];
        snprintf(line, sizeof line, "%s: %s", name, err);
        log_append(line);
        fprintf(2, "[js] %s\n", line);
        return;
    }
    struct env *top = env_new(NULL);
    top->has_this = 1;
    top->thisv = vobj(J->global);
    /* top-level `var`s and functions become properties of window */
    for (int i = 0; i < prog->n; i++) {
        struct ast *s = prog->l[i];
        if (s->k == S_FUNC) own_put(J->global, s->s, vobj(make_func(s->a, top)));
        else if (s->k == S_VAR && !s->flags)
            for (int k = 0; k < s->n; k++)
                if (!own_find(J->global, s->l[k]->s)) own_put(J->global, s->l[k]->s, undef());
    }
    hoist(prog, top, top, 0);
    for (int i = 0; i < top->n; i++)                              /* nested vars also go to window */
        if (!own_find(J->global, top->v[i].k)) own_put(J->global, top->v[i].k, top->v[i].v);
    top->n = 0;
    for (int i = 0; i < prog->n && !J->aborted; i++) {
        struct ast *s = prog->l[i];
        if (s->k == S_FUNC) continue;
        int c = run_stmt(s, top);
        if (c == C_THROW) { report(name); break; }
    }
    if (J->thrown) report(name);
}

/* Calls fn with args from outside any script (events, timers). */
void js_invoke(V fn, V thisv, V *args, int n, const char *where)
{
    call(fn, thisv, args, n);
    if (J->thrown) report(where);
}

const char *js_log(struct page *p) { return p->js ? p->js->log : ""; }
int js_dirty(struct page *p)
{
    if (!p->js) return 0;
    int d = p->js->dom_dirty;
    p->js->dom_dirty = 0;
    return d;
}
int js_stopped(struct page *p) { return p->js && p->js->aborted; }

void js_report_public(const char *where) { if (J->thrown) report(where); }

/* Used by jsdom.c */
int js_enter(struct page *p) { return enter(p); }
