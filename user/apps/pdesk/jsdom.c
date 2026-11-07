/* JavaScript's view of the page: window, document, elements, events, timers and localStorage.
 * Elements are the browser's own DOM nodes (web.h); every change marks the page for restyle and relayout. */
#include "jsint.h"

#define NATIVE(name) static V name(V self, V *args, int n)
#define A(i) arg(args, n, i)

extern Obj *g_callee;
int js_enter(struct page *p);
void js_invoke(V fn, V thisv, V *args, int n, const char *where);
void js_report_public(const char *where);

#define document_obj (J->doc_o)
#define window_obj   (J->win_o)
#define location_obj (J->loc_o)
#define storage_obj  (J->store_o)
#define storage_path (J->storage_path)

static void dirty(void) { J->dom_dirty = 1; }

/* ---- helpers ---- */

static struct node *node_of(V v)
{
    if (v.t == VOBJ && v.o->cls == O_NODE) return v.o->host;
    return NULL;
}

V node_value(struct node *n)
{
    if (!n) return vnull();
    if (n->js) return vobj((Obj *)n->js);
    Obj *o = obj_new(O_NODE, J->node_proto);
    o->host = n;
    o->hosted = 1;
    n->js = o;
    return vobj(o);
}


static int keyis(Str *k, const char *s) { return !strcmp(k->c, s); }

static char *big_buf(int *cap)
{
    static char *b;
    static int c;
    if (!b) { c = 256 * 1024; b = malloc(c); }
    *cap = c;
    return b;
}

static V text_of(struct node *n)
{
    int cap;
    char *buf = big_buf(&cap);
    if (!buf) return vcstr("");
    int len = dom_text(n, buf, cap);
    return vstr(str_new(buf, len < cap - 1 ? len : cap - 1));
}

static void set_text(struct node *n, const char *s, int len)
{
    struct page *p = J->page;
    if (n->type == 1) { dom_set_text(p, n, s, len); dirty(); return; }
    while (n->first) dom_remove(n->first);
    if (len) {
        struct node *t = dom_new_text(p, s, len);
        if (t) dom_insert(p, n, t, NULL);
    }
    dirty();
}

/* camelCase -> kebab-case */
static void kebab(const char *s, char *out, int cap)
{
    int o = 0;
    for (; *s && o < cap - 2; s++) {
        if (*s >= 'A' && *s <= 'Z') { out[o++] = '-'; out[o++] = *s + 32; }
        else out[o++] = *s;
    }
    out[o] = 0;
}

/* Finds "prop: value" in a style attribute; returns the value's span. */
static const char *style_find(const char *st, const char *prop, int *vlen)
{
    int pl = strlen(prop);
    const char *p = st;
    while (p && *p) {
        while (*p == ' ' || *p == ';' || *p == '\n' || *p == '\t') p++;
        const char *ne = p;
        while (*ne && *ne != ':' && *ne != ';') ne++;
        const char *nt = ne;
        while (nt > p && nt[-1] == ' ') nt--;
        const char *ve = ne;
        while (*ve && *ve != ';') ve++;
        if (*ne == ':' && nt - p == pl && !strncmp(p, prop, pl)) {
            const char *vs = ne + 1;
            while (*vs == ' ') vs++;
            const char *vt = ve;
            while (vt > vs && vt[-1] == ' ') vt--;
            *vlen = vt - vs;
            return vs;
        }
        p = *ve ? ve + 1 : ve;
    }
    return NULL;
}

static void style_set(struct node *n, const char *prop, const char *val)
{
    const char *st = attr_val(n, "style");
    char out[2048];
    int o = 0;
    int pl = strlen(prop);
    const char *p = st;
    while (p && *p) {                                         /* copy every other declaration */
        while (*p == ' ' || *p == ';' || *p == '\n') p++;
        if (!*p) break;
        const char *ve = p;
        while (*ve && *ve != ';') ve++;
        const char *ne = p;
        while (ne < ve && *ne != ':') ne++;
        const char *nt = ne;
        while (nt > p && nt[-1] == ' ') nt--;
        if (!(nt - p == pl && !strncmp(p, prop, pl)) && o + (ve - p) + 3 < (int)sizeof out) {
            memcpy(out + o, p, ve - p);
            o += ve - p;
            out[o++] = ';';
            out[o++] = ' ';
        }
        p = ve;
    }
    if (val && *val && o + pl + (int)strlen(val) + 4 < (int)sizeof out)
        o += snprintf(out + o, sizeof out - o, "%s: %s;", prop, val);
    out[o] = 0;
    dom_set_attr(J->page, n, "style", o ? out : NULL);
    dirty();
}

/* ---- collecting elements ---- */

static void collect(struct node *root, Obj *out, const char *sel, const char *tag, const char *cls, int first_only, int self_too)
{
    for (struct node *n = self_too ? root : root->first; n;) {
        if (n->type == 0) {
            int ok = 1;
            if (sel) ok = css_matches(J->page, n, sel) == 1;
            if (tag) ok = !strcmp(tag, "*") || !strcmp(dom_tag_name(n), tag);
            if (cls) {
                const char *c = attr_val(n, "class");
                ok = 0;
                if (c) {
                    int cl = strlen(cls);
                    for (const char *p = c; *p;) {
                        while (*p == ' ') p++;
                        const char *e = p;
                        while (*e && *e != ' ') e++;
                        if (e - p == cl && !strncmp(p, cls, cl)) { ok = 1; break; }
                        p = e;
                    }
                }
            }
            if (ok && !(self_too && n == root && !sel)) {
                arr_push(out, node_value(n));
                if (first_only) return;
            }
        }
        if (n->first) { n = n->first; continue; }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) break;
        n = n->next;
    }
}

static struct node *by_id(struct node *root, const char *id)
{
    for (struct node *n = root; n;) {
        if (n->type == 0) {
            const char *v = attr_val(n, "id");
            if (v && !strcmp(v, id)) return n;
        }
        if (n->first) { n = n->first; continue; }
        while (n && n != root && !n->next) n = n->parent;
        if (!n || n == root) break;
        n = n->next;
    }
    return NULL;
}

static struct node *self_node(V self)
{
    struct node *n = node_of(self);
    if (!n) throw_err("TypeError", "not a DOM node", NULL);
    return n;
}

#define SELF struct node *me = self_node(self); if (!me) return undef()

/* ---- node methods ---- */

NATIVE(n_getattr) { SELF; const char *v = attr_val(me, cstr(A(0))); return v ? vcstr(v) : vnull(); }
NATIVE(n_hasattr) { SELF; return vbool(attr_val(me, cstr(A(0))) != NULL); }
NATIVE(n_setattr)
{
    SELF;
    dom_set_attr(J->page, me, cstr(A(0)), cstr(A(1)));
    dirty();
    return undef();
}
NATIVE(n_removeattr) { SELF; dom_set_attr(J->page, me, cstr(A(0)), NULL); dirty(); return undef(); }
NATIVE(n_toggleattr)
{
    SELF;
    const char *name = cstr(A(0));
    int on = n > 1 ? truthy(A(1)) : !attr_val(me, name);
    dom_set_attr(J->page, me, name, on ? "" : NULL);
    dirty();
    return vbool(on);
}

static struct node *as_node(V v)
{
    struct node *c = node_of(v);
    if (c) return c;
    if (v.t == VSTR || v.t == VNUM) {                            /* append("text") */
        Str *s = to_str(v);
        return dom_new_text(J->page, s->c, s->len);
    }
    throw_err("TypeError", "argument is not a node", NULL);
    return NULL;
}

/* A document fragment's children move; the fragment itself never goes in the tree. */
static void insert_any(struct node *parent, struct node *c, struct node *before)
{
    if (c->type == 0 && c->tag == TG_OTHER && c->text && !strcmp(c->text, "#fragment")) {
        while (c->first) dom_insert(J->page, parent, c->first, before);
        return;
    }
    dom_insert(J->page, parent, c, before);
}

NATIVE(n_append_child)
{
    SELF;
    struct node *c = as_node(A(0));
    if (!c) return undef();
    insert_any(me, c, NULL);
    dirty();
    return A(0);
}
NATIVE(n_append)
{
    SELF;
    for (int i = 0; i < n; i++) { struct node *c = as_node(args[i]); if (c) insert_any(me, c, NULL); }
    dirty();
    return undef();
}
NATIVE(n_prepend)
{
    SELF;
    struct node *first = me->first;
    for (int i = 0; i < n; i++) { struct node *c = as_node(args[i]); if (c) insert_any(me, c, first); }
    dirty();
    return undef();
}
NATIVE(n_insert_before)
{
    SELF;
    struct node *c = as_node(A(0));
    if (!c) return undef();
    insert_any(me, c, node_of(A(1)));
    dirty();
    return A(0);
}
NATIVE(n_remove_child)
{
    SELF;
    struct node *c = node_of(A(0));
    if (c && c->parent == me) { dom_remove(c); dirty(); }
    return A(0);
}
NATIVE(n_replace_child)
{
    SELF;
    struct node *nw = as_node(A(0)), *old = node_of(A(1));
    if (!nw || !old || old->parent != me) return A(1);
    insert_any(me, nw, old);
    dom_remove(old);
    dirty();
    return A(1);
}
NATIVE(n_replace_with)
{
    SELF;
    struct node *par = me->parent;
    if (!par) return undef();
    for (int i = 0; i < n; i++) { struct node *c = as_node(args[i]); if (c) insert_any(par, c, me); }
    dom_remove(me);
    dirty();
    return undef();
}
NATIVE(n_remove) { SELF; dom_remove(me); dirty(); return undef(); }
NATIVE(n_clone) { SELF; return node_value(dom_clone(J->page, me, truthy(A(0)))); }
NATIVE(n_contains)
{
    SELF;
    for (struct node *x = node_of(A(0)); x; x = x->parent) if (x == me) return vbool(1);
    return vbool(0);
}
NATIVE(n_qs)
{
    SELF;
    const char *sel = cstr(A(0));
    if (css_matches(J->page, me, sel) < 0 && !J->thrown) {
        /* a selector we can't parse: report it once, like a SyntaxError would */
        return throw_err("SyntaxError", "selector not supported: ", sel);
    }
    Obj *r = arr_new();
    collect(me, r, sel, NULL, NULL, 1, 0);
    return r->alen ? r->a[0] : vnull();
}
NATIVE(n_qsa)
{
    SELF;
    Obj *r = arr_new();
    collect(me, r, cstr(A(0)), NULL, NULL, 0, 0);
    return vobj(r);
}
NATIVE(n_by_tag)
{
    SELF;
    Obj *r = arr_new();
    char t[32];
    const char *s = cstr(A(0));
    int i = 0;
    for (; s[i] && i < 31; i++) t[i] = s[i] >= 'A' && s[i] <= 'Z' ? s[i] + 32 : s[i];
    t[i] = 0;
    collect(me, r, NULL, t, NULL, 0, 0);
    return vobj(r);
}
NATIVE(n_by_class) { SELF; Obj *r = arr_new(); collect(me, r, NULL, NULL, cstr(A(0)), 0, 0); return vobj(r); }
NATIVE(n_by_id) { SELF; return node_value(by_id(me, cstr(A(0)))); }
NATIVE(n_matches) { SELF; return vbool(css_matches(J->page, me, cstr(A(0))) == 1); }
NATIVE(n_closest)
{
    SELF;
    const char *sel = cstr(A(0));
    for (struct node *x = me; x; x = x->parent)
        if (x->type == 0 && css_matches(J->page, x, sel) == 1) return node_value(x);
    return vnull();
}
NATIVE(n_focus) { SELF; J->focus = me; css_focus = me; dirty(); return undef(); }
NATIVE(n_blur) { SELF; if (J->focus == me) { J->focus = NULL; css_focus = NULL; dirty(); } return undef(); }
NATIVE(n_click) { SELF; js_event(J->page, me, "click", 0, 0); return undef(); }
NATIVE(n_noop) { return undef(); }
NATIVE(n_insert_html)
{
    SELF;
    const char *where = cstr(A(0));
    Str *html = to_str(A(1));
    struct node *tmp = dom_new_element(J->page, "div");
    if (!tmp) return undef();
    dom_set_html(J->page, tmp, html->c, html->len);
    struct node *par = me, *before = NULL;
    if (!strcmp(where, "beforebegin")) { par = me->parent; before = me; }
    else if (!strcmp(where, "afterbegin")) before = me->first;
    else if (!strcmp(where, "afterend")) { par = me->parent; before = me->next; }
    if (!par) return undef();
    while (tmp->first) dom_insert(J->page, par, tmp->first, before);
    dirty();
    return undef();
}

/* Bounding box from the display list: the union of everything the element (or its content) drew. */
static void bbox(struct node *n, int *x, int *y, int *w, int *h)
{
    struct page *p = J->page;
    int x0 = 1 << 30, y0 = 1 << 30, x1 = -(1 << 30), y1 = -(1 << 30);
    for (int i = 0; i < p->ncmds; i++) {
        struct cmd *c = &p->cmds[i];
        struct node *o = c->node;
        while (o && o != n) o = o->parent;
        if (!o) continue;
        if (c->x < x0) x0 = c->x;
        if (c->y < y0) y0 = c->y;
        if (c->x + c->w > x1) x1 = c->x + c->w;
        if (c->y + c->h > y1) y1 = c->y + c->h;
    }
    if (x1 < x0) { *x = *y = *w = *h = 0; return; }
    *x = x0; *y = y0; *w = x1 - x0; *h = y1 - y0;
}

NATIVE(n_rect)
{
    SELF;
    int x, y, w, h;
    bbox(me, &x, &y, &w, &h);
    Obj *r = obj_new(O_PLAIN, J->object_proto);
    own_put(r, intern_c("x"), vnum(x)); own_put(r, intern_c("left"), vnum(x));
    own_put(r, intern_c("y"), vnum(y)); own_put(r, intern_c("top"), vnum(y));
    own_put(r, intern_c("width"), vnum(w)); own_put(r, intern_c("height"), vnum(h));
    own_put(r, intern_c("right"), vnum(x + w)); own_put(r, intern_c("bottom"), vnum(y + h));
    return vobj(r);
}

/* ---- events ---- */

static Str *S_on_cache;

NATIVE(n_add_listener)
{
    if (!is_callable(A(1)) && !(A(1).t == VOBJ)) return undef();
    struct node *me = node_of(self);
    int is_doc = 0;
    if (self.t == VOBJ && self.o == window_obj) is_doc = 2;
    else if (self.t == VOBJ && self.o == document_obj) is_doc = 1;
    else if (!me) return undef();
    V fn = A(1);
    if (!is_callable(fn)) fn = get(fn, intern_c("handleEvent"));
    if (!is_callable(fn)) return undef();
    Str *type = intern_c(cstr(A(0)));
    for (int i = 0; i < J->nlis; i++)
        if (J->lis[i].n == me && J->lis[i].is_doc == is_doc && J->lis[i].type == type && J->lis[i].fn.o == fn.o)
            return undef();
    if (J->nlis == J->caplis) {
        int nc = J->caplis ? J->caplis * 2 : 32;
        struct jlis *nl = realloc(J->lis, nc * sizeof *nl);
        if (!nl) return undef();
        J->lis = nl;
        J->caplis = nc;
    }
    J->lis[J->nlis++] = (struct jlis){is_doc ? NULL : me, is_doc, type, fn};
    return undef();
}

NATIVE(n_remove_listener)
{
    struct node *me = node_of(self);
    int is_doc = self.t == VOBJ && self.o == window_obj ? 2 : self.t == VOBJ && self.o == document_obj ? 1 : 0;
    Str *type = intern_c(cstr(A(0)));
    for (int i = 0; i < J->nlis; i++)
        if (J->lis[i].n == (is_doc ? NULL : me) && J->lis[i].is_doc == is_doc && J->lis[i].type == type &&
            A(1).t == VOBJ && J->lis[i].fn.o == A(1).o) {
            memmove(&J->lis[i], &J->lis[i + 1], (J->nlis - i - 1) * sizeof *J->lis);
            J->nlis--;
            break;
        }
    return undef();
}

NATIVE(ev_prevent) { J->default_prevented = 1; if (self.t == VOBJ) own_put(self.o, intern_c("defaultPrevented"), vbool(1)); return undef(); }
NATIVE(ev_stop) { J->stop_prop = 1; return undef(); }

/* An inline handler attribute (onclick="...") compiled into a function once. */
static V inline_handler(struct node *n, const char *type)
{
    char an[40];
    snprintf(an, sizeof an, "on%s", type);
    const char *code = attr_val(n, an);
    if (!code) return undef();
    V obj = node_value(n);
    char key[48];
    snprintf(key, sizeof key, "__inline_%s", type);
    struct prop *cached = own_find(obj.o, intern_c(key));
    if (cached) return cached->v;
    int len = strlen(code);
    char *src = malloc(len + 64);
    if (!src) return undef();
    int m = snprintf(src, len + 64, "window.__h = function (event) {\n%s\n};", code);
    js_run(J->page, src, m, "inline handler");
    free(src);
    V f = get(vobj(J->global), intern_c("__h"));
    own_put(obj.o, intern_c(key), f);
    return f;
}

static V make_event(const char *type, struct node *target, int key, int flags)
{
    Obj *e = obj_new(O_EVENT, J->event_proto);
    own_put(e, intern_c("type"), vcstr(type));
    own_put(e, intern_c("target"), node_value(target));
    own_put(e, intern_c("srcElement"), node_value(target));
    own_put(e, intern_c("bubbles"), vbool(1));
    own_put(e, intern_c("defaultPrevented"), vbool(0));
    own_put(e, intern_c("timeStamp"), vnum(J->now_ms));
    if (key) {
        char k[16];
        if (key >= 32 && key < 127) { k[0] = key; k[1] = 0; }
        else if (key == '\n') safestrcpy(k, "Enter", sizeof k);
        else if (key == 8) safestrcpy(k, "Backspace", sizeof k);
        else if (key == 27) safestrcpy(k, "Escape", sizeof k);
        else if (key == '\t') safestrcpy(k, "Tab", sizeof k);
        else if (key == KEY_UP) safestrcpy(k, "ArrowUp", sizeof k);
        else if (key == KEY_DOWN) safestrcpy(k, "ArrowDown", sizeof k);
        else if (key == KEY_LEFT) safestrcpy(k, "ArrowLeft", sizeof k);
        else if (key == KEY_RIGHT) safestrcpy(k, "ArrowRight", sizeof k);
        else if (key == KEY_DELETE) safestrcpy(k, "Delete", sizeof k);
        else if (key == KEY_HOME) safestrcpy(k, "Home", sizeof k);
        else if (key == KEY_END) safestrcpy(k, "End", sizeof k);
        else safestrcpy(k, "Unidentified", sizeof k);
        own_put(e, intern_c("key"), vcstr(k));
        int code = key == '\n' ? 13 : key >= 'a' && key <= 'z' ? key - 32 : key == KEY_LEFT ? 37 : key == KEY_UP ? 38 :
                   key == KEY_RIGHT ? 39 : key == KEY_DOWN ? 40 : key < 128 ? key : 0;
        own_put(e, intern_c("keyCode"), vnum(code));
        own_put(e, intern_c("which"), vnum(code));
    }
    own_put(e, intern_c("shiftKey"), vbool(flags & IEF_SHIFT));
    own_put(e, intern_c("ctrlKey"), vbool(flags & IEF_CTRL));
    own_put(e, intern_c("altKey"), vbool(flags & IEF_ALT));
    own_put(e, intern_c("metaKey"), vbool(0));
    own_put(e, intern_c("button"), vnum(0));
    return vobj(e);
}

static void fire_on(V ev, int is_doc, struct node *n, Str *type, const char *where)
{
    for (int i = 0; i < J->nlis && !J->aborted; i++) {
        if (J->lis[i].is_doc != is_doc || J->lis[i].type != type || (!is_doc && J->lis[i].n != n)) continue;
        V target = is_doc == 2 ? vobj(window_obj) : is_doc == 1 ? vobj(document_obj) : node_value(n);
        own_put(ev.o, intern_c("currentTarget"), target);
        V fn = J->lis[i].fn;
        js_invoke(fn, target, &ev, 1, where);
    }
    /* element.onclick = function / onclick="..." */
    V target = is_doc == 2 ? vobj(window_obj) : is_doc == 1 ? vobj(document_obj) : node_value(n);
    char prop[40];
    snprintf(prop, sizeof prop, "on%s", type->c);
    struct prop *p = own_find(target.o, intern_c(prop));
    V h = p ? p->v : undef();
    if (!is_callable(h) && !is_doc) h = inline_handler(n, type->c);
    if (is_callable(h)) {
        own_put(ev.o, intern_c("currentTarget"), target);
        V tv = target;
        J->steps = 0;
        call(h, tv, &ev, 1);
        if (J->thrown)
            js_report_public(where);
    }
}

int js_event(struct page *p, struct node *target, const char *type, int key, int flags)
{
    if (!p->js) return 0;
    if (!js_enter(p)) return 0;
    int prev_prevented = J->default_prevented, prev_stop = J->stop_prop;
    J->default_prevented = 0;
    J->stop_prop = 0;
    Str *t = intern_c(type);
    V ev = make_event(type, target, key, flags);
    for (struct node *n = target; n && !J->stop_prop && !J->aborted; n = n->parent)
        fire_on(ev, 0, n, t, type);
    if (!J->stop_prop && target) fire_on(ev, 1, NULL, t, type);
    if (!J->stop_prop) fire_on(ev, 2, NULL, t, type);
    int prevented = J->default_prevented;
    J->default_prevented = prev_prevented;
    J->stop_prop = prev_stop;
    return prevented;
}

/* ---- element properties ---- */

static int input_kind(struct node *n, const char *t)
{
    const char *v = attr_val(n, "type");
    return v && !strcmp(v, t);
}

static V style_proxy(struct node *n)
{
    Obj *s = obj_new(O_STYLE, J->style_proto);
    s->host = n;
    s->hosted = 1;
    return vobj(s);
}

static V classlist_proxy(struct node *n)
{
    Obj *s = obj_new(O_CLASSLIST, J->classlist_proto);
    s->host = n;
    s->hosted = 1;
    return vobj(s);
}

static Obj *children_of(struct node *n, int elements_only)
{
    Obj *r = arr_new();
    for (struct node *c = n->first; c; c = c->next)
        if (!elements_only || c->type == 0) arr_push(r, node_value(c));
    return r;
}

static struct node *elem_sib(struct node *n, int fwd)
{
    for (n = fwd ? n->next : n->prev; n; n = fwd ? n->next : n->prev)
        if (n->type == 0) return n;
    return NULL;
}

static V doc_get(Str *k, int *ok)
{
    struct page *p = J->page;
    *ok = 1;
    if (keyis(k, "body")) return node_value(p->body);
    if (keyis(k, "documentElement")) return node_value(p->root);
    if (keyis(k, "head")) { for (struct node *c = p->root->first; c; c = c->next) if (c->tag == TG_HEAD) return node_value(c); return vnull(); }
    if (keyis(k, "title")) return vcstr(p->title);
    if (keyis(k, "readyState")) return vcstr("complete");
    if (keyis(k, "location")) return vobj(location_obj);
    if (keyis(k, "URL") || keyis(k, "documentURI")) return vcstr(p->url);
    if (keyis(k, "cookie")) return vcstr("");
    if (keyis(k, "activeElement")) return node_value(J->focus ? J->focus : p->body);
    if (keyis(k, "defaultView")) return vobj(window_obj);
    if (keyis(k, "nodeType")) return vnum(9);
    *ok = 0;
    return undef();
}

int host_get(Obj *o, Str *k, V *out)
{
    if (o == J->global) {
        if (keyis(k, "document")) { *out = vobj(document_obj); return 1; }
        if (keyis(k, "window") || keyis(k, "self") || keyis(k, "top") || keyis(k, "parent")) { *out = vobj(window_obj); return 1; }
        if (keyis(k, "location")) { *out = vobj(location_obj); return 1; }
        if (keyis(k, "innerWidth")) { *out = vnum(J->page->layout_w ? J->page->layout_w : 800); return 1; }
        if (keyis(k, "innerHeight")) { *out = vnum(600); return 1; }
        if (keyis(k, "devicePixelRatio")) { *out = vnum(1); return 1; }
        if (keyis(k, "scrollX") || keyis(k, "scrollY") || keyis(k, "pageXOffset") || keyis(k, "pageYOffset")) { *out = vnum(0); return 1; }
        if (keyis(k, "localStorage") || keyis(k, "sessionStorage")) { *out = vobj(storage_obj); return 1; }
        return 0;
    }
    if (o == document_obj) {
        int ok;
        V v = doc_get(k, &ok);
        if (ok) { *out = v; return 1; }
    }
    if (o == location_obj) {
        if (keyis(k, "href") || keyis(k, "toString")) { if (keyis(k, "href")) { *out = vcstr(J->page->url); return 1; } }
        const char *u = J->page->url;
        const char *hs = strstr(u, "://");
        const char *host = hs ? hs + 3 : u;
        const char *path = strchr(host, '/');
        if (keyis(k, "protocol")) { *out = vstr(str_new(u, hs ? hs - u + 1 : 0)); return 1; }
        if (keyis(k, "host") || keyis(k, "hostname")) { *out = vstr(str_new(host, path ? path - host : (int)strlen(host))); return 1; }
        if (keyis(k, "pathname")) { const char *q = path ? strchr(path, '?') : NULL; *out = vstr(path ? str_new(path, q ? q - path : (int)strlen(path)) : intern_c("/")); return 1; }
        if (keyis(k, "search")) { const char *q = strchr(u, '?'); const char *h = q ? strchr(q, '#') : NULL; *out = vstr(q ? str_new(q, h ? h - q : (int)strlen(q)) : intern_c("")); return 1; }
        if (keyis(k, "hash")) { const char *h = strchr(u, '#'); *out = vcstr(h ? h : ""); return 1; }
        if (keyis(k, "origin")) { *out = vstr(str_new(u, path ? path - u : (int)strlen(u))); return 1; }
        return 0;
    }
    if (o->cls == O_STYLE) {
        char prop[64];
        kebab(k->c, prop, sizeof prop);
        const char *st = attr_val(o->host, "style");
        int vl;
        const char *v = st ? style_find(st, prop, &vl) : NULL;
        if (keyis(k, "cssText")) { *out = vcstr(st ? st : ""); return 1; }
        if (keyis(k, "setProperty") || keyis(k, "removeProperty") || keyis(k, "getPropertyValue")) return 0;
        *out = v ? vstr(str_new(v, vl)) : vcstr("");
        return 1;
    }
    if (o->cls == O_CLASSLIST) {
        if (keyis(k, "length")) {
            const char *c = attr_val(o->host, "class");
            int cnt = 0;
            for (const char *p = c; p && *p;) { while (*p == ' ') p++; if (!*p) break; cnt++; while (*p && *p != ' ') p++; }
            *out = vnum(cnt);
            return 1;
        }
        if (keyis(k, "value")) { const char *c = attr_val(o->host, "class"); *out = vcstr(c ? c : ""); return 1; }
        return 0;
    }
    if (o->cls == O_STORAGE) {
        if (keyis(k, "length")) { int c = 0; for (int i = 0; i < o->np; i++) if (o->p[i].v.t == VSTR) c++; *out = vnum(c); return 1; }
        return 0;
    }
    struct node *n = o->host;
    if (!n) return 0;
    if (keyis(k, "nodeType")) { *out = vnum(n->type == 0 ? 1 : 3); return 1; }
    if (keyis(k, "nodeName") || keyis(k, "tagName")) {
        char t[32];
        const char *s = dom_tag_name(n);
        int i = 0;
        for (; s[i] && i < 31; i++) t[i] = n->type == 0 && s[i] >= 'a' && s[i] <= 'z' ? s[i] - 32 : s[i];
        t[i] = 0;
        *out = vcstr(t);
        return 1;
    }
    if (keyis(k, "localName")) { *out = vcstr(dom_tag_name(n)); return 1; }
    if (keyis(k, "parentNode") || keyis(k, "parentElement")) { *out = node_value(n->parent); return 1; }
    if (keyis(k, "firstChild")) { *out = node_value(n->first); return 1; }
    if (keyis(k, "lastChild")) { *out = node_value(n->last); return 1; }
    if (keyis(k, "nextSibling")) { *out = node_value(n->next); return 1; }
    if (keyis(k, "previousSibling")) { *out = node_value(n->prev); return 1; }
    if (keyis(k, "ownerDocument")) { *out = vobj(document_obj); return 1; }
    if (keyis(k, "isConnected")) { struct node *x = n; while (x->parent) x = x->parent; *out = vbool(x == J->page->root); return 1; }
    if (keyis(k, "textContent") || keyis(k, "innerText") || keyis(k, "data") || keyis(k, "nodeValue")) {
        if (n->type == 1) { *out = vstr(str_new(n->text, n->tlen)); return 1; }
        if (keyis(k, "nodeValue")) { *out = vnull(); return 1; }
        *out = text_of(n);
        return 1;
    }
    if (n->type != 0) return 0;
    if (keyis(k, "id") || keyis(k, "title") || keyis(k, "name") || keyis(k, "placeholder") || keyis(k, "alt") ||
        keyis(k, "rel") || keyis(k, "target") || keyis(k, "lang") || keyis(k, "htmlFor") || keyis(k, "action") ||
        keyis(k, "method")) {
        const char *v = attr_val(n, keyis(k, "htmlFor") ? "for" : k->c);
        *out = vcstr(v ? v : "");
        return 1;
    }
    if (keyis(k, "className")) { const char *v = attr_val(n, "class"); *out = vcstr(v ? v : ""); return 1; }
    if (keyis(k, "type")) { const char *v = attr_val(n, "type"); *out = vcstr(v ? v : n->tag == TG_INPUT ? "text" : n->tag == TG_BUTTON ? "submit" : ""); return 1; }
    if (keyis(k, "href") || keyis(k, "src")) {
        const char *v = attr_val(n, k->c);
        if (!v) { *out = vcstr(""); return 1; }
        char full[512];
        url_resolve(J->page->url, v, full, sizeof full);
        *out = vcstr(full);
        return 1;
    }
    if (keyis(k, "value")) {
        if (n->tag == TG_TEXTAREA) { *out = text_of(n); return 1; }
        if (n->tag == TG_SELECT) {
            struct node *pick = NULL;
            for (struct node *c = n->first; c; c = c->next)
                if (c->type == 0 && c->tag == TG_OPTION && (!pick || attr_val(c, "selected"))) pick = c;
            if (!pick) { *out = vcstr(""); return 1; }
            const char *v = attr_val(pick, "value");
            *out = v ? vcstr(v) : text_of(pick);
            return 1;
        }
        const char *v = attr_val(n, "value");
        *out = vcstr(v ? v : (n->tag == TG_INPUT && (input_kind(n, "checkbox") || input_kind(n, "radio")) ? "on" : ""));
        return 1;
    }
    if (keyis(k, "checked") || keyis(k, "disabled") || keyis(k, "hidden") || keyis(k, "selected") || keyis(k, "required") ||
        keyis(k, "readOnly")) {
        *out = vbool(attr_val(n, keyis(k, "readOnly") ? "readonly" : k->c) != NULL);
        return 1;
    }
    if (keyis(k, "innerHTML") || keyis(k, "outerHTML")) {
        int cap;
        char *buf = big_buf(&cap);
        if (!buf) { *out = vcstr(""); return 1; }
        int len = dom_serialize(n, buf, cap, keyis(k, "outerHTML"));
        *out = vstr(str_new(buf, len < cap - 1 ? len : cap - 1));
        return 1;
    }
    if (keyis(k, "style")) { *out = style_proxy(n); return 1; }
    if (keyis(k, "classList")) { *out = classlist_proxy(n); return 1; }
    if (keyis(k, "children")) { *out = vobj(children_of(n, 1)); return 1; }
    if (keyis(k, "childNodes")) { *out = vobj(children_of(n, 0)); return 1; }
    if (keyis(k, "childElementCount")) { *out = vnum(children_of(n, 1)->alen); return 1; }
    if (keyis(k, "firstElementChild")) { struct node *c = n->first; while (c && c->type) c = c->next; *out = node_value(c); return 1; }
    if (keyis(k, "lastElementChild")) { struct node *c = n->last; while (c && c->type) c = c->prev; *out = node_value(c); return 1; }
    if (keyis(k, "nextElementSibling")) { *out = node_value(elem_sib(n, 1)); return 1; }
    if (keyis(k, "previousElementSibling")) { *out = node_value(elem_sib(n, 0)); return 1; }
    if (keyis(k, "offsetWidth") || keyis(k, "clientWidth") || keyis(k, "scrollWidth") || keyis(k, "offsetHeight") ||
        keyis(k, "clientHeight") || keyis(k, "scrollHeight") || keyis(k, "offsetLeft") || keyis(k, "offsetTop")) {
        int x, y, w, h;
        bbox(n, &x, &y, &w, &h);
        *out = vnum(strstr(k->c, "Width") ? w : strstr(k->c, "Height") ? h : strstr(k->c, "Left") ? x : y);
        return 1;
    }
    if (keyis(k, "scrollTop") || keyis(k, "scrollLeft")) { *out = vnum(0); return 1; }
    if (keyis(k, "dataset")) {
        Obj *d = obj_new(O_PLAIN, J->object_proto);
        for (int i = 0; i < n->nattr; i++)
            if (!strncmp(n->attr[i].name, "data-", 5)) {
                char cam[64];
                int o2 = 0;
                for (const char *s = n->attr[i].name + 5; *s && o2 < 62; s++) {
                    if (*s == '-' && s[1]) { cam[o2++] = s[1] >= 'a' && s[1] <= 'z' ? s[1] - 32 : s[1]; s++; }
                    else cam[o2++] = *s;
                }
                cam[o2] = 0;
                own_put(d, intern_c(cam), vcstr(n->attr[i].val));
            }
        *out = vobj(d);
        return 1;
    }
    if (keyis(k, "selectedIndex") && n->tag == TG_SELECT) {
        int i = 0, sel = 0;
        for (struct node *c = n->first; c; c = c->next) if (c->type == 0 && c->tag == TG_OPTION) { if (attr_val(c, "selected")) sel = i; i++; }
        *out = vnum(sel);
        return 1;
    }
    if (keyis(k, "options") && n->tag == TG_SELECT) {
        Obj *r = arr_new();
        collect(n, r, NULL, "option", NULL, 0, 0);
        *out = vobj(r);
        return 1;
    }
    if (keyis(k, "form")) { struct node *x = n->parent; while (x && !(x->type == 0 && x->tag == TG_FORM)) x = x->parent; *out = node_value(x); return 1; }
    return 0;
}

int host_put(Obj *o, Str *k, V v)
{
    if (o == document_obj) {
        if (keyis(k, "title")) { safestrcpy(J->page->title, cstr(v), sizeof J->page->title); J->want_paint = 1; return 1; }
        if (keyis(k, "cookie")) return 1;
        if (keyis(k, "location")) { safestrcpy(J->nav, cstr(v), sizeof J->nav); return 1; }
        return 0;
    }
    if (o == J->global && keyis(k, "location")) { safestrcpy(J->nav, cstr(v), sizeof J->nav); return 1; }
    if (o == location_obj) {
        if (keyis(k, "href")) { safestrcpy(J->nav, cstr(v), sizeof J->nav); return 1; }
        if (keyis(k, "hash")) return 1;
        return 0;
    }
    if (o->cls == O_STYLE) {
        if (keyis(k, "cssText")) { dom_set_attr(J->page, o->host, "style", cstr(v)); dirty(); return 1; }
        char prop[64];
        kebab(k->c, prop, sizeof prop);
        Str *val = v.t == VNUM ? to_str(v) : to_str(v);
        char buf[160];
        if (v.t == VNUM && v.n != 0 && strcmp(prop, "opacity") && strcmp(prop, "z-index") && strcmp(prop, "flex-grow") &&
            strcmp(prop, "font-weight") && strcmp(prop, "line-height")) {
            snprintf(buf, sizeof buf, "%spx", val->c);                 /* sloppy numeric lengths */
            style_set(o->host, prop, buf);
        } else {
            style_set(o->host, prop, v.t == VNULL ? "" : val->c);
        }
        return 1;
    }
    if (o->cls == O_STORAGE) {
        if (v.t == VOBJ && is_callable(v)) return 0;
        own_put(o, k, vstr(to_str(v)));
        extern void storage_save(void);
        storage_save();
        return 1;
    }
    if (o->cls == O_CLASSLIST) return 1;
    struct node *n = o->host;
    if (!n) return 0;
    struct page *p = J->page;
    if (keyis(k, "textContent") || keyis(k, "innerText") || keyis(k, "data") || keyis(k, "nodeValue")) {
        Str *s = to_str(v.t == VNULL ? vcstr("") : v);
        set_text(n, s->c, s->len);
        return 1;
    }
    if (n->type != 0) return 0;
    if (keyis(k, "innerHTML")) {
        Str *s = to_str(v);
        dom_set_html(p, n, s->c, s->len);
        dirty();
        return 1;
    }
    if (keyis(k, "outerHTML")) {
        Str *s = to_str(v);
        struct node *tmp = dom_new_element(p, "div");
        if (tmp && n->parent) {
            dom_set_html(p, tmp, s->c, s->len);
            while (tmp->first) dom_insert(p, n->parent, tmp->first, n);
            dom_remove(n);
            dirty();
        }
        return 1;
    }
    if (keyis(k, "id") || keyis(k, "title") || keyis(k, "name") || keyis(k, "placeholder") || keyis(k, "alt") ||
        keyis(k, "href") || keyis(k, "src") || keyis(k, "type") || keyis(k, "className") || keyis(k, "htmlFor") ||
        keyis(k, "target") || keyis(k, "rel")) {
        const char *an = keyis(k, "className") ? "class" : keyis(k, "htmlFor") ? "for" : k->c;
        dom_set_attr(p, n, an, cstr(v));
        dirty();
        return 1;
    }
    if (keyis(k, "value")) {
        Str *s = to_str(v);
        if (n->tag == TG_TEXTAREA) set_text(n, s->c, s->len);
        else if (n->tag == TG_SELECT) {
            for (struct node *c = n->first; c; c = c->next)
                if (c->type == 0 && c->tag == TG_OPTION) {
                    const char *ov = attr_val(c, "value");
                    char t[256];
                    if (!ov) { dom_text(c, t, sizeof t); ov = t; }
                    dom_set_attr(p, c, "selected", !strcmp(ov, s->c) ? "" : NULL);
                }
        } else dom_set_attr(p, n, "value", s->c);
        dirty();
        return 1;
    }
    if (keyis(k, "checked") || keyis(k, "disabled") || keyis(k, "hidden") || keyis(k, "selected") || keyis(k, "required") ||
        keyis(k, "readOnly")) {
        const char *an = keyis(k, "readOnly") ? "readonly" : k->c;
        if (keyis(k, "checked") && truthy(v) && input_kind(n, "radio")) {   /* only one radio of a name is on */
            const char *nm = attr_val(n, "name");
            if (nm) {
                Obj *all = arr_new();
                collect(p->root, all, NULL, "input", NULL, 0, 0);
                for (int i = 0; i < all->alen; i++) {
                    struct node *r = node_of(all->a[i]);
                    const char *rn = attr_val(r, "name");
                    if (r != n && rn && !strcmp(rn, nm) && input_kind(r, "radio")) dom_set_attr(p, r, "checked", NULL);
                }
            }
        }
        dom_set_attr(p, n, an, truthy(v) ? "" : NULL);
        dirty();
        return 1;
    }
    if (keyis(k, "scrollTop") || keyis(k, "scrollLeft")) return 1;
    return 0;
}

/* ---- style and classList methods ---- */

NATIVE(st_setprop) { if (self.t == VOBJ && self.o->cls == O_STYLE) style_set(self.o->host, cstr(A(0)), cstr(A(1))); return undef(); }
NATIVE(st_removeprop) { if (self.t == VOBJ && self.o->cls == O_STYLE) style_set(self.o->host, cstr(A(0)), NULL); return vcstr(""); }
NATIVE(st_getprop)
{
    if (!(self.t == VOBJ && self.o->cls == O_STYLE)) return vcstr("");
    const char *st = attr_val(self.o->host, "style");
    int vl;
    const char *v = st ? style_find(st, cstr(A(0)), &vl) : NULL;
    return v ? vstr(str_new(v, vl)) : vcstr("");
}

static int cl_has(const char *c, const char *w)
{
    int wl = strlen(w);
    for (const char *p = c; p && *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e - p == wl && !strncmp(p, w, wl)) return 1;
        p = e;
    }
    return 0;
}

static void cl_set(struct node *n, const char *add, const char *remove)
{
    const char *c = attr_val(n, "class");
    char out[1024];
    int o = 0;
    for (const char *p = c; p && *p;) {
        while (*p == ' ') p++;
        const char *e = p;
        while (*e && *e != ' ') e++;
        if (e > p && !(remove && (int)strlen(remove) == e - p && !strncmp(p, remove, e - p)) && o + (e - p) + 2 < (int)sizeof out) {
            if (o) out[o++] = ' ';
            memcpy(out + o, p, e - p);
            o += e - p;
        }
        p = e;
    }
    out[o] = 0;
    if (add && !cl_has(out, add) && o + (int)strlen(add) + 2 < (int)sizeof out) {
        if (o) out[o++] = ' ';
        strcpy(out + o, add);
        o += strlen(add);
    }
    dom_set_attr(J->page, n, "class", out);
    dirty();
}

#define CL struct node *me = self.t == VOBJ && self.o->cls == O_CLASSLIST ? self.o->host : NULL; if (!me) return undef()
NATIVE(cl_add) { CL; for (int i = 0; i < n; i++) cl_set(me, cstr(args[i]), NULL); return undef(); }
NATIVE(cl_remove) { CL; for (int i = 0; i < n; i++) cl_set(me, NULL, cstr(args[i])); return undef(); }
NATIVE(cl_contains) { CL; const char *c = attr_val(me, "class"); return vbool(c && cl_has(c, cstr(A(0)))); }
NATIVE(cl_toggle)
{
    CL;
    const char *w = cstr(A(0)), *c = attr_val(me, "class");
    int on = n > 1 ? truthy(A(1)) : !(c && cl_has(c, w));
    cl_set(me, on ? w : NULL, on ? NULL : w);
    return vbool(on);
}
NATIVE(cl_replace) { CL; cl_set(me, NULL, cstr(A(0))); cl_set(me, cstr(A(1)), NULL); return vbool(1); }

/* ---- document ---- */

NATIVE(d_create) { return node_value(dom_new_element(J->page, cstr(A(0)))); }
NATIVE(d_create_text) { Str *s = to_str(A(0)); return node_value(dom_new_text(J->page, s->c, s->len)); }
NATIVE(d_create_fragment) { struct node *f = dom_new_element(J->page, "#fragment"); return node_value(f); }
NATIVE(d_by_id) { return node_value(by_id(J->page->root, cstr(A(0)))); }
NATIVE(d_qs)
{
    const char *sel = cstr(A(0));
    Obj *r = arr_new();
    collect(J->page->root, r, sel, NULL, NULL, 1, 1);
    return r->alen ? r->a[0] : vnull();
}
NATIVE(d_qsa) { Obj *r = arr_new(); collect(J->page->root, r, cstr(A(0)), NULL, NULL, 0, 1); return vobj(r); }
NATIVE(d_by_tag) { V root = node_value(J->page->root); return n_by_tag(root, args, n); }
NATIVE(d_by_class) { Obj *r = arr_new(); collect(J->page->root, r, NULL, NULL, cstr(A(0)), 0, 1); return vobj(r); }
NATIVE(d_by_name)
{
    Obj *all = arr_new(), *r = arr_new();
    collect(J->page->root, all, "[name]", NULL, NULL, 0, 1);
    const char *want = cstr(A(0));
    for (int i = 0; i < all->alen; i++) {
        const char *nm = attr_val(node_of(all->a[i]), "name");
        if (nm && !strcmp(nm, want)) arr_push(r, all->a[i]);
    }
    return vobj(r);
}
NATIVE(d_write)
{
    /* document.write after load: append to <body> */
    Str *s = to_str(A(0));
    struct node *tmp = dom_new_element(J->page, "div");
    if (!tmp || !J->page->body) return undef();
    dom_set_html(J->page, tmp, s->c, s->len);
    while (tmp->first) dom_insert(J->page, J->page->body, tmp->first, NULL);
    dirty();
    return undef();
}

/* ---- window ---- */

NATIVE(w_alert)
{
    safestrcpy(J->alert_text, cstr(A(0)), sizeof J->alert_text);
    J->has_alert = 1;
    return undef();
}
NATIVE(w_confirm) { w_alert(self, args, n); return vbool(1); }
NATIVE(w_prompt) { w_alert(self, args, n); return n > 1 ? A(1) : vnull(); }

static V add_timer(V *args, int n, int interval)
{
    if (!is_callable(A(0))) {
        if (A(0).t == VSTR) {                                       /* setTimeout("code", ms) */
            Str *s = to_str(A(0));
            char *src = malloc(s->len + 64);
            if (!src) return vnum(0);
            int m = snprintf(src, s->len + 64, "window.__t = function () {\n%s\n};", s->c);
            js_run(J->page, src, m, "timer");
            free(src);
            V f = get(vobj(J->global), intern_c("__t"));
            args[0] = f;
            if (!is_callable(f)) return vnum(0);
        } else {
            return vnum(0);
        }
    }
    int slot = -1;
    for (int i = 0; i < J->ntimers; i++) if (!J->timers[i].alive) { slot = i; break; }
    if (slot < 0) {
        if (J->ntimers == 64) return vnum(0);
        slot = J->ntimers++;
    }
    int ms = to_num(A(1)) > 0 ? (int)to_num(A(1)) : 0;
    if (interval && ms < 10) ms = 10;
    struct jtimer *t = &J->timers[slot];
    t->id = J->next_id++;
    t->interval = interval ? ms : 0;
    t->due = J->now_ms + ms;
    t->fn = A(0);
    t->alive = 1;
    t->nargs = n > 2 ? n - 2 : 0;
    t->args = NULL;
    if (t->nargs) {
        t->args = jalloc(t->nargs * sizeof *t->args);
        if (t->args) memcpy(t->args, args + 2, t->nargs * sizeof *t->args);
        else t->nargs = 0;
    }
    return vnum(t->id);
}

NATIVE(w_set_timeout) { return add_timer(args, n, 0); }
NATIVE(w_set_interval) { return add_timer(args, n, 1); }
NATIVE(w_raf) { V a2[2] = {A(0), vnum(16)}; V r = add_timer(a2, 2, 0); return r; }
NATIVE(w_clear_timer)
{
    int id = (int)to_num(A(0));
    for (int i = 0; i < J->ntimers; i++)
        if (J->timers[i].alive && J->timers[i].id == id) J->timers[i].alive = 0;
    return undef();
}

NATIVE(w_computed) { struct node *nd = node_of(A(0)); return nd ? style_proxy(nd) : vnull(); }
NATIVE(w_scroll) { return undef(); }
NATIVE(loc_reload) { safestrcpy(J->nav, J->page->url, sizeof J->nav); return undef(); }
NATIVE(loc_assign) { safestrcpy(J->nav, cstr(A(0)), sizeof J->nav); return undef(); }
NATIVE(loc_tostring) { return vcstr(J->page->url); }

/* ---- localStorage: one JSON-ish file per site under /disk/etc/web ---- */

void storage_save(void)
{
    if (!storage_path[0]) return;
    int fd = open(storage_path, O_WRONLY | O_CREAT | O_TRUNC);
    if (fd < 0) return;
    for (int i = 0; i < storage_obj->np; i++) {
        V v = storage_obj->p[i].v;
        if (v.t != VSTR) continue;
        Str *k = storage_obj->p[i].k;
        /* key \t value \n, with \\, \t and \n escaped */
        for (int pass = 0; pass < 2; pass++) {
            const char *s = pass ? v.s->c : k->c;
            int len = pass ? v.s->len : k->len;
            for (int j = 0; j < len; j++) {
                char c = s[j];
                if (c == '\\') write(fd, "\\\\", 2);
                else if (c == '\t') write(fd, "\\t", 2);
                else if (c == '\n') write(fd, "\\n", 2);
                else write(fd, &c, 1);
            }
            write(fd, pass ? "\n" : "\t", 1);
        }
    }
    close(fd);
}

static void storage_load(void)
{
    struct diskinfo di;
    diskinfo(&di);
    storage_path[0] = 0;
    if (!di.mounted) return;
    const char *u = J->page->url;
    const char *hs = strstr(u, "://");
    const char *host = hs ? hs + 3 : u;
    const char *end = strchr(host, '/');
    int hl = end ? end - host : (int)strlen(host);
    u32 h = 2166136261u;
    for (int i = 0; i < hl; i++) h = (h ^ (u8)host[i]) * 16777619u;
    mkdir("/disk/etc");
    mkdir("/disk/etc/web");
    snprintf(storage_path, sizeof storage_path, "/disk/etc/web/%08x", h);
    int fd = open(storage_path, O_RDONLY);
    if (fd < 0) return;
    char *buf = malloc(256 * 1024);
    int len = buf ? read(fd, buf, 256 * 1024 - 1) : 0;
    close(fd);
    if (len <= 0) { free(buf); return; }
    buf[len] = 0;
    char *key = malloc(len + 1), *val = malloc(len + 1);
    int kl = 0, vl = 0, in_val = 0;
    for (int i = 0; key && val && i < len; i++) {
        char c = buf[i];
        if (c == '\\' && i + 1 < len) {
            c = buf[++i];
            c = c == 't' ? '\t' : c == 'n' ? '\n' : c;
            if (in_val) val[vl++] = c; else key[kl++] = c;
        } else if (c == '\t' && !in_val) {
            in_val = 1;
        } else if (c == '\n') {
            if (in_val) own_put(storage_obj, intern(key, kl), vstr(str_new(val, vl)));
            kl = vl = in_val = 0;
        } else if (in_val) val[vl++] = c;
        else key[kl++] = c;
    }
    free(key);
    free(val);
    free(buf);
}

NATIVE(s_get)
{
    struct prop *p = own_find(storage_obj, intern_c(cstr(A(0))));
    return p && p->v.t == VSTR ? p->v : vnull();
}
NATIVE(s_set) { own_put(storage_obj, intern_c(cstr(A(0))), vstr(to_str(A(1)))); storage_save(); return undef(); }
NATIVE(s_remove)
{
    struct prop *p = own_find(storage_obj, intern_c(cstr(A(0))));
    if (p) { p->v = undef(); storage_save(); }
    return undef();
}
NATIVE(s_clear)
{
    for (int i = 0; i < storage_obj->np; i++) if (storage_obj->p[i].v.t == VSTR) storage_obj->p[i].v = undef();
    storage_save();
    return undef();
}
NATIVE(s_key)
{
    int want = (int)to_num(A(0)), k = 0;
    for (int i = 0; i < storage_obj->np; i++)
        if (storage_obj->p[i].v.t == VSTR && k++ == want) return vstr(storage_obj->p[i].k);
    return vnull();
}

/* ---- setup ---- */

void dom_init_globals(Obj *g)
{
    S_on_cache = intern_c("__on");
    Obj *np = obj_new(O_PLAIN, J->object_proto);
    J->node_proto = np;
    static const struct { const char *n; native_fn f; } NODE_FNS[] = {
        {"getAttribute", n_getattr}, {"setAttribute", n_setattr}, {"removeAttribute", n_removeattr},
        {"hasAttribute", n_hasattr}, {"toggleAttribute", n_toggleattr}, {"appendChild", n_append_child},
        {"append", n_append}, {"prepend", n_prepend}, {"insertBefore", n_insert_before}, {"removeChild", n_remove_child},
        {"replaceChild", n_replace_child}, {"replaceWith", n_replace_with}, {"remove", n_remove}, {"cloneNode", n_clone},
        {"contains", n_contains}, {"querySelector", n_qs}, {"querySelectorAll", n_qsa},
        {"getElementsByTagName", n_by_tag}, {"getElementsByClassName", n_by_class}, {"getElementById", n_by_id},
        {"matches", n_matches}, {"closest", n_closest}, {"focus", n_focus}, {"blur", n_blur}, {"click", n_click},
        {"addEventListener", n_add_listener}, {"removeEventListener", n_remove_listener},
        {"insertAdjacentHTML", n_insert_html}, {"getBoundingClientRect", n_rect}, {"scrollIntoView", n_noop},
        {"select", n_noop}, {"setSelectionRange", n_noop}, {"normalize", n_noop}, {NULL, NULL}};
    for (int i = 0; NODE_FNS[i].n; i++) defn(np, NODE_FNS[i].n, NODE_FNS[i].f);
    own_put(np, intern_c("ELEMENT_NODE"), vnum(1));
    own_put(np, intern_c("TEXT_NODE"), vnum(3));
    /* constructors for instanceof checks */
    static const char *const ctors[] = {"Node", "Element", "HTMLElement", "HTMLInputElement", "HTMLDivElement", NULL};
    for (int i = 0; ctors[i]; i++) {
        Obj *c = native(ctors[i], n_noop, 0);
        own_put(c, intern_c("prototype"), vobj(np));
        own_put(g, intern_c(ctors[i]), vobj(c));
    }

    J->event_proto = obj_new(O_PLAIN, J->object_proto);
    defn(J->event_proto, "preventDefault", ev_prevent);
    defn(J->event_proto, "stopPropagation", ev_stop);
    defn(J->event_proto, "stopImmediatePropagation", ev_stop);

    J->style_proto = obj_new(O_PLAIN, J->object_proto);
    defn(J->style_proto, "setProperty", st_setprop);
    defn(J->style_proto, "removeProperty", st_removeprop);
    defn(J->style_proto, "getPropertyValue", st_getprop);

    J->classlist_proto = obj_new(O_PLAIN, J->object_proto);
    defn(J->classlist_proto, "add", cl_add);
    defn(J->classlist_proto, "remove", cl_remove);
    defn(J->classlist_proto, "contains", cl_contains);
    defn(J->classlist_proto, "toggle", cl_toggle);
    defn(J->classlist_proto, "replace", cl_replace);

    document_obj = obj_new(O_PLAIN, np);
    document_obj->host = J->page->root;
    document_obj->hosted = 1;
    J->page->root->js = document_obj;                              /* the root's object is the document */
    defn(document_obj, "createElement", d_create);
    defn(document_obj, "createElementNS", d_create);
    defn(document_obj, "createTextNode", d_create_text);
    defn(document_obj, "createDocumentFragment", d_create_fragment);
    defn(document_obj, "getElementById", d_by_id);
    defn(document_obj, "querySelector", d_qs);
    defn(document_obj, "querySelectorAll", d_qsa);
    defn(document_obj, "getElementsByTagName", d_by_tag);
    defn(document_obj, "getElementsByClassName", d_by_class);
    defn(document_obj, "getElementsByName", d_by_name);
    defn(document_obj, "write", d_write);
    defn(document_obj, "writeln", d_write);

    window_obj = g;
    defn(g, "alert", w_alert);
    defn(g, "confirm", w_confirm);
    defn(g, "prompt", w_prompt);
    defn(g, "setTimeout", w_set_timeout);
    defn(g, "setInterval", w_set_interval);
    defn(g, "clearTimeout", w_clear_timer);
    defn(g, "clearInterval", w_clear_timer);
    defn(g, "requestAnimationFrame", w_raf);
    defn(g, "cancelAnimationFrame", w_clear_timer);
    defn(g, "addEventListener", n_add_listener);
    defn(g, "removeEventListener", n_remove_listener);
    defn(g, "getComputedStyle", w_computed);
    defn(g, "scrollTo", w_scroll);
    defn(g, "scrollBy", w_scroll);
    defn(g, "focus", n_noop);

    location_obj = obj_new(O_PLAIN, J->object_proto);
    location_obj->hosted = 1;
    defn(location_obj, "reload", loc_reload);
    defn(location_obj, "assign", loc_assign);
    defn(location_obj, "replace", loc_assign);
    defn(location_obj, "toString", loc_tostring);

    Obj *nav = obj_new(O_PLAIN, J->object_proto);
    own_put(nav, intern_c("userAgent"), vcstr("Mozilla/5.0 (Petrichor) PetrichorBrowser/1.0"));
    own_put(nav, intern_c("language"), vcstr("en-US"));
    own_put(nav, intern_c("platform"), vcstr("Petrichor"));
    own_put(nav, intern_c("onLine"), vbool(1));
    own_put(g, intern_c("navigator"), vobj(nav));

    storage_obj = obj_new(O_STORAGE, J->object_proto);
    storage_obj->hosted = 1;
    defn(storage_obj, "getItem", s_get);
    defn(storage_obj, "setItem", s_set);
    defn(storage_obj, "removeItem", s_remove);
    defn(storage_obj, "clear", s_clear);
    defn(storage_obj, "key", s_key);
    storage_load();
}

/* ---- API used by the browser ---- */

void js_loaded(struct page *p)
{
    if (!js_enter(p)) return;
    js_event(p, NULL, "DOMContentLoaded", 0, 0);
    if (!js_enter(p)) return;
    js_event(p, NULL, "load", 0, 0);
}

int js_timers(struct page *p)
{
    if (!js_enter(p)) return -1;
    u32 now = J->now_ms;
    for (int round = 0; round < 4; round++) {
        int ran = 0;
        for (int i = 0; i < J->ntimers && !J->aborted; i++) {
            struct jtimer *t = &J->timers[i];
            if (!t->alive || (int)(t->due - now) > 0) continue;
            V fn = t->fn;
            V *a = t->args;
            int na = t->nargs;
            if (t->interval) t->due = now + t->interval;
            else t->alive = 0;
            J->steps = 0;
            js_invoke(fn, vobj(J->global), a, na, "timer");
            ran = 1;
        }
        if (!ran) break;
    }
    int next = -1;
    for (int i = 0; i < J->ntimers; i++) {
        struct jtimer *t = &J->timers[i];
        if (!t->alive) continue;
        int d = (int)(t->due - now);
        if (d < 0) d = 0;
        if (next < 0 || d < next) next = d;
    }
    return J->aborted ? -1 : next;
}

const char *js_take_nav(struct page *p)
{
    static char buf[512];
    if (!p->js || !p->js->nav[0]) return NULL;
    safestrcpy(buf, p->js->nav, sizeof buf);
    p->js->nav[0] = 0;
    return buf;
}

const char *js_take_alert(struct page *p)
{
    static char buf[200];
    if (!p->js || !p->js->has_alert) return NULL;
    p->js->has_alert = 0;
    safestrcpy(buf, p->js->alert_text, sizeof buf);
    return buf;
}

struct node *js_focus(struct page *p) { return p->js ? p->js->focus : NULL; }

void timers_reset(void) {}
