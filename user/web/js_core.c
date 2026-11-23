/* web/js_core.c - the JavaScript engine: values, strings, objects, the
   garbage collector, numbers and maths. Part of the browser.

   The engine (js_core, js_parse, js_eval, js_lib, js_dom) is a tree-walking
   interpreter for most of modern JavaScript: functions and closures, arrow
   functions, classes, let/const, destructuring, spread, template strings,
   exceptions, regular expressions, JSON, promises and timers, and the DOM.
   BigInt is in js_bigint.c.
   Not supported: generators, real async/await (await only unwraps promises
   that have already settled), modules, Proxy.

   Memory is collected by mark and sweep: between events from the roots
   alone, and while a script runs (when it allocates a lot) also from
   anything on the C stack that looks like a pointer into the heap. */


typedef struct Ast Ast;
typedef struct JsStr JsStr;
typedef struct JsObj JsObj;
typedef struct Env Env;

/* V_SYM: a symbol. It is its property key: an interned string starting
   "@@" ("@@iterator", "@@symbol:7:description", "@@symbol.for:name").
   V_BIG: a BigInt, kept in a string-kind block (see js_bigint.c). */
enum { V_UNDEF, V_NULL, V_BOOL, V_NUM, V_STR, V_OBJ, V_SYM, V_BIG };

typedef struct {
    unsigned char t;
    union {
        double n;
        int b;
        JsStr *s;
        JsObj *o;
    } u;
} Value;

static Value v_undef(void) {
    Value v;
    v.t = V_UNDEF;
    v.u.n = 0;
    return v;
}
static Value v_null(void) {
    Value v;
    v.t = V_NULL;
    v.u.n = 0;
    return v;
}
static Value v_bool(int b) {
    Value v;
    v.t = V_BOOL;
    v.u.n = 0;
    v.u.b = b != 0;
    return v;
}
static Value v_num(double n) {
    Value v;
    v.t = V_NUM;
    v.u.n = n;
    return v;
}
static Value v_str(JsStr *s) {
    Value v;
    v.t = V_STR;
    v.u.n = 0;
    v.u.s = s;
    return v;
}
static Value v_sym(JsStr *key) {
    Value v;
    v.t = V_SYM;
    v.u.n = 0;
    v.u.s = key;
    return v;
}
static Value v_obj(JsObj *o) {
    Value v;
    v.t = V_OBJ;
    v.u.n = 0;
    v.u.o = o;
    return v;
}

/* ---------------------------------------------------------------- the heap */

typedef struct Gc {
    struct Gc *next;
    unsigned char kind, mark, perm;
    unsigned char builtin; /* (made before the page's scripts: for BROWSER_MISSING) */
    unsigned size; /* bytes, for recognising pointers into it */
} Gc;

enum { K_STR = 1, K_OBJ, K_ENV };

static Gc *heap_list;
static size_t heap_since_gc, heap_live;
static int js_oom; /* out of memory: scripts are stopped */
static void js_oom_where(int line) {
    if (!js_oom && getenv("BROWSER_TRACE")) printf("[oom] at js_core.c line %d\n", line);
    js_oom = 1;
}
static unsigned char oom_scratch[65536];

/* Collecting while scripts run (in js_dom.c): after this much allocation. */
static void js_gc_during(void);
#define GC_TRIGGER_BYTES (24u << 20)

/* Memory outside the heap that may hold the only reference to heap values
   (big argument lists): the collector scans these ranges too. */
static char *gc_extra_lo[64], *gc_extra_hi[64];
static int gc_extra_n;

static void gc_extra_add(void *p, size_t bytes) {
    if (gc_extra_n < 64) {
        gc_extra_lo[gc_extra_n] = p;
        gc_extra_hi[gc_extra_n] = (char *)p + bytes;
        gc_extra_n++;
    }
}

static void gc_extra_remove(void *p) {
    for (int i = gc_extra_n - 1; i >= 0; i--) {
        if (gc_extra_lo[i] == p) {
            gc_extra_lo[i] = gc_extra_lo[gc_extra_n - 1];
            gc_extra_hi[i] = gc_extra_hi[gc_extra_n - 1];
            gc_extra_n--;
            return;
        }
    }
}

static void free_args(Value *args, Value *small) {
    if (args != small) {
        gc_extra_remove(args);
        free(args);
    }
}


static void *js_alloc(size_t size, int kind) {
    if (heap_since_gc > GC_TRIGGER_BYTES) js_gc_during();
    Gc *g = js_oom ? NULL : malloc(size);
    if (!g) {
        js_oom_where(__LINE__);
        if (size > sizeof(oom_scratch)) size = sizeof(oom_scratch);
        memset(oom_scratch, 0, sizeof(oom_scratch));
        g = (Gc *)oom_scratch;
        g->kind = (unsigned char)kind;
        g->perm = 2; /* not on the heap list */
        return g;
    }
    memset(g, 0, size);
    g->kind = (unsigned char)kind;
    g->size = (unsigned)size;
    g->next = heap_list;
    heap_list = g;
    heap_since_gc += size;
    return g;
}

/* ---------------------------------------------------------------- strings */

struct JsStr {
    Gc gc;
    int len;
    unsigned hash;
    int index; /* the array index it spells, or -1 (interned strings only) */
    char s[1];
};

static unsigned str_hash(const char *s, int len) {
    unsigned h = 2166136261u;
    for (int i = 0; i < len; i++) h = (h ^ (unsigned char)s[i]) * 16777619u;
    return h ? h : 1;
}

static JsStr *str_new(const char *s, int len) {
    if (len < 0) len = 0;
    JsStr *r = js_alloc(sizeof(JsStr) + (size_t)len, K_STR);
    if (js_oom && (Gc *)r == (Gc *)oom_scratch) {
        r->len = 0;
        return r;
    }
    r->len = len;
    if (len && s) memcpy(r->s, s, (size_t)len); /* (no source: the caller fills it in) */
    r->s[len] = 0;
    r->index = -2;
    return r;
}

static JsStr *str_c(const char *s) { return str_new(s, (int)strlen(s)); }

/* Interned strings: one copy of each, never collected; property names are
   always interned, so they can be compared by pointer. */
static JsStr **intern_table;
static int intern_count, intern_cap;

static int canonical_index(const char *s, int len) {
    if (len == 0 || len > 9) return -1;
    if (s[0] == '0' && len > 1) return -1;
    int v = 0;
    for (int i = 0; i < len; i++) {
        if (s[i] < '0' || s[i] > '9') return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

static JsStr *intern(const char *s, int len) {
    if (intern_count * 2 >= intern_cap) {
        int cap = intern_cap ? intern_cap * 2 : 4096;
        JsStr **t = malloc(sizeof(JsStr *) * (size_t)cap);
        if (!t) {
            js_oom_where(__LINE__);
            return str_new(s, len);
        }
        memset(t, 0, sizeof(JsStr *) * (size_t)cap);
        for (int i = 0; i < intern_cap; i++) {
            JsStr *e = intern_table[i];
            if (!e) continue;
            unsigned at = e->hash & (unsigned)(cap - 1);
            while (t[at]) at = (at + 1) & (unsigned)(cap - 1);
            t[at] = e;
        }
        free(intern_table);
        intern_table = t;
        intern_cap = cap;
    }
    unsigned h = str_hash(s, len);
    unsigned at = h & (unsigned)(intern_cap - 1);
    while (intern_table[at]) {
        JsStr *e = intern_table[at];
        if (e->hash == h && e->len == len && memcmp(e->s, s, (size_t)len) == 0) return e;
        at = (at + 1) & (unsigned)(intern_cap - 1);
    }
    JsStr *r = malloc(sizeof(JsStr) + (size_t)len);
    if (!r) {
        js_oom_where(__LINE__);
        return str_new(s, len);
    }
    memset(r, 0, sizeof(JsStr));
    r->gc.kind = K_STR;
    r->gc.perm = 1;
    r->len = len;
    memcpy(r->s, s, (size_t)len);
    r->s[len] = 0;
    r->hash = h;
    r->index = canonical_index(s, len);
    intern_table[at] = r;
    intern_count++;
    return r;
}

static JsStr *intern_c(const char *s) { return intern(s, (int)strlen(s)); }
static JsStr *interned(JsStr *s) { return s->gc.perm == 1 ? s : intern(s->s, s->len); }

static JsStr *str_concat(JsStr *a, JsStr *b) {
    JsStr *r = str_new(NULL, a->len + b->len);
    if (r->len != a->len + b->len) return r;
    memcpy(r->s, a->s, (size_t)a->len);
    memcpy(r->s + a->len, b->s, (size_t)b->len);
    r->s[r->len] = 0;
    return r;
}

static int str_eq(const JsStr *a, const JsStr *b) {
    return a == b || (a->len == b->len && memcmp(a->s, b->s, (size_t)a->len) == 0);
}

static int str_cmp(const JsStr *a, const JsStr *b) {
    int n = a->len < b->len ? a->len : b->len;
    int c = memcmp(a->s, b->s, (size_t)n);
    if (c) return c;
    return a->len - b->len;
}

/* A growable byte buffer, for building strings. */
typedef struct {
    char *p;
    int len, cap;
} Buf;

static void buf_add(Buf *b, const char *s, int n) {
    if (n <= 0) return;
    if (b->len + n + 1 > b->cap) {
        int cap = b->cap ? b->cap * 2 : 64;
        while (cap < b->len + n + 1) cap *= 2;
        char *p = realloc(b->p, (size_t)cap);
        if (!p) {
            js_oom_where(__LINE__);
            return;
        }
        b->p = p;
        b->cap = cap;
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
    b->p[b->len] = 0;
}

static void buf_c(Buf *b, const char *s) { buf_add(b, s, (int)strlen(s)); }
static void buf_ch(Buf *b, char c) { buf_add(b, &c, 1); }

static JsStr *buf_done(Buf *b) {
    JsStr *s = str_new(b->p ? b->p : "", b->len);
    free(b->p);
    b->p = NULL;
    b->len = b->cap = 0;
    return s;
}

/* ---------------------------------------------------------------- objects */

enum {
    CL_OBJECT, CL_ARRAY, CL_FUNCTION, CL_ERROR, CL_BOOLEAN, CL_NUMBER, CL_STRING, CL_DATE, CL_REGEXP,
    CL_ARGUMENTS, CL_MAP, CL_SET, CL_PROMISE, CL_BUFFER, CL_NODE, CL_STYLE, CL_CLASSLIST, CL_DATASET, CL_EVENT, CL_STORAGE
};

enum { P_HIDDEN = 1, P_ACCESSOR = 2, P_READONLY = 4 };

typedef struct {
    JsStr *key;
    Value v;
    JsObj *get, *set;
    unsigned char flags;
} Prop;

/* A built-in function: returns 0, or 1 having thrown (see js_throw). */
typedef int (*Native)(JsObj *self, Value this_v, Value *args, int argc, Value *ret);

enum { FN_NONE, FN_NORMAL, FN_ARROW, FN_METHOD, FN_CLASS, FN_DERIVED };

struct JsObj {
    Gc gc;
    unsigned char cls, fn_kind, frozen, strict;
    JsObj *proto;
    Prop *props;
    int nprops, pcap;
    int *hidx; /* hash index into props, when there are many */
    int hcap;
    Value *arr; /* array elements */
    int alen, acap;
    Ast *fn;         /* a function written in JavaScript */
    Env *scope;      /* ... and the scope it closes over */
    Native native;   /* or a built-in one */
    int magic;       /* for built-ins sharing one Native */
    Value this_val;  /* arrows: their this; bound functions: the bound this; boxed primitives, dates */
    JsObj *target;   /* bound functions: the target; promises: reactions */
    JsObj *home;     /* methods: the object they belong to (for super) */
    Value *bound;    /* bound functions: arguments */
    int nbound;
    struct Node *node; /* DOM wrappers */
    void *extra;     /* regular expressions: the program; maps and sets: entries */
    int extra_len, extra_cap;
};

static JsObj *obj_new_raw(int cls, JsObj *proto) {
    JsObj *o = js_alloc(sizeof(JsObj), K_OBJ);
    o->cls = (unsigned char)cls;
    o->proto = proto;
    return o;
}

static int prop_slot(const JsObj *o, const JsStr *key) {
    if (o->hidx) {
        unsigned at = key->hash & (unsigned)(o->hcap - 1);
        while (o->hidx[at]) {
            int i = o->hidx[at] - 1;
            if (o->props[i].key == key) return i;
            at = (at + 1) & (unsigned)(o->hcap - 1);
        }
        return -1;
    }
    for (int i = 0; i < o->nprops; i++) {
        if (o->props[i].key == key) return i;
    }
    return -1;
}

static void rebuild_index(JsObj *o) {
    free(o->hidx);
    o->hidx = NULL;
    if (o->nprops <= 8) return;
    int cap = 16;
    while (cap < o->nprops * 2) cap *= 2;
    o->hidx = malloc(sizeof(int) * (size_t)cap);
    if (!o->hidx) return;
    memset(o->hidx, 0, sizeof(int) * (size_t)cap);
    o->hcap = cap;
    for (int i = 0; i < o->nprops; i++) {
        unsigned at = o->props[i].key->hash & (unsigned)(cap - 1);
        while (o->hidx[at]) at = (at + 1) & (unsigned)(cap - 1);
        o->hidx[at] = i + 1;
    }
}

static Prop *own_prop(JsObj *o, JsStr *key) {
    int i = prop_slot(o, key);
    return i < 0 ? NULL : &o->props[i];
}

/* Add or find an own property (key interned). */
static Prop *prop_add(JsObj *o, JsStr *key) {
    int i = prop_slot(o, key);
    if (i >= 0) return &o->props[i];
    if (o->nprops == o->pcap) {
        int cap = o->pcap ? o->pcap * 2 : 4;
        Prop *p = realloc(o->props, sizeof(Prop) * (size_t)cap);
        if (!p) {
            if (!js_oom && getenv("BROWSER_TRACE"))
                printf("[oom] object of class %d with %d properties, adding '%s'\n", o->cls, o->nprops, key->s);
            js_oom_where(__LINE__);
            static Prop dummy;
            return &dummy;
        }
        o->props = p;
        o->pcap = cap;
    }
    Prop *p = &o->props[o->nprops++];
    memset(p, 0, sizeof(*p));
    p->key = key;
    p->v = v_undef();
    if (o->hidx && o->nprops * 2 <= o->hcap) {
        unsigned at = key->hash & (unsigned)(o->hcap - 1);
        while (o->hidx[at]) at = (at + 1) & (unsigned)(o->hcap - 1);
        o->hidx[at] = o->nprops;
    } else if (o->nprops > 8) {
        rebuild_index(o);
    }
    return p;
}

static void prop_set_raw(JsObj *o, JsStr *key, Value v, int flags) {
    Prop *p = prop_add(o, key);
    p->v = v;
    p->flags = (unsigned char)flags;
    p->get = p->set = NULL;
}

static int prop_delete(JsObj *o, JsStr *key) {
    int i = prop_slot(o, key);
    if (i < 0) return 0;
    memmove(&o->props[i], &o->props[i + 1], sizeof(Prop) * (size_t)(o->nprops - i - 1));
    o->nprops--;
    rebuild_index(o);
    return 1;
}

static void arr_reserve(JsObj *a, int n) {
    if (n <= a->acap) return;
    int cap = a->acap ? a->acap * 2 : 8;
    while (cap < n) cap *= 2;
    Value *p = realloc(a->arr, sizeof(Value) * (size_t)cap);
    if (!p) {
        js_oom_where(__LINE__);
        return;
    }
    a->arr = p;
    a->acap = cap;
}

/* Elements this far past an array's end are kept as plain properties
   instead (a sparse array): a[1e9] = x must not allocate a billion slots. */
#define SPARSE_GAP (1 << 20)

static void arr_set_len(JsObj *a, int n) {
    if (n < 0) n = 0;
    if (n > a->alen + SPARSE_GAP) return; /* a huge, empty length: not representable here, ignored */
    arr_reserve(a, n);
    if (js_oom) return;
    for (int i = a->alen; i < n; i++) a->arr[i] = v_undef();
    a->alen = n;
}

static void arr_push(JsObj *a, Value v) {
    arr_reserve(a, a->alen + 1);
    if (js_oom) return;
    a->arr[a->alen++] = v;
}

/* ---------------------------------------------------------------- environments */

struct Env {
    Gc gc;
    Env *parent;
    JsObj *object; /* the global scope: variables are its properties */
    JsStr **names;
    Value *vals;
    unsigned char *consts;
    int n, cap;
};

static Env *env_new(Env *parent) {
    Env *e = js_alloc(sizeof(Env), K_ENV);
    e->parent = parent;
    return e;
}

static Value *env_slot(Env *e, JsStr *name) {
    for (int i = 0; i < e->n; i++) {
        if (e->names[i] == name) return &e->vals[i];
    }
    return NULL;
}

static Value *env_define(Env *e, JsStr *name, Value v, int is_const) {
    if (e->object) {
        Prop *p = prop_add(e->object, name);
        if (!(p->flags & P_ACCESSOR)) p->v = v;
        return &p->v;
    }
    Value *slot = env_slot(e, name);
    if (slot) {
        *slot = v;
        return slot;
    }
    if (e->n == e->cap) {
        int cap = e->cap ? e->cap * 2 : 4;
        JsStr **names = realloc(e->names, sizeof(JsStr *) * (size_t)cap);
        Value *vals = names ? realloc(e->vals, sizeof(Value) * (size_t)cap) : NULL;
        unsigned char *consts = vals ? realloc(e->consts, (size_t)cap) : NULL;
        if (names) e->names = names;
        if (vals) e->vals = vals;
        if (!names || !vals || !consts) {
            js_oom_where(__LINE__);
            static Value dummy;
            return &dummy;
        }
        e->consts = consts;
        e->cap = cap;
    }
    e->names[e->n] = name;
    e->vals[e->n] = v;
    e->consts[e->n] = (unsigned char)is_const;
    return &e->vals[e->n++];
}

/* ---------------------------------------------------------------- numbers */

static double js_nan(void) {
    union {
        u64 i;
        double d;
    } u;
    u.i = 0x7FF8000000000000ull;
    return u.d;
}
static double js_inf(void) {
    union {
        u64 i;
        double d;
    } u;
    u.i = 0x7FF0000000000000ull;
    return u.d;
}
static int is_nan(double d) { return d != d; }
static int is_finite(double d) { return !is_nan(d) && d != js_inf() && d != -js_inf(); }

static double js_sqrt(double x) {
    double r;
    __asm__("sqrtsd %1, %0" : "=x"(r) : "x"(x));
    return r;
}

static double js_trunc(double x) {
    if (!is_finite(x) || x >= 4503599627370496.0 || x <= -4503599627370496.0) return x;
    double t = (double)(long long)x;
    return t == 0 && x < 0 ? -0.0 : t;
}
static double js_floor(double x) {
    double t = js_trunc(x);
    return t > x ? t - 1 : t;
}
static double js_ceil(double x) {
    double t = js_trunc(x);
    return t < x ? t + 1 : t;
}
static double js_fabs(double x) { return x < 0 ? -x : x; }

static double js_fmod(double x, double y) {
    if (y == 0 || !is_finite(x) || is_nan(y)) return js_nan();
    if (!is_finite(y)) return x;
    double ay = js_fabs(y), ax = js_fabs(x);
    if (ax < ay) return x;
    /* Subtract shifted multiples of y, so large quotients stay exact. */
    double r = ax;
    while (r >= ay) {
        double m = ay;
        while (m * 2 <= r) m *= 2;
        r -= m;
    }
    return x < 0 ? -r : r;
}

static const double POW10[23] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                 1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};

static double pow10_int(int e) {
    double r = 1;
    int neg = e < 0;
    if (neg) e = -e;
    while (e > 22) {
        r *= 1e22;
        e -= 22;
        if (r > 1e300) break;
    }
    r *= POW10[e];
    return neg ? 1 / r : r;
}

static double js_exp(double x) {
    if (is_nan(x)) return x;
    if (x > 709.8) return js_inf();
    if (x < -745) return 0;
    /* e^x = 2^k * e^r, |r| <= ln2/2 */
    const double LN2 = 0.6931471805599453;
    double kf = js_floor(x / LN2 + 0.5);
    double r = x - kf * LN2;
    double term = 1, sum = 1;
    for (int i = 1; i < 25; i++) {
        term *= r / i;
        sum += term;
    }
    int k = (int)kf;
    while (k > 0) {
        int s = k > 60 ? 60 : k;
        sum *= (double)(1ull << s);
        k -= s;
    }
    while (k < 0) {
        int s = -k > 60 ? 60 : -k;
        sum /= (double)(1ull << s);
        k += s;
    }
    return sum;
}

static double js_log(double x) {
    if (is_nan(x) || x < 0) return js_nan();
    if (x == 0) return -js_inf();
    if (!is_finite(x)) return x;
    /* x = m * 2^k with m in [sqrt(1/2), sqrt(2)) */
    union {
        double d;
        u64 i;
    } u;
    u.d = x;
    int k = (int)((u.i >> 52) & 0x7FF) - 1023;
    if (k == -1023) { /* subnormal */
        u.d = x * 4503599627370496.0;
        k = (int)((u.i >> 52) & 0x7FF) - 1023 - 52;
    }
    u.i = (u.i & 0x000FFFFFFFFFFFFFull) | 0x3FF0000000000000ull;
    double m = u.d;
    if (m > 1.4142135623730951) {
        m /= 2;
        k++;
    }
    double s = (m - 1) / (m + 1), s2 = s * s, term = s, sum = 0;
    for (int i = 1; i < 60; i += 2) {
        sum += term / i;
        term *= s2;
    }
    return 2 * sum + k * 0.6931471805599453;
}

static double js_pow(double x, double y) {
    if (y == 0) return 1;
    if (is_nan(x) || is_nan(y)) return js_nan();
    if (y == js_trunc(y) && js_fabs(y) < 4294967296.0) {
        long long n = (long long)y;
        int neg = n < 0;
        if (neg) n = -n;
        double r = 1, b = x;
        while (n) {
            if (n & 1) r *= b;
            b *= b;
            n >>= 1;
        }
        return neg ? 1 / r : r;
    }
    if (x < 0) return js_nan();
    if (x == 0) return y > 0 ? 0 : js_inf();
    return js_exp(y * js_log(x));
}

static const double PI = 3.141592653589793;

static double js_sin(double x) {
    if (!is_finite(x)) return js_nan();
    x = js_fmod(x, 2 * PI);
    if (x > PI) x -= 2 * PI;
    if (x < -PI) x += 2 * PI;
    if (x > PI / 2) x = PI - x;
    if (x < -PI / 2) x = -PI - x;
    double term = x, sum = x, x2 = x * x;
    for (int i = 1; i < 12; i++) {
        term *= -x2 / ((2 * i) * (2 * i + 1));
        sum += term;
    }
    return sum;
}
static double js_cos(double x) { return js_sin(x + PI / 2); }
static double js_tan(double x) { return js_sin(x) / js_cos(x); }

static double js_atan(double x) {
    if (is_nan(x)) return x;
    int neg = x < 0;
    if (neg) x = -x;
    int inv = x > 1;
    if (inv) x = 1 / x;
    /* atan(x) = 2 atan(x / (1 + sqrt(1 + x^2))): twice, to get |x| small */
    int halvings = 0;
    while (x > 0.2 && halvings < 4) {
        x = x / (1 + js_sqrt(1 + x * x));
        halvings++;
    }
    double term = x, sum = x, x2 = x * x;
    for (int i = 1; i < 30; i++) {
        term *= -x2;
        sum += term / (2 * i + 1);
    }
    sum *= (double)(1 << halvings);
    if (inv) sum = PI / 2 - sum;
    return neg ? -sum : sum;
}

static double js_atan2(double y, double x) {
    if (is_nan(x) || is_nan(y)) return js_nan();
    if (x > 0) return js_atan(y / x);
    if (x < 0) return y >= 0 ? js_atan(y / x) + PI : js_atan(y / x) - PI;
    return y > 0 ? PI / 2 : y < 0 ? -PI / 2 : 0;
}

static double js_asin(double x) {
    if (x < -1 || x > 1) return js_nan();
    return js_atan2(x, js_sqrt(1 - x * x));
}
static double js_acos(double x) {
    if (x < -1 || x > 1) return js_nan();
    return js_atan2(js_sqrt(1 - x * x), x);
}

/* Text to number, as Number("...") does: whitespace allowed around, "" is
   0, anything else not a number is NaN. */
static double parse_number(const char *s, int len, int *used_out, int allow_trailing) {
    int i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
    int start = i, neg = 0;
    double result;
    if (i < len && (s[i] == '+' || s[i] == '-')) neg = s[i++] == '-';
    if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X' || s[i + 1] == 'b' || s[i + 1] == 'B' ||
                                       s[i + 1] == 'o' || s[i + 1] == 'O')) {
        int base = s[i + 1] == 'x' || s[i + 1] == 'X' ? 16 : s[i + 1] == 'b' || s[i + 1] == 'B' ? 2 : 8;
        i += 2;
        double v = 0;
        int digits = 0;
        while (i < len) {
            int d = s[i] >= '0' && s[i] <= '9' ? s[i] - '0' : s[i] >= 'a' && s[i] <= 'f' ? s[i] - 'a' + 10 : s[i] >= 'A' && s[i] <= 'F' ? s[i] - 'A' + 10 : 99;
            if (d >= base) break;
            v = v * base + d;
            i++;
            digits++;
        }
        result = digits ? (neg ? -v : v) : js_nan();
    } else if (i + 8 <= len && memcmp(s + i, "Infinity", 8) == 0) {
        i += 8;
        result = neg ? -js_inf() : js_inf();
    } else {
        u64 mant = 0;
        int exp10 = 0, digits = 0, any = 0;
        while (i < len && s[i] >= '0' && s[i] <= '9') {
            if (digits < 19) {
                mant = mant * 10 + (u64)(s[i] - '0');
                if (mant) digits++;
            } else {
                exp10++;
            }
            i++;
            any = 1;
        }
        if (i < len && s[i] == '.') {
            i++;
            while (i < len && s[i] >= '0' && s[i] <= '9') {
                if (digits < 19) {
                    mant = mant * 10 + (u64)(s[i] - '0');
                    if (mant) digits++;
                    exp10--;
                }
                i++;
                any = 1;
            }
        }
        if (!any) {
            result = js_nan();
            i = start;
        } else {
            if (i < len && (s[i] == 'e' || s[i] == 'E')) {
                int j = i + 1, eneg = 0, e = 0, edigits = 0;
                if (j < len && (s[j] == '+' || s[j] == '-')) eneg = s[j++] == '-';
                while (j < len && s[j] >= '0' && s[j] <= '9') {
                    if (e < 10000) e = e * 10 + (s[j] - '0');
                    j++;
                    edigits++;
                }
                if (edigits) {
                    exp10 += eneg ? -e : e;
                    i = j;
                }
            }
            double v = (double)mant;
            if (exp10 >= 0 && exp10 <= 22) v *= POW10[exp10];
            else if (exp10 < 0 && exp10 >= -22) v /= POW10[-exp10];
            else if (mant) v = exp10 > 0 ? v * pow10_int(exp10) : v / pow10_int(-exp10);
            result = neg ? -v : v;
        }
    }
    if (used_out) *used_out = i;
    if (!allow_trailing) {
        while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) i++;
        if (i < len) return js_nan();
        if (start == len) return 0; /* "" and "   " are 0 */
    }
    return result;
}

/* The digits of a positive finite number, shortest that read back the
   same (up to 17): digits in out, returns the decimal exponent n such
   that the value is 0.DIGITS x 10^n. */
static int shortest_digits(double x, char *out, int *count) {
    int e = (int)js_floor(js_log(x) / 2.302585092994046);
    for (int precision = 1; precision <= 17; precision++) {
        /* x / 10^(e - precision + 1), rounded */
        int shift = e - precision + 1;
        double scaled = shift >= 0 ? x / pow10_int(shift) : x * pow10_int(-shift);
        u64 m = (u64)(scaled + 0.5);
        char digits[24];
        int nd = 0;
        u64 t = m;
        do digits[nd++] = (char)('0' + t % 10);
        while (t /= 10);
        int exp_n = shift + nd; /* value = 0.digits * 10^exp_n */
        char text[48];
        int k = 0;
        for (int i = nd - 1; i >= 0; i--) text[k++] = digits[i];
        text[k++] = 'e';
        int ev = exp_n - nd;
        if (ev < 0) {
            text[k++] = '-';
            ev = -ev;
        }
        char ed[8];
        int ne = 0;
        do ed[ne++] = (char)('0' + ev % 10);
        while (ev /= 10);
        while (ne) text[k++] = ed[--ne];
        if (parse_number(text, k, NULL, 0) == x || precision == 17) {
            int j = 0;
            for (int i = nd - 1; i >= 0; i--) out[j++] = digits[i];
            while (j > 1 && out[j - 1] == '0') j--;
            *count = j;
            return exp_n;
        }
    }
    return 0;
}

static void number_to_text(double x, char *out) {
    if (is_nan(x)) {
        strcpy(out, "NaN");
        return;
    }
    if (x == 0) {
        strcpy(out, "0");
        return;
    }
    if (!is_finite(x)) {
        strcpy(out, x > 0 ? "Infinity" : "-Infinity");
        return;
    }
    int k = 0;
    if (x < 0) {
        out[k++] = '-';
        x = -x;
    }
    if (x < 9007199254740992.0 && x == js_trunc(x)) { /* an integer: exact */
        u64 v = (u64)x;
        char d[24];
        int n = 0;
        do d[n++] = (char)('0' + v % 10);
        while (v /= 10);
        while (n) out[k++] = d[--n];
        out[k] = 0;
        return;
    }
    char digits[24];
    int nd;
    int n = shortest_digits(x, digits, &nd);
    if (nd <= n && n <= 21) {
        for (int i = 0; i < nd; i++) out[k++] = digits[i];
        for (int i = nd; i < n; i++) out[k++] = '0';
    } else if (0 < n && n <= 21) {
        for (int i = 0; i < nd; i++) {
            if (i == n) out[k++] = '.';
            out[k++] = digits[i];
        }
    } else if (-6 < n && n <= 0) {
        out[k++] = '0';
        out[k++] = '.';
        for (int i = 0; i < -n; i++) out[k++] = '0';
        for (int i = 0; i < nd; i++) out[k++] = digits[i];
    } else {
        out[k++] = digits[0];
        if (nd > 1) {
            out[k++] = '.';
            for (int i = 1; i < nd; i++) out[k++] = digits[i];
        }
        out[k++] = 'e';
        int e = n - 1;
        out[k++] = e < 0 ? '-' : '+';
        if (e < 0) e = -e;
        char d[8];
        int ne = 0;
        do d[ne++] = (char)('0' + e % 10);
        while (e /= 10);
        while (ne) out[k++] = d[--ne];
    }
    out[k] = 0;
}

static int to_int32(double d) {
    if (!is_finite(d)) return 0;
    d = js_trunc(d);
    double m = js_fmod(d, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    unsigned u = (unsigned)(u64)m;
    return (int)u;
}
static unsigned to_uint32(double d) { return (unsigned)to_int32(d); }

/* ---------------------------------------------------------------- random */

static u64 js_random_state;
static double js_random(void) {
    if (!js_random_state) getrandom(&js_random_state, sizeof(js_random_state));
    js_random_state ^= js_random_state << 13;
    js_random_state ^= js_random_state >> 7;
    js_random_state ^= js_random_state << 17;
    return (double)(js_random_state >> 11) / 9007199254740992.0;
}
