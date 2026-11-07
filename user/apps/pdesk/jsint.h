/* Internals of the browser's JavaScript engine (js.c: language, jsdom.c: the DOM and window).
 * Both files are compiled with SSE enabled: numbers are IEEE doubles.  Nothing here crosses into the rest of
 * the desktop, which only sees the API in web.h. */
#pragma once
#include "web.h"

typedef struct jsobj Obj;
typedef struct jsstr Str;

enum { VU, VNULL, VBOOL, VNUM, VSTR, VOBJ };

typedef struct {
    u8 t;
    union {
        double n;
        int b;
        Str *s;
        Obj *o;
    };
} V;

struct jsstr {
    int len;
    u32 hash;
    Str *inext;                                /* intern-table chain */
    u8 interned;
    char c[];                                  /* NUL-terminated */
};

struct prop {
    Str *k;                                    /* interned */
    V v;
};

typedef V (*native_fn)(V self, V *args, int n);

enum { O_PLAIN, O_ARRAY, O_FUNC, O_NATIVE, O_ERROR, O_NODE, O_STYLE, O_CLASSLIST, O_EVENT, O_REGEXP, O_BOUND,
       O_STORAGE };

struct ast;
struct env;

struct jsobj {
    u8 cls;
    u8 hosted;                                 /* reads and writes go through host_get/host_put first */
    int np, cap;
    struct prop *p;
    Obj *proto;
    V *a;                                      /* arrays */
    int alen, acap;
    struct ast *fn;                            /* script functions */
    struct env *env;
    native_fn native;                          /* native functions */
    V data;                                    /* natives: bound target, timer id, ... */
    V bthis;                                   /* bound functions: `this` */
    struct node *host;                         /* DOM objects */
    Obj *home;                                 /* class methods: the class (for super) */
};

struct env {
    struct env *up;
    struct prop *v;
    int n, cap;
    V thisv;
    u8 has_this;
    Obj *func;                                 /* function this env belongs to (super lookups) */
};

/* engine (js.c) */
extern struct jsctx *J;
V undef(void);
V vnull(void);
V vbool(int b);
V vnum(double d);
V vstr(Str *s);
V vobj(Obj *o);
V vcstr(const char *s);
Str *str_new(const char *s, int len);
Str *intern(const char *s, int len);
Str *intern_c(const char *s);
Str *to_str(V v);
double to_num(V v);
int truthy(V v);
Obj *obj_new(int cls, Obj *proto);
Obj *arr_new(void);
void arr_push(Obj *a, V v);
V get(V o, Str *k);
void put(V o, Str *k, V v);
void own_put(Obj *o, Str *k, V v);
struct prop *own_find(Obj *o, Str *k);
V call(V f, V thisv, V *args, int n);
V throw_err(const char *kind, const char *fmt, ...);
Obj *native(const char *name, native_fn fn, int nargs_unused);
void defn(Obj *o, const char *name, native_fn fn);
int is_callable(V v);
void *jalloc(int n);
V arg(V *a, int n, int i);
const char *cstr(V v);                         /* to_str(v)->c */
int is_obj(V v, int cls);

/* the DOM (jsdom.c) */
void dom_init_globals(Obj *global);
V node_value(struct node *n);                  /* the JS object for a DOM node (null for NULL) */
int host_get(Obj *o, Str *k, V *out);          /* 1 if handled */
int host_put(Obj *o, Str *k, V v);
void timers_reset(void);

struct jsctx {
    struct jsmem *mem;
    u32 used;
    Str **intern;
    Obj *global, *object_proto, *func_proto, *array_proto, *string_proto, *number_proto, *bool_proto, *error_proto,
        *node_proto, *event_proto, *regexp_proto, *style_proto, *classlist_proto;
    struct page *page;
    int thrown;
    V exc;
    V retval;
    long steps, step_limit;
    int depth;
    int aborted;                               /* out of memory or runaway: scripts are off for this page */
    int dom_dirty;                             /* the tree or styles changed: restyle and relayout */
    int want_paint;
    char nav[512];                             /* location.href = ... */
    char alert_text[200];
    int has_alert;
    struct node *focus;
    /* timers */
    struct jtimer { int id, interval, alive; u32 due; V fn; V *args; int nargs; } timers[64];
    int ntimers, next_id;
    u32 now_ms;
    /* event listeners */
    struct jlis { struct node *n; int is_doc; Str *type; V fn; } *lis;
    int nlis, caplis;
    int default_prevented, stop_prop;
    char log[2048];                            /* the last console output, shown in the status bar */
    Obj *doc_o, *win_o, *loc_o, *store_o;      /* document, window, location, localStorage */
    char storage_path[96];
};
