/* web/js_eval.c - running JavaScript: evaluating the syntax tree.
   Part of the browser. */

/* How a statement finished. Expressions return C_NORMAL or C_THROW. */
enum { C_NORMAL, C_RETURN, C_BREAK, C_CONTINUE, C_THROW };

static Value js_exception;      /* what was thrown */
static int js_abort;            /* stop everything (time limit, out of memory): not catchable */
static long js_steps, js_step_limit = 20000000;
/* The functions running, innermost last (for messages about a script
   that ran too long: where it was). */
#define JS_FRAMES 64
static Ast *js_frames[JS_FRAMES];
static int js_frame_depth;

static int js_exec_line, js_call_pos; /* the line of the statement running, where the last call was (for messages) */
static u64 js_deadline;          /* uptime (ms) when the running script must stop; 0: none */

/* A run of script code (a script, an event handler, a timer, a promise
   job), allowed `ms` milliseconds. Runs nest: a script can insert a
   script that runs at once, or fire an event. A nested run has a budget
   of its own; when it ends, the run it was in carries on with its own
   (not counting the time the nested one took). */
typedef struct {
    u64 deadline, started;
    long steps;
    int frame_depth;
} JsRun;
static int js_run_depth; /* runs in progress */

static void js_run_begin(JsRun *r, int ms) {
    r->deadline = js_deadline;
    r->steps = js_steps;
    r->frame_depth = js_frame_depth;
    r->started = uptime_ms();
    js_run_depth++;
    js_steps = 0;
    js_abort = 0;
    js_deadline = r->started + (u64)ms;
}

static void js_run_end(JsRun *r) {
    if (js_run_depth > 0) js_run_depth--;
    if (js_run_depth > 0) { /* back in the run this one was in */
        js_deadline = r->deadline ? r->deadline + (uptime_ms() - r->started) : 0;
        js_steps = r->steps;
        js_frame_depth = r->frame_depth;
    }
}
static char *js_stack_base;
static char *js_stack_floor; /* on a coroutine's stack: built-ins that recurse must stay above this */
/* (js_stack_room, declared in js_parse.c: how deep scripts may go from js_stack_base; less on a coroutine's stack) */

/* js_coro.c: generators and async functions */
typedef struct Coro Coro;
static JsObj *GeneratorProto, *AsyncGeneratorProto;
static int coro_make_generator(JsObj *fo, Value this_v, Value *args, int argc, Value *ret);
static int coro_run_async(JsObj *fo, Value this_v, Value *args, int argc, Value *ret);
static int coro_await(Value v, Value *out);
static void coro_init(JsObj *global);
static JsStr *jump_label;       /* the label of a break / continue in flight */
static int optional_short;      /* an optional chain met null or undefined */
static JsObj *native_new_target; /* set while a built-in runs as a constructor */
static int native_call_line, native_call_pos; /* where the last built-in was called from */

typedef struct {
    Env *env;        /* the current scope */
    Env *var_env;    /* where var declarations go */
    Value this_v;
    JsObj *func;     /* the function running (NULL at the top level) */
    JsObj *home;     /* for super */
    JsObj *new_target;
    Value *args;
    int argc;
    int strict;      /* strict mode: this is not replaced by the global object */
} Frame;

static int coro_yield(Ast *n, Frame *f, Value *out); /* (js_coro.c) */

/* The global object (window) and the prototypes of the built-in types. */
static JsObj *global_obj, *ObjectProto, *FunctionProto, *ArrayProto, *StringProto, *NumberProto, *BooleanProto,
    *ErrorProto, *TypeErrorProto, *RangeErrorProto, *SyntaxErrorProto, *ReferenceErrorProto, *DateProto, *RegExpProto,
    *MapProto, *SetProto, *PromiseProto, *NodeProto, *EventProto;
static Env *global_env, *global_lex;

/* Names used by the engine itself. */
static JsStr *S_length, *S_prototype, *S_constructor, *S_name, *S_message, *S_toString, *S_valueOf, *S_arguments,
    *S_undefined, *S_null, *S_true, *S_false, *S_get, *S_set, *S_value, *S_then, *S_stack, *S_empty, *S_object,
    *S_function, *S_number, *S_string, *S_boolean, *S_index, *S_input, *S_done, *S_lastIndex, *S_enumerable,
    *S_writable, *S_configurable, *S_toJSON, *S_handleEvent, *S_proto, *S_iterator;

static void init_names(void) {
    S_length = intern_c("length");
    S_prototype = intern_c("prototype");
    S_constructor = intern_c("constructor");
    S_name = intern_c("name");
    S_message = intern_c("message");
    S_toString = intern_c("toString");
    S_valueOf = intern_c("valueOf");
    S_arguments = intern_c("arguments");
    S_undefined = intern_c("undefined");
    S_null = intern_c("null");
    S_true = intern_c("true");
    S_false = intern_c("false");
    S_get = intern_c("get");
    S_set = intern_c("set");
    S_value = intern_c("value");
    S_then = intern_c("then");
    S_stack = intern_c("stack");
    S_empty = intern_c("");
    S_object = intern_c("object");
    S_function = intern_c("function");
    S_number = intern_c("number");
    S_string = intern_c("string");
    S_boolean = intern_c("boolean");
    S_index = intern_c("index");
    S_input = intern_c("input");
    S_done = intern_c("done");
    S_lastIndex = intern_c("lastIndex");
    S_enumerable = intern_c("enumerable");
    S_writable = intern_c("writable");
    S_configurable = intern_c("configurable");
    S_toJSON = intern_c("toJSON");
    S_handleEvent = intern_c("handleEvent");
    S_proto = intern_c("__proto__");
    S_iterator = intern_c("@@iterator");
}

static int dom_get(JsObj *o, JsStr *key, Value *out, int *handled);
static int dom_set(JsObj *o, JsStr *key, Value v, int *handled);
static JsObj *regexp_new(JsStr *pattern, JsStr *flags, char *error);

/* ---------------------------------------------------------------- errors */

static Value arg(Value *args, int argc, int i) { return i < argc ? args[i] : v_undef(); }

static JsObj *make_error(JsObj *proto, const char *msg) {
    JsObj *e = obj_new_raw(CL_ERROR, proto);
    prop_set_raw(e, S_message, v_str(str_c(msg)), P_HIDDEN);
    prop_set_raw(e, S_stack, v_str(str_c(msg)), P_HIDDEN);
    return e;
}

static void describe_exception(char *out, int max);
static int js_trace_throws; /* BROWSER_TRACE=1: print every error when it is thrown */

static int throw_error(JsObj *proto, const char *msg) {
    if (js_trace_throws) printf("[throw] %s\n", msg);
    js_exception = v_obj(make_error(proto, msg));
    return C_THROW;
}

/* Where the stack was when script code was entered: limits are measured
   from here. Code entered again from inside a script (an inserted
   <script>, an event a script fired, a body parsed on first call) must not
   move it deeper, or the limits would stop protecting the real stack. */
static void js_set_stack_base(char *here) {
    if (!js_stack_base || here > js_stack_base) js_stack_base = here;
}

static int throw_type(const char *a, const char *b, const char *c) {
    Buf m = {0};
    buf_c(&m, a);
    if (b) buf_c(&m, b);
    if (c) buf_c(&m, c);
    int r = throw_error(TypeErrorProto, m.p ? m.p : "");
    free(m.p);
    return r;
}

#define TRY(x)                      \
    do {                            \
        int r_ = (x);               \
        if (r_ != C_NORMAL) return r_; \
    } while (0)

/* ---------------------------------------------------------------- conversions */

static int is_callable(Value v) { return v.t == V_OBJ && v.u.o->cls == CL_FUNCTION; }
static int is_nullish(Value v) { return v.t == V_UNDEF || v.t == V_NULL; }

static int call_function(JsObj *fo, Value this_v, Value *args, int argc, Value *ret);
static int get_value(Value obj, JsStr *key, Value *out);

static int to_boolean(Value v) {
    switch (v.t) {
    case V_BOOL: return v.u.b;
    case V_NUM: return !(v.u.n == 0 || is_nan(v.u.n));
    case V_STR: return v.u.s->len > 0;
    case V_OBJ: return 1;
    case V_SYM: return 1;
    case V_BIG: return !big_is_zero(v.u.s);
    }
    return 0;
}

/* ToPrimitive: hint 's' string, 'n' number / default. */
static int to_primitive(Value v, int hint, Value *out) {
    if (v.t != V_OBJ) {
        *out = v;
        return C_NORMAL;
    }
    JsObj *o = v.u.o;
    if (o->cls == CL_DATE && hint != 'n') hint = 's';
    JsStr *order[2] = {hint == 's' ? S_toString : S_valueOf, hint == 's' ? S_valueOf : S_toString};
    for (int i = 0; i < 2; i++) {
        Value f;
        TRY(get_value(v, order[i], &f));
        if (is_callable(f)) {
            Value r;
            TRY(call_function(f.u.o, v, NULL, 0, &r));
            if (r.t != V_OBJ) {
                *out = r;
                return C_NORMAL;
            }
        }
    }
    return throw_type("Cannot convert object to primitive value", NULL, NULL);
}

static double str_to_number(const JsStr *s) { return parse_number(s->s, s->len, NULL, 0); }

static int to_number(Value v, double *out) {
    switch (v.t) {
    case V_UNDEF: *out = js_nan(); return C_NORMAL;
    case V_NULL: *out = 0; return C_NORMAL;
    case V_BOOL: *out = v.u.b; return C_NORMAL;
    case V_NUM: *out = v.u.n; return C_NORMAL;
    case V_STR: *out = str_to_number(v.u.s); return C_NORMAL;
    case V_SYM: return throw_type("Cannot convert a Symbol value to a number", NULL, NULL);
    case V_BIG: return throw_type("Cannot convert a BigInt value to a number", NULL, NULL);
    }
    Value p;
    TRY(to_primitive(v, 'n', &p));
    return to_number(p, out);
}

static JsStr *number_str(double d) {
    char text[40];
    number_to_text(d, text);
    return str_c(text);
}

static int to_string(Value v, JsStr **out) {
    switch (v.t) {
    case V_UNDEF: *out = S_undefined; return C_NORMAL;
    case V_NULL: *out = S_null; return C_NORMAL;
    case V_BOOL: *out = v.u.b ? S_true : S_false; return C_NORMAL;
    case V_NUM: *out = number_str(v.u.n); return C_NORMAL;
    case V_STR: *out = v.u.s; return C_NORMAL;
    case V_SYM: return throw_type("Cannot convert a Symbol value to a string", NULL, NULL);
    case V_BIG: *out = big_text(v.u.s, 10); return C_NORMAL;
    }
    Value p;
    TRY(to_primitive(v, 's', &p));
    return to_string(p, out);
}

/* A property key from any value. */
static int to_key(Value v, JsStr **out) {
    if (v.t == V_SYM) {
        *out = v.u.s;
        return C_NORMAL;
    }
    if (v.t == V_NUM && v.u.n >= 0 && v.u.n < 1e9 && v.u.n == js_trunc(v.u.n)) {
        char text[16];
        number_to_text(v.u.n, text);
        *out = intern_c(text);
        return C_NORMAL;
    }
    JsStr *s;
    TRY(to_string(v, &s));
    *out = interned(s);
    return C_NORMAL;
}

static JsStr *type_of(Value v) {
    switch (v.t) {
    case V_UNDEF: return S_undefined;
    case V_NULL: return S_object;
    case V_BOOL: return S_boolean;
    case V_NUM: return S_number;
    case V_STR: return S_string;
    case V_SYM: return intern_c("symbol");
    case V_BIG: return intern_c("bigint");
    }
    return v.u.o->cls == CL_FUNCTION ? S_function : S_object;
}

static int strict_equals(Value a, Value b) {
    if (a.t != b.t) return 0;
    switch (a.t) {
    case V_UNDEF: case V_NULL: return 1;
    case V_BOOL: return a.u.b == b.u.b;
    case V_NUM: return a.u.n == b.u.n;
    case V_STR: return str_eq(a.u.s, b.u.s);
    case V_SYM: return a.u.s == b.u.s;
    case V_BIG: return str_eq(a.u.s, b.u.s);
    }
    return a.u.o == b.u.o;
}

/* SameValueZero: like === but NaN equals NaN (for includes, Map, Set). */
static int same_value_zero(Value a, Value b) {
    if (a.t == V_NUM && b.t == V_NUM && is_nan(a.u.n) && is_nan(b.u.n)) return 1;
    return strict_equals(a, b);
}

static int loose_equals(Value a, Value b, int *result) {
    for (;;) {
        if (a.t == b.t) {
            *result = strict_equals(a, b);
            return C_NORMAL;
        }
        if (is_nullish(a) && is_nullish(b)) {
            *result = 1;
            return C_NORMAL;
        }
        if (is_nullish(a) || is_nullish(b)) {
            *result = 0;
            return C_NORMAL;
        }
        if (a.t == V_NUM && b.t == V_STR) {
            b = v_num(str_to_number(b.u.s));
            continue;
        }
        if (a.t == V_STR && b.t == V_NUM) {
            a = v_num(str_to_number(a.u.s));
            continue;
        }
        if (a.t == V_BOOL) {
            a = v_num(a.u.b);
            continue;
        }
        if (b.t == V_BOOL) {
            b = v_num(b.u.b);
            continue;
        }
        if (a.t == V_SYM || b.t == V_SYM) break;
        if (a.t == V_BIG && b.t == V_STR) {
            Big t = {0};
            int ok = big_from_text(&t, b.u.s->s, b.u.s->len, 0);
            *result = ok && str_eq(a.u.s, big_store(&t));
            big_free(&t);
            return C_NORMAL;
        }
        if (a.t == V_STR && b.t == V_BIG) {
            Value x = a;
            a = b;
            b = x;
            continue;
        }
        if (a.t == V_BIG && b.t == V_NUM) {
            *result = big_compare_number(a.u.s, b.u.n) == 0;
            return C_NORMAL;
        }
        if (a.t == V_NUM && b.t == V_BIG) {
            *result = big_compare_number(b.u.s, a.u.n) == 0;
            return C_NORMAL;
        }
        if (a.t == V_OBJ) {
            TRY(to_primitive(a, 'n', &a));
            continue;
        }
        if (b.t == V_OBJ) {
            TRY(to_primitive(b, 'n', &b));
            continue;
        }
        *result = 0;
        return C_NORMAL;
    }
    *result = 0; /* (a symbol equals only itself) */
    return C_NORMAL;
}

/* ---------------------------------------------------------------- properties */

static JsObj *SymbolProto, *BigIntProto;

/* Is a property key a symbol's? (those are left out of keys, for...in, JSON) */
static int is_symbol_key(const JsStr *k) { return k->len >= 2 && k->s[0] == '@' && k->s[1] == '@'; }

static JsObj *proto_of_primitive(Value v) {
    switch (v.t) {
    case V_SYM: return SymbolProto;
    case V_BIG: return BigIntProto;
    case V_STR: return StringProto;
    case V_NUM: return NumberProto;
    case V_BOOL: return BooleanProto;
    }
    return NULL;
}

static JsObj *make_function_proto(JsObj *f);

/* Typed arrays are arrays whose fn_kind says what their elements are:
   1 Int8 2 Uint8 3 Uint8Clamped 4 Int16 5 Uint16 6 Int32 7 Uint32 8 Float32 9 Float64. */
static double typed_coerce(int kind, double d) {
    switch (kind) {
    case 1: return (signed char)to_int32(d);
    case 2: return (unsigned char)to_int32(d);
    case 3: return is_nan(d) ? 0 : d < 0 ? 0 : d > 255 ? 255 : js_floor(d + 0.5);
    case 4: return (short)to_int32(d);
    case 5: return (unsigned short)to_int32(d);
    case 6: return to_int32(d);
    case 7: return to_uint32(d);
    case 8: return (double)(float)d;
    }
    return d;
}

/* ArrayBuffers (CL_BUFFER) hold bytes: extra, extra_len of them. A typed
   array made over one is a view of it: target is the buffer and magic the
   byte offset, and the buffer lists its views in bound/nbound (extra_cap
   slots; the collector drops views that died). A view keeps its elements
   in arr like any array, so reading is as quick as ever; writing an
   element writes its bytes too and refreshes the other views over them. */
static const int TYPED_BYTES[12] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 8, 8, 8};
static int buffers_made; /* (the collector has views to prune) */

static double typed_decode(int kind, const unsigned char *p) {
    switch (kind) {
    case 1: return (signed char)p[0];
    case 2: case 3: return p[0];
    case 4: { short v; memcpy(&v, p, 2); return v; }
    case 5: { unsigned short v; memcpy(&v, p, 2); return v; }
    case 6: { int v; memcpy(&v, p, 4); return v; }
    case 7: { unsigned v; memcpy(&v, p, 4); return v; }
    case 8: { float v; memcpy(&v, p, 4); return v; }
    }
    double v;
    memcpy(&v, p, 8);
    return v;
}

/* d already coerced to the kind */
static void typed_encode(int kind, double d, unsigned char *p) {
    switch (kind) {
    case 1: case 2: case 3: p[0] = (unsigned char)(int)d; return;
    case 4: case 5: { unsigned short v = (unsigned short)(int)d; memcpy(p, &v, 2); return; }
    case 6: { int v = (int)d; memcpy(p, &v, 4); return; }
    case 7: { unsigned v = (unsigned)d; memcpy(p, &v, 4); return; }
    case 8: { float v = (float)d; memcpy(p, &v, 4); return; }
    }
    memcpy(p, &d, 8);
}

static int is_view(JsObj *a) { return a->cls == CL_ARRAY && a->fn_kind && a->target && a->target->cls == CL_BUFFER; }

/* Bytes [from, to) of a buffer changed: re-read them into its views. */
static void buffer_changed(JsObj *buf, int from, int to, JsObj *except) {
    const unsigned char *bytes = buf->extra;
    for (int i = 0; i < buf->nbound; i++) {
        JsObj *v = buf->bound[i].u.o;
        if (v == except || !v->fn_kind) continue;
        int size = TYPED_BYTES[v->fn_kind], off = v->magic;
        int end = off + v->alen * size;
        if (to <= off || from >= end) continue;
        int first = from > off ? (from - off) / size : 0;
        int last = (to - off + size - 1) / size;
        if (last > v->alen) last = v->alen;
        for (int k = first; k < last; k++) {
            if (off + (k + 1) * size > buf->extra_len) break;
            v->arr[k] = v_num(typed_decode(v->fn_kind, bytes + off + k * size));
        }
    }
}

/* Elements [from, to) of view a changed: write their bytes, tell the others. */
static void view_wrote(JsObj *a, int from, int to) {
    JsObj *buf = a->target;
    int size = TYPED_BYTES[a->fn_kind];
    if (to > a->alen) to = a->alen;
    for (int k = from; k < to; k++) {
        int at = a->magic + k * size;
        if (at + size > buf->extra_len) break;
        double d = a->arr[k].t == V_NUM ? a->arr[k].u.n : js_nan();
        d = typed_coerce(a->fn_kind, d);
        a->arr[k] = v_num(d);
        typed_encode(a->fn_kind, d, (unsigned char *)buf->extra + at);
    }
    buffer_changed(buf, a->magic + from * size, a->magic + to * size, a);
}

/* after a built-in rewrote a typed array's elements in place */
static void typed_touched(Value v) {
    if (v.t == V_OBJ && is_view(v.u.o)) view_wrote(v.u.o, 0, v.u.o->alen);
}

static int typed_put(JsObj *a, int index, Value v) {
    if (index < 0 || index >= a->alen) return C_NORMAL; /* fixed length */
    double d;
    TRY(to_number(v, &d));
    a->arr[index] = v_num(typed_coerce(a->fn_kind, d));
    if (is_view(a)) view_wrote(a, index, index + 1);
    return C_NORMAL;
}

/* BROWSER_MISSING: report (once each) properties scripts look for and do
   not find on the browser's own objects (window, navigator, prototypes,
   elements): features the browser lacks. */
static int missing_on;
static JsObj *missing_document;
static char *missing_seen[1024];
static int missing_count;

static void note_missing(JsObj *o, JsStr *key, const char *how) {
    if (!missing_on || is_symbol_key(key) || key->index >= 0) return;
    int element = o->cls == CL_NODE && o->node && o != missing_document;
    if (!o->gc.builtin && !element) return;
    char what[96] = "";
    if (o == global_obj) strcpy(what, "window");
    else if (o == missing_document) strcpy(what, "document");
    else if (element) strcpy(what, "an element");
    else {
        Prop *own = own_prop(o, S_constructor), *n = NULL;
        if (o->cls == CL_FUNCTION) n = own_prop(o, S_name);
        else if (own && own->v.t == V_OBJ) n = own_prop(own->v.u.o, S_name);
        if (n && n->v.t == V_STR) {
            strncat_safe(what, n->v.u.s->s, 60);
            if (o->cls != CL_FUNCTION) strncat_safe(what, ".prototype", 12);
        } else {
            for (JsObj *p = o->proto; p && !n; p = p->proto) {
                Prop *c = own_prop(p, S_constructor);
                if (c && c->v.t == V_OBJ) n = own_prop(c->v.u.o, S_name);
            }
            strcpy(what, "a built-in ");
            strncat_safe(what, n && n->v.t == V_STR ? n->v.u.s->s : "object", 60);
        }
    }
    char line[200] = "";
    strncat_safe(line, what, 96);
    strncat_safe(line, ".", 2);
    strncat_safe(line, key->s, 90);
    for (int i = 0; i < missing_count; i++)
        if (!strcmp(missing_seen[i], line)) return;
    if (missing_count < 1024) {
        missing_seen[missing_count] = malloc(strlen(line) + 1);
        if (missing_seen[missing_count]) strcpy(missing_seen[missing_count++], line);
    }
    printf("[missing] %s (%s)\n", line, how);
}

static int get_from(JsObj *o, JsStr *key, Value receiver, Value *out) {
    if (key == S_proto && !own_prop(o, key)) {
        *out = o->proto ? v_obj(o->proto) : v_null();
        return C_NORMAL;
    }
    for (JsObj *p = o; p; p = p->proto) {
        if (p->cls >= CL_NODE) {
            int handled = 0;
            int r = dom_get(p, key, out, &handled);
            if (handled || r != C_NORMAL) return r;
        }
        if (p->cls == CL_ARRAY || p->cls == CL_ARGUMENTS) {
            if (key == S_length) {
                *out = v_num(p->alen);
                return C_NORMAL;
            }
            if (key->index >= 0) {
                if (key->index < p->alen) {
                    *out = p->arr[key->index];
                    return C_NORMAL;
                }
            }
        } else if (p->cls == CL_STRING && p->this_val.t == V_STR) {
            if (key == S_length) {
                *out = v_num(p->this_val.u.s->len);
                return C_NORMAL;
            }
            if (key->index >= 0 && key->index < p->this_val.u.s->len) {
                *out = v_str(str_new(p->this_val.u.s->s + key->index, 1));
                return C_NORMAL;
            }
        } else if (p->cls == CL_FUNCTION && p == o) {
            if (key == S_prototype && !own_prop(p, key) && p->fn && (p->fn_kind == FN_NORMAL || p->fn_kind >= FN_CLASS)) {
                *out = v_obj(make_function_proto(p));
                return C_NORMAL;
            }
            if (key == S_name && !own_prop(p, key)) {
                *out = v_str(p->fn && p->fn->str ? p->fn->str : S_empty);
                return C_NORMAL;
            }
            if (key == S_length && !own_prop(p, key)) {
                int n = 0;
                if (p->fn) {
                    for (Ast *a = p->fn->list; a && a->type != N_SPREAD && a->type != N_ASSIGN; a = a->next) n++;
                } else {
                    n = p->magic >> 16;
                }
                *out = v_num(n);
                return C_NORMAL;
            }
        }
        Prop *pr = own_prop(p, key);
        if (pr) {
            if (pr->flags & P_ACCESSOR) {
                if (!pr->get) {
                    *out = v_undef();
                    return C_NORMAL;
                }
                return call_function(pr->get, receiver, NULL, 0, out);
            }
            *out = pr->v;
            return C_NORMAL;
        }
    }
    if (missing_on) note_missing(o, key, "read");
    *out = v_undef();
    return C_NORMAL;
}

static int get_value(Value obj, JsStr *key, Value *out) {
    if (obj.t == V_OBJ) return get_from(obj.u.o, key, obj, out);
    if (is_nullish(obj)) return throw_type("Cannot read properties of ", obj.t == V_NULL ? "null" : "undefined", NULL);
    if (obj.t == V_STR) {
        if (key == S_length) {
            *out = v_num(obj.u.s->len);
            return C_NORMAL;
        }
        if (key->index >= 0) {
            *out = key->index < obj.u.s->len ? v_str(str_new(obj.u.s->s + key->index, 1)) : v_undef();
            return C_NORMAL;
        }
    }
    return get_from(proto_of_primitive(obj), key, obj, out);
}

static int get_index(Value obj, Value key, Value *out) {
    if (obj.t == V_OBJ && key.t == V_NUM && (obj.u.o->cls == CL_ARRAY || obj.u.o->cls == CL_ARGUMENTS)) {
        double d = key.u.n;
        if (d >= 0 && d < obj.u.o->alen && d == js_trunc(d)) {
            *out = obj.u.o->arr[(int)d];
            return C_NORMAL;
        }
    }
    if (obj.t == V_STR && key.t == V_NUM) {
        double d = key.u.n;
        if (d >= 0 && d < obj.u.s->len && d == js_trunc(d)) {
            *out = v_str(str_new(obj.u.s->s + (int)d, 1));
            return C_NORMAL;
        }
    }
    if (is_nullish(obj)) {
        JsStr *k;
        if (to_string(key, &k) != C_NORMAL) k = S_empty;
        Buf m = {0};
        buf_c(&m, obj.t == V_NULL ? "null (reading '" : "undefined (reading '");
        buf_add(&m, k->s, k->len < 60 ? k->len : 60);
        buf_c(&m, "')");
        int r = throw_type("Cannot read properties of ", m.p, NULL);
        free(m.p);
        return r;
    }
    JsStr *k;
    TRY(to_key(key, &k));
    return get_value(obj, k, out);
}

static int put_value(Value obj, JsStr *key, Value v) {
    if (obj.t != V_OBJ) {
        if (is_nullish(obj)) return throw_type("Cannot set properties of ", obj.t == V_NULL ? "null" : "undefined", NULL);
        return C_NORMAL; /* properties of primitives are dropped */
    }
    JsObj *o = obj.u.o;
    if (key == S_proto) {
        if (v.t == V_OBJ) {
            for (JsObj *p = v.u.o; p; p = p->proto) {
                if (p == o) return throw_type("Cyclic __proto__ value", NULL, NULL);
            }
            o->proto = v.u.o;
        } else if (v.t == V_NULL) {
            o->proto = NULL;
        }
        return C_NORMAL;
    }
    if (o->cls == CL_ARRAY && o->fn_kind) {
        if (key == S_length) return C_NORMAL;
        if (key->index >= 0) return typed_put(o, key->index, v);
    }
    if (o->cls == CL_ARRAY || o->cls == CL_ARGUMENTS) {
        if (key == S_length) {
            double d;
            TRY(to_number(v, &d));
            if (d < 0 || d != js_trunc(d) || d > 4294967295.0) return throw_error(RangeErrorProto, "Invalid array length");
            if (d <= 2147483647.0) arr_set_len(o, (int)d);
            return C_NORMAL;
        }
        if (key->index >= 0 && key->index < o->alen + SPARSE_GAP) {
            if (key->index >= o->alen) arr_set_len(o, key->index + 1);
            if (!js_oom && key->index < o->alen) o->arr[key->index] = v;
            return C_NORMAL;
        }
    }
    for (JsObj *p = o; p; p = p->proto) {
        if (p->cls >= CL_NODE) {
            int handled = 0;
            int r = dom_set(p, key, v, &handled);
            if (handled || r != C_NORMAL) return r;
        }
        Prop *pr = own_prop(p, key);
        if (pr) {
            if (pr->flags & P_ACCESSOR) {
                if (!pr->set) return C_NORMAL;
                Value ignored;
                return call_function(pr->set, obj, &v, 1, &ignored);
            }
            if (pr->flags & P_READONLY) return C_NORMAL;
            if (p == o) {
                if (!o->frozen) pr->v = v;
                return C_NORMAL;
            }
            break;
        }
    }
    if (o->frozen) return C_NORMAL;
    Prop *pr = prop_add(o, key);
    pr->v = v;
    return C_NORMAL;
}

static int put_index(Value obj, Value key, Value v) {
    if (obj.t == V_OBJ && key.t == V_NUM && obj.u.o->cls == CL_ARRAY) {
        double d = key.u.n;
        if (obj.u.o->fn_kind) return d == js_trunc(d) && d >= 0 && d < 2147483647.0 ? typed_put(obj.u.o, (int)d, v) : C_NORMAL;
        if (d >= 0 && d == js_trunc(d) && d < obj.u.o->alen + SPARSE_GAP) {
            JsObj *a = obj.u.o;
            int i = (int)d;
            if (i >= a->alen) arr_set_len(a, i + 1);
            if (!js_oom && i < a->alen) a->arr[i] = v;
            return C_NORMAL;
        }
    }
    JsStr *k;
    TRY(to_key(key, &k));
    return put_value(obj, k, v);
}

static void function_own_props(JsObj *f);

static int has_property(JsObj *o, JsStr *key) {
    if (o && o->cls == CL_FUNCTION && (key == S_prototype || key == S_name || key == S_length)) function_own_props(o);
    for (JsObj *p = o; p; p = p->proto) {
        if ((p->cls == CL_ARRAY || p->cls == CL_ARGUMENTS) && (key == S_length || (key->index >= 0 && key->index < p->alen))) return 1;
        if (own_prop(p, key)) return 1;
        if (p->cls >= CL_NODE) {
            Value v;
            int handled = 0;
            dom_get(p, key, &v, &handled);
            if (handled && v.t != V_UNDEF) return 1;
        }
    }
    if (missing_on) note_missing(o, key, "tested with in");
    return 0;
}

static void define_hidden(JsObj *o, const char *name, Value v) { prop_set_raw(o, intern_c(name), v, P_HIDDEN); }

static JsObj *new_object(void) { return obj_new_raw(CL_OBJECT, ObjectProto); }

static JsObj *new_array(int n) {
    JsObj *a = obj_new_raw(CL_ARRAY, ArrayProto);
    if (n) arr_reserve(a, n);
    return a;
}

static JsObj *make_function_proto(JsObj *f) {
    JsObj *p = new_object();
    prop_set_raw(p, S_constructor, v_obj(f), P_HIDDEN);
    prop_set_raw(f, S_prototype, v_obj(p), P_HIDDEN);
    return p;
}

/* A function's length, name and prototype are made when first read (most
   functions never are). Anything looking at its own properties as such
   ("prototype" in f, hasOwnProperty, getOwnPropertyNames ...) makes them
   real first. */
static void function_own_props(JsObj *f) {
    if (!f || f->cls != CL_FUNCTION || own_prop(f, S_length)) return;
    Value v;
    if (get_value(v_obj(f), S_length, &v) == C_NORMAL) prop_set_raw(f, S_length, v, P_HIDDEN | P_READONLY);
    if (!own_prop(f, S_name) && get_value(v_obj(f), S_name, &v) == C_NORMAL) prop_set_raw(f, S_name, v, P_HIDDEN | P_READONLY);
    if (f->fn && (f->fn_kind == FN_NORMAL || f->fn_kind >= FN_CLASS) && !own_prop(f, S_prototype)) make_function_proto(f);
}

static JsObj *make_native(const char *name, Native fn, int nargs, int magic) {
    JsObj *f = obj_new_raw(CL_FUNCTION, FunctionProto);
    f->native = fn;
    f->magic = magic | nargs << 16;
    f->fn_kind = FN_NONE;
    if (name) prop_set_raw(f, S_name, v_str(intern_c(name)), P_HIDDEN);
    return f;
}

static JsObj *define_method(JsObj *o, const char *name, Native fn, int nargs, int magic) {
    JsObj *f = make_native(name, fn, nargs, magic);
    prop_set_raw(o, intern_c(name), v_obj(f), P_HIDDEN);
    return f;
}

static void define_getter(JsObj *o, const char *name, Native get, Native set, int magic) {
    Prop *p = prop_add(o, intern_c(name));
    p->flags = P_ACCESSOR | P_HIDDEN;
    p->get = make_native(name, get, 0, magic);
    p->set = set ? make_native(name, set, 1, magic) : NULL;
}

/* ---------------------------------------------------------------- functions */

/* What a function needs to know before running its body: the names of
   its var declarations, and whether it uses `arguments`. */
typedef struct {
    JsStr **vars;
    int nvars;
    int uses_arguments;
    int strict;      /* its body starts with "use strict" */
} FnInfo;

static void collect_pattern_names(Ast *t, FnInfo *info);

static void add_var_name(FnInfo *info, JsStr *name) {
    for (int i = 0; i < info->nvars; i++) {
        if (info->vars[i] == name) return;
    }
    JsStr **v = realloc(info->vars, sizeof(JsStr *) * (size_t)(info->nvars + 1));
    if (!v) return;
    info->vars = v;
    info->vars[info->nvars++] = name;
}

static void collect_pattern_names(Ast *t, FnInfo *info) {
    if (!t) return;
    switch (t->type) {
    case N_IDENT: add_var_name(info, t->str); break;
    case N_ASSIGN: collect_pattern_names(t->a, info); break;
    case N_SPREAD: collect_pattern_names(t->a, info); break;
    case N_ARRAY:
        for (Ast *e = t->list; e; e = e->next) collect_pattern_names(e, info);
        break;
    case N_OBJECT:
        for (Ast *p = t->list; p; p = p->next) collect_pattern_names(p->b, info);
        break;
    }
}

static void scan_uses_arguments(Ast *n, FnInfo *info) {
    if (!n || info->uses_arguments) return;
    if (n->type == N_IDENT && n->str == S_arguments) {
        info->uses_arguments = 1;
        return;
    }
    if (n->type == N_FUNC && !(n->flags & F_ARROW)) return;
    if (n->type == N_FUNC && (n->flags & F_LAZY_ARGS)) { /* an arrow function's unparsed body uses it */
        info->uses_arguments = 1;
        return;
    }
    if (n->type == N_CLASS) return;
    scan_uses_arguments(n->a, info);
    scan_uses_arguments(n->b, info);
    scan_uses_arguments(n->c, info);
    scan_uses_arguments(n->d, info);
    for (Ast *c = n->list; c; c = c->next) scan_uses_arguments(c, info);
}

static void collect_vars(Ast *n, FnInfo *info) {
    if (!n) return;
    switch (n->type) {
    case N_VAR:
        if (n->op == 'v') {
            for (Ast *d = n->list; d; d = d->next) collect_pattern_names(d->a, info);
        }
        return;
    case N_BLOCK:
        for (Ast *s = n->list; s; s = s->next) collect_vars(s, info);
        return;
    case N_IF: collect_vars(n->b, info); collect_vars(n->c, info); return;
    case N_FOR: collect_vars(n->a, info); collect_vars(n->d, info); return;
    case N_FORIN: collect_vars(n->a, info); collect_vars(n->d, info); return;
    case N_WHILE: collect_vars(n->b, info); return;
    case N_DOWHILE: collect_vars(n->a, info); return;
    case N_TRY: collect_vars(n->a, info); collect_vars(n->c, info); collect_vars(n->d, info); return;
    case N_LABEL: collect_vars(n->a, info); return;
    case N_SWITCH:
        for (Ast *c = n->list; c; c = c->next) {
            for (Ast *s = c->list; s; s = s->next) collect_vars(s, info);
        }
        return;
    }
}

static FnInfo *function_info(Ast *fn) {
    if (fn->cache) return fn->cache;
    FnInfo *info = arena_alloc(sizeof(FnInfo));
    if (!(fn->flags & F_EXPR_BODY)) collect_vars(fn->a, info);
    if (!(fn->flags & F_EXPR_BODY) && fn->a && fn->a->list && fn->a->list->type == N_EXPR && fn->a->list->a->type == N_STR &&
        fn->a->list->a->str->len == 10 && !memcmp(fn->a->list->a->str->s, "use strict", 10))
        info->strict = 1;
    if (!(fn->flags & F_ARROW)) {
        scan_uses_arguments(fn->a, info);
        for (Ast *p = fn->list; p; p = p->next) scan_uses_arguments(p, info);
    }
    fn->cache = info;
    return info;
}

static JsObj *make_closure(Ast *fn, Frame *f) {
    JsObj *o = obj_new_raw(CL_FUNCTION, FunctionProto);
    o->fn = fn;
    o->scope = f->env;
    o->fn_kind = (fn->flags & F_ARROW) ? FN_ARROW : (fn->flags & F_METHOD) ? FN_METHOD : FN_NORMAL;
    int lazy = fn->a && fn->a->type == N_LAZY; /* (not parsed yet: its strictness was noted when skimmed) */
    o->strict = (unsigned char)(f->strict || (lazy ? (fn->flags & F_LAZY_STRICT) != 0 : function_info(fn)->strict));
    if (fn->flags & F_ARROW) {
        o->this_val = f->this_v;
        o->home = f->home;
        o->target = f->new_target;
    }
    return o;
}

static int eval(Ast *n, Frame *f, Value *out);
static int exec(Ast *n, Frame *f, Value *out);
static int exec_list(Ast *first, Frame *f, Value *out);
static int bind_pattern(Ast *t, Value v, Frame *f, int mode);
static int construct(JsObj *fo, Value *args, int argc, JsObj *new_target, Value *out);

static int check_limits(void) {
    if (js_abort) return C_THROW;
    if (js_oom) {
        js_abort = 1;
        js_exception = v_str(str_c("the script ran out of memory"));
        return C_THROW;
    }
    if (++js_steps > js_step_limit || ((js_steps & 1023) == 0 && js_deadline && uptime_ms() > js_deadline)) {
        js_abort = 1;
        char text[140] = "the script ran too long and was stopped (at line ";
        char t[16];
        number_to_text(js_exec_line, t);
        strncat_safe(text, t, 12);
        strncat_safe(text, ", last call at character ", 26);
        number_to_text(js_call_pos, t);
        strncat_safe(text, t, 12);
        strncat_safe(text, ")", 2);
        if (getenv("BROWSER_TRACE")) { /* the functions it was in, innermost first */
            printf("[stack] functions running (where each starts):");
            int top = js_frame_depth < JS_FRAMES ? js_frame_depth : JS_FRAMES;
            for (int i = top - 1; i >= 0 && i >= top - 12; i--) {
                Ast *fn = js_frames[i];
                printf(" %d", fn && fn->cache_len > 0 ? fn->cache_len - 1 : -1);
            }
            printf("\n");
        }
        js_exception = v_str(str_c(text));
        return C_THROW;
    }
    return C_NORMAL;
}

/* Run a JavaScript function's body. */
static int run_function_body(JsObj *fo, Value this_v, Value *args, int argc, JsObj *new_target, Value *ret);

static int run_function(JsObj *fo, Value this_v, Value *args, int argc, JsObj *new_target, Value *ret) {
    if (js_frame_depth < JS_FRAMES) js_frames[js_frame_depth] = fo->fn;
    js_frame_depth++;
    int r;
    if (!new_target && fo->fn && (fo->fn->flags & F_GENERATOR)) r = coro_make_generator(fo, this_v, args, argc, ret);
    else if (!new_target && fo->fn && (fo->fn->flags & F_ASYNC)) r = coro_run_async(fo, this_v, args, argc, ret);
    else r = run_function_body(fo, this_v, args, argc, new_target, ret);
    js_frame_depth--;
    return r;
}

static int run_function_body(JsObj *fo, Value this_v, Value *args, int argc, JsObj *new_target, Value *ret) {
    char here;
    if (js_stack_base - &here > js_stack_room) return throw_error(RangeErrorProto, "Maximum call stack size exceeded");
    TRY(check_limits());
    Ast *fn = fo->fn;
    if (fn->a && fn->a->type == N_LAZY && parse_lazy_body(fn) != 0) { /* first call: parse the body now */
        return throw_error(SyntaxErrorProto, parse_error);
    }
    FnInfo *info = function_info(fn);
    Env *env = env_new(fo->scope);
    Frame fr;
    fr.env = env;
    fr.var_env = env;
    fr.func = fo;
    fr.args = args;
    fr.argc = argc;
    if (fo->fn_kind == FN_ARROW) {
        fr.this_v = fo->this_val;
        fr.home = fo->home;
        fr.new_target = fo->target;
    } else {
        fr.this_v = this_v;
        fr.home = fo->home;
        fr.new_target = new_target;
        /* sloppy mode: a missing this is the global object */
        if (!fo->strict && is_nullish(this_v) && global_obj) fr.this_v = v_obj(global_obj);
    }
    fr.strict = fo->strict;
    if (info->uses_arguments) {
        JsObj *a = obj_new_raw(CL_ARGUMENTS, ObjectProto);
        arr_reserve(a, argc);
        for (int i = 0; i < argc; i++) arr_push(a, args[i]);
        Prop *values = own_prop(ArrayProto, S_iterator); /* (arguments is iterable, like an array) */
        if (values) prop_set_raw(a, S_iterator, values->v, P_HIDDEN);
        env_define(env, S_arguments, v_obj(a), 0);
    }
    for (int i = 0; i < info->nvars; i++) env_define(env, info->vars[i], v_undef(), 0);
    int i = 0;
    for (Ast *p = fn->list; p; p = p->next, i++) {
        if (p->type == N_SPREAD) {
            JsObj *rest = new_array(argc > i ? argc - i : 0);
            for (int k = i; k < argc; k++) arr_push(rest, args[k]);
            TRY(bind_pattern(p->a, v_obj(rest), &fr, 'l'));
            break;
        }
        TRY(bind_pattern(p, i < argc ? args[i] : v_undef(), &fr, 'l'));
    }
    if (fn->flags & F_EXPR_BODY) return eval(fn->a, &fr, ret);
    Value v = v_undef();
    int r = exec_list(fn->a ? fn->a->list : NULL, &fr, &v);
    if (r == C_THROW) return r;
    *ret = r == C_RETURN ? v : v_undef();
    return C_NORMAL;
}

/* Too deep in the C stack to call anything more? (built-in functions can
   call each other in a cycle without passing through run_function) */
static int stack_exhausted(void) {
    char here;
    return js_stack_base && js_stack_base - &here > js_stack_room + 100000;
}

static int call_function(JsObj *fo, Value this_v, Value *args, int argc, Value *ret) {
    if (!fo || fo->cls != CL_FUNCTION) return throw_type("not a function", NULL, NULL);
    if (stack_exhausted()) {
        if (js_trace_throws) {
            Prop *p = own_prop(fo, S_name);
            printf("[deep] calling %s %s\n", fo->native ? "built-in" : "function",
                   p && p->v.t == V_STR ? p->v.u.s->s : fo->fn && fo->fn->str ? fo->fn->str->s : "(anonymous)");
        }
        return throw_error(RangeErrorProto, "Maximum call stack size exceeded");
    }
    if (fo->native) {
        JsObj *saved = native_new_target;
        native_new_target = NULL;
        TRY(check_limits());
        int r = fo->native(fo, this_v, args, argc, ret);
        native_new_target = saved;
        return r;
    }
    if (fo->target && !fo->fn) { /* a bound function */
        int total = fo->nbound + argc;
        Value small[16], *all = total <= 16 ? small : malloc(sizeof(Value) * (size_t)total);
        if (!all) return throw_error(RangeErrorProto, "out of memory");
        for (int i = 0; i < fo->nbound; i++) all[i] = fo->bound[i];
        for (int i = 0; i < argc; i++) all[fo->nbound + i] = args[i];
        if (all != small) gc_extra_add(all, sizeof(Value) * (size_t)total);
        int r = call_function(fo->target, fo->this_val, all, total, ret);
        free_args(all, small);
        return r;
    }
    if (fo->fn_kind == FN_CLASS || fo->fn_kind == FN_DERIVED) return throw_type("Class constructor cannot be invoked without 'new'", NULL, NULL);
    return run_function(fo, this_v, args, argc, NULL, ret);
}

/* ---------------------------------------------------------------- variables */

static Value *lookup(Env *e, JsStr *name, Env **where) {
    for (; e; e = e->parent) {
        if (e->object) {
            Prop *p = own_prop(e->object, name);
            if (p) {
                if (where) *where = e;
                return &p->v;
            }
        } else {
            Value *slot = env_slot(e, name);
            if (slot) {
                if (where) *where = e;
                return slot;
            }
        }
    }
    return NULL;
}

static int read_variable(JsStr *name, Frame *f, Value *out) {
    Env *where;
    Value *slot = lookup(f->env, name, &where);
    if (!slot) {
        if (name == S_undefined) {
            *out = v_undef();
            return C_NORMAL;
        }
        Buf m = {0};
        buf_add(&m, name->s, name->len);
        buf_c(&m, " is not defined");
        int r = throw_error(ReferenceErrorProto, m.p);
        free(m.p);
        return r;
    }
    if (where->object) {
        /* the global object: its properties may be accessors */
        Prop *p = own_prop(where->object, name);
        if (p && (p->flags & P_ACCESSOR)) return get_from(where->object, name, v_obj(where->object), out);
    }
    *out = *slot;
    return C_NORMAL;
}

static int write_variable(JsStr *name, Value v, Frame *f) {
    Env *where;
    Value *slot = lookup(f->env, name, &where);
    if (!slot) {
        env_define(global_env, name, v, 0); /* an undeclared variable becomes global */
        return C_NORMAL;
    }
    if (where->object) return put_value(v_obj(where->object), name, v);
    int index = (int)(slot - where->vals);
    if (where->consts[index]) return throw_type("Assignment to constant variable.", NULL, NULL);
    *slot = v;
    return C_NORMAL;
}

/* Give an anonymous function or class a name from where it is stored. */
static void name_function(Value v, JsStr *name) {
    if (v.t == V_OBJ && v.u.o->cls == CL_FUNCTION && v.u.o->fn && !v.u.o->fn->str && !own_prop(v.u.o, S_name))
        prop_set_raw(v.u.o, S_name, v_str(name), P_HIDDEN);
}

/* ---------------------------------------------------------------- iteration */

/* Collect the values an iterable yields (arrays, strings, Maps, Sets,
   array-likes such as NodeLists and arguments). */
static int iterate_to_array(Value v, JsObj **out) {
    JsObj *a = new_array(0);
    *out = a;
    if (stack_exhausted()) return throw_error(RangeErrorProto, "Maximum call stack size exceeded");
    if (v.t == V_STR) {
        const unsigned char *s = (const unsigned char *)v.u.s->s;
        for (int i = 0; i < v.u.s->len;) {
            int n = s[i] < 0x80 ? 1 : s[i] < 0xE0 ? 2 : s[i] < 0xF0 ? 3 : 4;
            if (i + n > v.u.s->len) n = v.u.s->len - i;
            arr_push(a, v_str(str_new((const char *)s + i, n)));
            i += n;
        }
        return C_NORMAL;
    }
    if (v.t != V_OBJ) return throw_type("value is not iterable", NULL, NULL);
    JsObj *o = v.u.o;
    if (o->cls == CL_ARRAY || o->cls == CL_ARGUMENTS) {
        arr_reserve(a, o->alen);
        for (int i = 0; i < o->alen; i++) arr_push(a, o->arr[i]);
        return C_NORMAL;
    }
    if (o->cls == CL_MAP || o->cls == CL_SET) {
        Value *entries = o->extra;
        for (int i = 0; i < o->extra_len; i++) {
            if (entries[2 * i].t == V_UNDEF && entries[2 * i + 1].t == V_NULL) continue; /* deleted */
            if (o->cls == CL_SET) {
                arr_push(a, entries[2 * i]);
            } else {
                JsObj *pair = new_array(2);
                arr_push(pair, entries[2 * i]);
                arr_push(pair, entries[2 * i + 1]);
                arr_push(a, v_obj(pair));
            }
        }
        return C_NORMAL;
    }
    /* an iterable: obj[Symbol.iterator]() gives an iterator */
    Value iter_fn;
    TRY(get_value(v, S_iterator, &iter_fn));
    if (is_callable(iter_fn) && o->cls != CL_NODE) {
        Value it;
        TRY(call_function(iter_fn.u.o, v, NULL, 0, &it));
        if (it.t == V_OBJ && it.u.o != o) return iterate_to_array(it, out);
        if (it.t == V_OBJ) v = it; /* it is its own iterator: fall through to next() */
        else return throw_type("Result of the Symbol.iterator method is not an object", NULL, NULL);
        Value next;
        TRY(get_value(v, intern_c("next"), &next));
        if (is_callable(next)) {
            for (int guard = 0; guard < 10000000; guard++) {
                Value r, done, value;
                TRY(call_function(next.u.o, v, NULL, 0, &r));
                TRY(get_value(r, S_done, &done));
                if (to_boolean(done)) break;
                TRY(get_value(r, S_value, &value));
                arr_push(a, value);
            }
            return C_NORMAL;
        }
    }
    /* array-like: anything with a length (NodeList, HTMLCollection, ...) */
    Value len;
    TRY(get_value(v, S_length, &len));
    if (len.t == V_NUM) {
        for (int i = 0; i < (int)len.u.n && i < 10000000; i++) {
            Value item;
            TRY(get_index(v, v_num(i), &item));
            arr_push(a, item);
        }
        return C_NORMAL;
    }
    /* an iterator protocol object: next() until done */
    Value next;
    TRY(get_value(v, intern_c("next"), &next));
    if (is_callable(next)) {
        for (int guard = 0; guard < 10000000; guard++) {
            Value r, done, value;
            TRY(call_function(next.u.o, v, NULL, 0, &r));
            TRY(get_value(r, S_done, &done));
            if (to_boolean(done)) break;
            TRY(get_value(r, S_value, &value));
            arr_push(a, value);
        }
        return C_NORMAL;
    }
    return throw_type("object is not iterable", NULL, NULL);
}

/* The enumerable keys of an object, own and inherited (for...in). */
static void enumerable_keys(JsObj *o, JsObj *out, int own_only) {
    for (JsObj *p = o; p; p = p->proto) {
        if (p->cls == CL_ARRAY || p->cls == CL_ARGUMENTS) {
            for (int i = 0; i < p->alen; i++) arr_push(out, v_str(number_str(i)));
        } else if (p->cls == CL_STRING && p->this_val.t == V_STR) {
            for (int i = 0; i < p->this_val.u.s->len; i++) arr_push(out, v_str(number_str(i)));
        }
        for (int i = 0; i < p->nprops; i++) {
            if ((p->props[i].flags & P_HIDDEN) || is_symbol_key(p->props[i].key)) continue;
            /* skip names already seen (shadowed) */
            int seen = 0;
            for (int k = 0; k < out->alen && !seen; k++) seen = out->arr[k].t == V_STR && out->arr[k].u.s == p->props[i].key;
            if (!seen) arr_push(out, v_str(p->props[i].key));
        }
        if (own_only) break;
    }
}

/* ---------------------------------------------------------------- patterns */

/* Bind or assign a target: mode 0 assigns, 'v' / 'l' / 'c' declares. */
static int bind_pattern(Ast *t, Value v, Frame *f, int mode) {
    switch (t->type) {
    case N_IDENT:
        if (mode == 'l' || mode == 'c') {
            env_define(f->env, t->str, v, mode == 'c');
            return C_NORMAL;
        }
        if (mode == 'v') {
            Value *slot = lookup(f->var_env, t->str, NULL);
            if (slot && f->var_env->object == NULL) {
                *slot = v;
                return C_NORMAL;
            }
        }
        return write_variable(t->str, v, f);
    case N_MEMBER: {
        Value obj;
        TRY(eval(t->a, f, &obj));
        return put_value(obj, t->str, v);
    }
    case N_INDEX: {
        Value obj, key;
        TRY(eval(t->a, f, &obj));
        TRY(eval(t->b, f, &key));
        return put_index(obj, key, v);
    }
    case N_ASSIGN: /* a default value */
        if (v.t == V_UNDEF) {
            TRY(eval(t->b, f, &v));
            if (t->a->type == N_IDENT) name_function(v, t->a->str);
        }
        return bind_pattern(t->a, v, f, mode);
    case N_ARRAY: {
        JsObj *items;
        TRY(iterate_to_array(v, &items));
        int i = 0;
        for (Ast *e = t->list; e; e = e->next, i++) {
            if (e->type == N_HOLE) continue;
            if (e->type == N_SPREAD) {
                JsObj *rest = new_array(0);
                for (int k = i; k < items->alen; k++) arr_push(rest, items->arr[k]);
                return bind_pattern(e->a, v_obj(rest), f, mode);
            }
            TRY(bind_pattern(e, i < items->alen ? items->arr[i] : v_undef(), f, mode));
        }
        return C_NORMAL;
    }
    case N_OBJECT: {
        if (is_nullish(v)) return throw_type("Cannot destructure ", v.t == V_NULL ? "null" : "undefined", NULL);
        JsStr *used[64];
        int nused = 0;
        for (Ast *p = t->list; p; p = p->next) {
            if (p->flags & F_SPREAD) {
                JsObj *rest = new_object();
                if (v.t == V_OBJ) {
                    JsObj *keys = new_array(0);
                    enumerable_keys(v.u.o, keys, 1);
                    for (int k = 0; k < keys->alen; k++) {
                        JsStr *key = interned(keys->arr[k].u.s);
                        int skip = 0;
                        for (int u = 0; u < nused && !skip; u++) skip = used[u] == key;
                        if (skip) continue;
                        Value item;
                        TRY(get_value(v, key, &item));
                        prop_set_raw(rest, key, item, 0);
                    }
                }
                TRY(bind_pattern(p->b, v_obj(rest), f, mode));
                continue;
            }
            JsStr *key = p->str;
            if (p->flags & F_COMPUTED) {
                Value k;
                TRY(eval(p->a, f, &k));
                TRY(to_key(k, &key));
            }
            if (nused < 64) used[nused++] = key;
            Value item;
            TRY(get_value(v, key, &item));
            TRY(bind_pattern(p->b, item, f, mode));
        }
        return C_NORMAL;
    }
    }
    return throw_error(SyntaxErrorProto, "Invalid destructuring target");
}

/* ---------------------------------------------------------------- classes */

static int init_fields(JsObj *cls, Value this_v) {
    Ast *c = cls->extra;
    if (!c) return C_NORMAL;
    Frame fr;
    memset(&fr, 0, sizeof(fr));
    fr.env = cls->scope;
    fr.var_env = cls->scope;
    fr.this_v = this_v;
    fr.home = cls->home;
    fr.func = cls;
    for (Ast *m = c->list; m; m = m->next) {
        if (!(m->flags & F_FIELD) || (m->flags & F_STATIC)) continue;
        JsStr *key = m->str;
        if ((m->flags & F_COMPUTED) && m->a) {
            Value k;
            TRY(eval(m->a, &fr, &k));
            TRY(to_key(k, &key));
        }
        Value v = v_undef();
        if (m->b) TRY(eval(m->b, &fr, &v));
        if (this_v.t == V_OBJ && key) prop_set_raw(this_v.u.o, key, v, 0);
    }
    return C_NORMAL;
}

static int eval_class(Ast *c, Frame *f, Value *out) {
    Value parent = v_undef();
    JsObj *proto_parent = ObjectProto, *ctor_parent = FunctionProto;
    if (c->a) {
        TRY(eval(c->a, f, &parent));
        if (parent.t == V_NULL) {
            proto_parent = NULL;
        } else {
            if (!is_callable(parent)) return throw_type("Class extends value is not a constructor", NULL, NULL);
            Value pp;
            TRY(get_value(parent, S_prototype, &pp));
            proto_parent = pp.t == V_OBJ ? pp.u.o : NULL;
            ctor_parent = parent.u.o;
        }
    }
    Env *scope = env_new(f->env);
    Ast *ctor_ast = NULL;
    for (Ast *m = c->list; m; m = m->next) {
        if (m->flags & F_CTOR) ctor_ast = m->b;
    }
    if (!ctor_ast) { /* a default constructor; a derived class's passes its arguments on: super(...arguments) */
        ctor_ast = arena_alloc(sizeof(Ast));
        ctor_ast->type = N_FUNC;
        ctor_ast->flags = F_METHOD;
        ctor_ast->a = arena_alloc(sizeof(Ast));
        ctor_ast->a->type = N_BLOCK;
        if (c->a) {
            Ast *stmt = arena_alloc(sizeof(Ast)), *call = arena_alloc(sizeof(Ast)), *sup = arena_alloc(sizeof(Ast));
            Ast *spread = arena_alloc(sizeof(Ast)), *args_id = arena_alloc(sizeof(Ast));
            stmt->type = N_EXPR;
            stmt->a = call;
            call->type = N_CALL;
            call->a = sup;
            sup->type = N_SUPER;
            call->list = spread;
            spread->type = N_SPREAD;
            spread->a = args_id;
            args_id->type = N_IDENT;
            args_id->str = S_arguments;
            ctor_ast->a->list = stmt;
        }
    }
    JsObj *cls = obj_new_raw(CL_FUNCTION, ctor_parent);
    cls->strict = 1;
    cls->fn = ctor_ast;
    cls->scope = scope;
    cls->fn_kind = c->a ? FN_DERIVED : FN_CLASS;
    cls->extra = c;
    if (c->str) prop_set_raw(cls, S_name, v_str(c->str), P_HIDDEN);
    JsObj *proto = obj_new_raw(CL_OBJECT, proto_parent);
    prop_set_raw(proto, S_constructor, v_obj(cls), P_HIDDEN);
    prop_set_raw(cls, S_prototype, v_obj(proto), P_HIDDEN | P_READONLY);
    cls->home = proto;
    if (c->str) env_define(scope, c->str, v_obj(cls), 1);
    Frame cf = *f;
    cf.env = scope;
    cf.strict = 1;
    for (Ast *m = c->list; m; m = m->next) {
        if (m->flags & (F_CTOR | F_FIELD)) continue;
        JsObj *target = (m->flags & F_STATIC) ? cls : proto;
        JsStr *key = m->str;
        if ((m->flags & F_COMPUTED) && m->a) {
            Value k;
            TRY(eval(m->a, &cf, &k));
            TRY(to_key(k, &key));
        }
        JsObj *method = make_closure(m->b, &cf);
        method->home = target;
        method->fn_kind = FN_METHOD;
        if (m->flags & (F_GETTER | F_SETTER)) {
            Prop *p = prop_add(target, key);
            if (!(p->flags & P_ACCESSOR)) p->get = p->set = NULL;
            p->flags = P_ACCESSOR | P_HIDDEN;
            if (m->flags & F_GETTER) p->get = method;
            else p->set = method;
        } else {
            prop_set_raw(target, key, v_obj(method), P_HIDDEN);
        }
    }
    /* static fields and blocks, with this = the class */
    Frame sf = cf;
    sf.this_v = v_obj(cls);
    sf.home = cls;
    for (Ast *m = c->list; m; m = m->next) {
        if (!(m->flags & F_FIELD) || !(m->flags & F_STATIC)) continue;
        JsStr *key = m->str;
        if ((m->flags & F_COMPUTED) && m->a && m->a->type != N_STR) {
            Value k;
            TRY(eval(m->a, &sf, &k));
            TRY(to_key(k, &key));
        }
        Value v = v_undef();
        if (m->b) TRY(eval(m->b, &sf, &v));
        if (key && key->len) prop_set_raw(cls, key, v, 0);
    }
    *out = v_obj(cls);
    return C_NORMAL;
}

/* new F(args) */
static int construct(JsObj *fo, Value *args, int argc, JsObj *new_target, Value *out) {
    if (!fo || fo->cls != CL_FUNCTION || fo->fn_kind == FN_ARROW || fo->fn_kind == FN_METHOD)
        return throw_type("not a constructor", NULL, NULL);
    if (stack_exhausted()) return throw_error(RangeErrorProto, "Maximum call stack size exceeded");
    if (fo->target && !fo->fn && !fo->native) return construct(fo->target, args, argc, new_target, out);
    Value proto_v;
    TRY(get_value(v_obj(new_target), S_prototype, &proto_v));
    JsObj *proto = proto_v.t == V_OBJ ? proto_v.u.o : ObjectProto;
    if (fo->native) {
        JsObj *obj = obj_new_raw(CL_OBJECT, proto);
        JsObj *saved = native_new_target;
        native_new_target = new_target;
        Value r;
        int code = fo->native(fo, v_obj(obj), args, argc, &r);
        native_new_target = saved;
        if (code != C_NORMAL) return code;
        if (r.t == V_OBJ) {
            /* a built-in made its own object (Array, Date, Map...): give it the subclass's prototype */
            if (new_target != fo && proto) r.u.o->proto = proto;
            *out = r;
        } else {
            *out = v_obj(obj);
        }
        return C_NORMAL;
    }
    Value this_v = v_obj(obj_new_raw(CL_OBJECT, proto));
    if (fo->fn_kind == FN_CLASS) TRY(init_fields(fo, this_v));
    Value r;
    TRY(run_function(fo, this_v, args, argc, new_target, &r));
    *out = r.t == V_OBJ ? r : this_v;
    return C_NORMAL;
}

/* super(...) inside a derived class's constructor. */
static int call_super(Frame *f, Value *args, int argc) {
    JsObj *cls = f->func;
    while (cls && cls->fn_kind == FN_ARROW) cls = NULL; /* arrows carry their frame's func */
    if (!f->func) return throw_error(SyntaxErrorProto, "'super' keyword unexpected here");
    cls = f->func;
    JsObj *parent = cls->proto;
    if (!parent || parent->cls != CL_FUNCTION) return throw_type("Super constructor is not a constructor", NULL, NULL);
    if (parent->native) {
        JsObj *saved = native_new_target;
        native_new_target = f->new_target ? f->new_target : cls;
        Value r;
        int code = parent->native(parent, f->this_v, args, argc, &r);
        native_new_target = saved;
        if (code != C_NORMAL) return code;
        /* a built-in that made its own object (Array, Error...): copy what it set up */
        if (r.t == V_OBJ && f->this_v.t == V_OBJ && r.u.o != f->this_v.u.o) {
            JsObj *made = r.u.o, *self = f->this_v.u.o;
            self->cls = made->cls;
            for (int i = 0; i < made->nprops; i++) prop_set_raw(self, made->props[i].key, made->props[i].v, made->props[i].flags);
            self->this_val = made->this_val;
            self->extra = made->extra;
            self->extra_len = made->extra_len;
            self->extra_cap = made->extra_cap;
            made->extra = NULL;
        }
    } else {
        if (parent->fn_kind == FN_CLASS) TRY(init_fields(parent, f->this_v));
        Value ignored;
        Frame saved = *f;
        TRY(run_function(parent, f->this_v, args, argc, f->new_target, &ignored));
        *f = saved;
    }
    return init_fields(cls, f->this_v);
}

/* ---------------------------------------------------------------- expressions */

static int eval_args(Ast *list, Frame *f, Value **out, int *count, Value *small, int small_n) {
    int n = 0, cap = small_n;
    Value *args = small;
    for (Ast *a = list; a; a = a->next) {
        if (a->type == N_SPREAD) {
            Value v;
            TRY(eval(a->a, f, &v));
            JsObj *items;
            TRY(iterate_to_array(v, &items));
            for (int i = 0; i < items->alen; i++) {
                if (n == cap) {
                    cap *= 2;
                    Value *bigger = malloc(sizeof(Value) * (size_t)cap);
                    if (!bigger) {
                        free_args(args, small);
                        return throw_error(RangeErrorProto, "out of memory");
                    }
                    memset(bigger, 0, sizeof(Value) * (size_t)cap);
                    memcpy(bigger, args, sizeof(Value) * (size_t)n);
                    free_args(args, small);
                    gc_extra_add(bigger, sizeof(Value) * (size_t)cap);
                    args = bigger;
                }
                args[n++] = items->arr[i];
            }
            continue;
        }
        if (n == cap) {
            cap *= 2;
            Value *bigger = malloc(sizeof(Value) * (size_t)cap);
            if (!bigger) {
                free_args(args, small);
                return throw_error(RangeErrorProto, "out of memory");
            }
            memset(bigger, 0, sizeof(Value) * (size_t)cap);
            memcpy(bigger, args, sizeof(Value) * (size_t)n);
            free_args(args, small);
            gc_extra_add(bigger, sizeof(Value) * (size_t)cap);
            args = bigger;
        }
        int r = eval(a, f, &args[n]);
        if (r != C_NORMAL) {
            free_args(args, small);
            return r;
        }
        n++;
    }
    *out = args;
    *count = n;
    return C_NORMAL;
}

/* ", line 12" for an error message */
static void add_line(Buf *b, int line) {
    if (line <= 0) return;
    char t[16];
    number_to_text(line, t);
    buf_c(b, ", line ");
    buf_c(b, t);
}

/* Source-like text for the thing being called, for error messages:
   "a.b.c", "x[...]", "(0, r.default)" -> "r.default", "f(...)". */
static void describe_callee(Ast *n, Buf *b, int depth) {
    if (!n || depth > 6 || b->len > 80) {
        buf_c(b, "...");
        return;
    }
    switch (n->type) {
    case N_IDENT: buf_add(b, n->str->s, n->str->len); return;
    case N_THIS: buf_c(b, "this"); return;
    case N_MEMBER:
        describe_callee(n->a, b, depth + 1);
        buf_ch(b, '.');
        buf_add(b, n->str->s, n->str->len);
        return;
    case N_INDEX:
        describe_callee(n->a, b, depth + 1);
        if (n->b && n->b->type == N_STR) {
            buf_c(b, "[\"");
            buf_add(b, n->b->str->s, n->b->str->len < 40 ? n->b->str->len : 40);
            buf_c(b, "\"]");
        } else {
            buf_c(b, "[...]");
        }
        return;
    case N_SEQ: describe_callee(n->b, b, depth + 1); return;
    case N_CALL:
        describe_callee(n->a, b, depth + 1);
        buf_c(b, "(...)");
        return;
    case N_FUNC: buf_c(b, "a function expression"); return;
    default: buf_c(b, "expression"); return;
    }
}

/* BigInt op BigInt */
static int big_binary(int op, Value a, Value b, Value *out) {
    if (a.t != V_BIG || b.t != V_BIG) return throw_type("Cannot mix BigInt and other types, use explicit conversions", NULL, NULL);
    if (op == P3('>', '>', '>')) return throw_type("BigInts have no unsigned right shift, use >> instead", NULL, NULL);
    const char *error = NULL;
    JsStr *r = big_arith(op, a.u.s, b.u.s, &error);
    if (!r) return throw_error(RangeErrorProto, error ? error : "BigInt operation failed");
    *out = v_big(r);
    return C_NORMAL;
}

static int arith(int op, Value a, Value b, Value *out) {
    if (a.t == V_NUM && b.t == V_NUM) { /* (the common case, quickly) */
        double x = a.u.n, y = b.u.n;
        switch (op) {
        case '+': *out = v_num(x + y); return C_NORMAL;
        case '-': *out = v_num(x - y); return C_NORMAL;
        case '*': *out = v_num(x * y); return C_NORMAL;
        }
    }
    if (op == '+') {
        Value pa, pb;
        TRY(to_primitive(a, 'n', &pa));
        TRY(to_primitive(b, 'n', &pb));
        if (pa.t == V_STR || pb.t == V_STR) {
            JsStr *sa, *sb;
            TRY(to_string(pa, &sa));
            TRY(to_string(pb, &sb));
            *out = v_str(str_concat(sa, sb));
            return C_NORMAL;
        }
        if (pa.t == V_BIG || pb.t == V_BIG) return big_binary(op, pa, pb, out);
        double x, y;
        TRY(to_number(pa, &x));
        TRY(to_number(pb, &y));
        *out = v_num(x + y);
        return C_NORMAL;
    }
    if (a.t == V_OBJ) TRY(to_primitive(a, 'n', &a));
    if (b.t == V_OBJ) TRY(to_primitive(b, 'n', &b));
    if (a.t == V_BIG || b.t == V_BIG) return big_binary(op, a, b, out);
    double x, y;
    TRY(to_number(a, &x));
    TRY(to_number(b, &y));
    switch (op) {
    case '-': *out = v_num(x - y); break;
    case '*': *out = v_num(x * y); break;
    case '/': *out = v_num(x / y); break;
    case '%': *out = v_num(js_fmod(x, y)); break;
    case P2('*', '*'): *out = v_num(js_pow(x, y)); break;
    case '&': *out = v_num(to_int32(x) & to_int32(y)); break;
    case '|': *out = v_num(to_int32(x) | to_int32(y)); break;
    case '^': *out = v_num(to_int32(x) ^ to_int32(y)); break;
    case P2('<', '<'): *out = v_num((int)((unsigned)to_int32(x) << (to_uint32(y) & 31))); break;
    case P2('>', '>'): *out = v_num(to_int32(x) >> (to_uint32(y) & 31)); break;
    case P3('>', '>', '>'): *out = v_num(to_uint32(x) >> (to_uint32(y) & 31)); break;
    default: *out = v_undef();
    }
    return C_NORMAL;
}

/* a < b: 1, 0, or -1 for undefined (NaN involved) */
static int less_than(Value a, Value b, int *result) {
    Value pa, pb;
    TRY(to_primitive(a, 'n', &pa));
    TRY(to_primitive(b, 'n', &pb));
    if (pa.t == V_STR && pb.t == V_STR) {
        *result = str_cmp(pa.u.s, pb.u.s) < 0;
        return C_NORMAL;
    }
    if (pa.t == V_BIG || pb.t == V_BIG) {
        for (int i = 0; i < 2; i++) { /* a string side is read as a BigInt */
            Value *p = i ? &pb : &pa;
            if (p->t == V_STR) {
                Big t = {0};
                if (!big_from_text(&t, p->u.s->s, p->u.s->len, 0)) {
                    big_free(&t);
                    *result = -1;
                    return C_NORMAL;
                }
                *p = v_big(big_store(&t));
            }
        }
        if (pa.t == V_BIG && pb.t == V_BIG) {
            *result = big_compare(pa.u.s, pb.u.s) < 0;
            return C_NORMAL;
        }
        double d;
        if (pa.t == V_BIG) {
            TRY(to_number(pb, &d));
            int c = big_compare_number(pa.u.s, d);
            *result = c == 2 ? -1 : c < 0;
        } else {
            TRY(to_number(pa, &d));
            int c = big_compare_number(pb.u.s, d);
            *result = c == 2 ? -1 : c > 0;
        }
        return C_NORMAL;
    }
    double x, y;
    TRY(to_number(pa, &x));
    TRY(to_number(pb, &y));
    *result = is_nan(x) || is_nan(y) ? -1 : x < y;
    return C_NORMAL;
}

static int instance_of(Value v, Value ctor, int *result) {
    if (!is_callable(ctor)) return throw_type("Right-hand side of 'instanceof' is not callable", NULL, NULL);
    *result = 0;
    if (v.t != V_OBJ) return C_NORMAL;
    JsObj *c = ctor.u.o;
    while (c->target && !c->fn && !c->native) c = c->target; /* bound */
    Value proto;
    TRY(get_value(v_obj(c), S_prototype, &proto));
    if (proto.t != V_OBJ) return C_NORMAL;
    for (JsObj *p = v.u.o->proto; p; p = p->proto) {
        if (p == proto.u.o) {
            *result = 1;
            return C_NORMAL;
        }
    }
    return C_NORMAL;
}

static int binary(int op, Value a, Value b, Value *out) {
    int r;
    switch (op) {
    case P3('=', '=', '='): *out = v_bool(strict_equals(a, b)); return C_NORMAL;
    case P3('!', '=', '='): *out = v_bool(!strict_equals(a, b)); return C_NORMAL;
    case P2('=', '='): TRY(loose_equals(a, b, &r)); *out = v_bool(r); return C_NORMAL;
    case P2('!', '='): TRY(loose_equals(a, b, &r)); *out = v_bool(!r); return C_NORMAL;
    case '<': TRY(less_than(a, b, &r)); *out = v_bool(r == 1); return C_NORMAL;
    case '>': TRY(less_than(b, a, &r)); *out = v_bool(r == 1); return C_NORMAL;
    case P2('<', '='): TRY(less_than(b, a, &r)); *out = v_bool(r == 0); return C_NORMAL;
    case P2('>', '='): TRY(less_than(a, b, &r)); *out = v_bool(r == 0); return C_NORMAL;
    case P2('i', 'o'): TRY(instance_of(a, b, &r)); *out = v_bool(r); return C_NORMAL;
    case P2('i', 'n'): {
        if (b.t != V_OBJ) return throw_type("Cannot use 'in' operator to search for a key in a primitive", NULL, NULL);
        JsStr *k;
        TRY(to_key(a, &k));
        *out = v_bool(has_property(b.u.o, k));
        return C_NORMAL;
    }
    }
    return arith(op, a, b, out);
}

/* Read the reference a target names (for ++, +=, ...). */
static int read_target(Ast *t, Frame *f, Value *base, Value *key, Value *out) {
    if (t->type == N_IDENT) return read_variable(t->str, f, out);
    TRY(eval(t->a, f, base));
    if (t->type == N_MEMBER) {
        *key = v_str(t->str);
        return get_value(*base, t->str, out);
    }
    TRY(eval(t->b, f, key));
    return get_index(*base, *key, out);
}

static int write_target(Ast *t, Frame *f, Value base, Value key, Value v) {
    if (t->type == N_IDENT) return write_variable(t->str, v, f);
    if (t->type == N_MEMBER) return put_value(base, t->str, v);
    return put_index(base, key, v);
}

static int eval_object(Ast *n, Frame *f, Value *out) {
    JsObj *o = new_object();
    for (Ast *p = n->list; p; p = p->next) {
        if (p->flags & F_SPREAD) {
            Value v;
            TRY(eval(p->b, f, &v));
            if (v.t == V_OBJ) {
                JsObj *keys = new_array(0);
                enumerable_keys(v.u.o, keys, 1);
                for (int i = 0; i < keys->alen; i++) {
                    JsStr *k = interned(keys->arr[i].u.s);
                    Value item;
                    TRY(get_value(v, k, &item));
                    prop_set_raw(o, k, item, 0);
                }
            } else if (v.t == V_STR) {
                for (int i = 0; i < v.u.s->len; i++) prop_set_raw(o, intern_c(number_str(i)->s), v_str(str_new(v.u.s->s + i, 1)), 0);
            }
            continue;
        }
        JsStr *key = p->str;
        if (p->flags & F_COMPUTED) {
            Value k;
            TRY(eval(p->a, f, &k));
            TRY(to_key(k, &key));
        }
        if (p->flags & F_METHOD) {
            JsObj *m = make_closure(p->b, f);
            m->home = o;
            m->fn_kind = FN_METHOD;
            if (p->flags & (F_GETTER | F_SETTER)) {
                Prop *pr = prop_add(o, key);
                if (!(pr->flags & P_ACCESSOR)) pr->get = pr->set = NULL;
                pr->flags = P_ACCESSOR;
                if (p->flags & F_GETTER) pr->get = m;
                else pr->set = m;
            } else {
                if (p->flags & F_COMPUTED) prop_set_raw(m, S_name, v_str(key), P_HIDDEN);
                prop_set_raw(o, key, v_obj(m), 0);
            }
            continue;
        }
        Value v;
        TRY(eval(p->b, f, &v));
        name_function(v, key);
        if (key == intern_c("__proto__") && !(p->flags & F_COMPUTED)) {
            if (v.t == V_OBJ) o->proto = v.u.o;
            else if (v.t == V_NULL) o->proto = NULL;
            continue;
        }
        prop_set_raw(o, key, v, 0);
    }
    *out = v_obj(o);
    return C_NORMAL;
}

static int eval_call(Ast *n, Frame *f, Value *out) {
    Ast *callee = n->a;
    Value fn, this_v = v_undef(), called_key = v_undef();
    if (callee->type == N_SUPER) {
        Value small[8], *args;
        int argc;
        TRY(eval_args(n->list, f, &args, &argc, small, 8));
        int r = call_super(f, args, argc);
        free_args(args, small);
        *out = v_undef();
        return r;
    }
    if (callee->type == N_MEMBER || callee->type == N_INDEX) {
        if (callee->a->type == N_SUPER) {
            this_v = f->this_v;
            JsObj *base = f->home ? f->home->proto : NULL;
            JsStr *key = callee->str;
            if (callee->type == N_INDEX) {
                Value k;
                TRY(eval(callee->b, f, &k));
                TRY(to_key(k, &key));
            }
            if (!base) return throw_error(SyntaxErrorProto, "'super' keyword unexpected here");
            TRY(get_from(base, key, this_v, &fn));
        } else {
            TRY(eval(callee->a, f, &this_v));
            if (optional_short) {
                if (callee->flags & F_CHAIN) {
                    *out = v_undef();
                    return C_NORMAL;
                }
                optional_short = 0;
            }
            if ((callee->flags & F_OPTIONAL) && is_nullish(this_v)) {
                optional_short = 1;
                *out = v_undef();
                return C_NORMAL;
            }
            if (is_nullish(this_v)) { /* a.b.c() with a.b undefined: say which */
                Buf m = {0};
                buf_c(&m, this_v.t == V_NULL ? "null (calling '" : "undefined (calling '");
                if (callee->type == N_MEMBER) buf_add(&m, callee->str->s, callee->str->len < 60 ? callee->str->len : 60);
                else buf_c(&m, "[...]");
                buf_c(&m, "' of ");
                describe_callee(callee->a, &m, 0);
                buf_ch(&m, ')');
                add_line(&m, n->line);
                int r = throw_type("Cannot read properties of ", m.p, NULL);
                free(m.p);
                return r;
            }
            if (callee->type == N_MEMBER) {
                TRY(get_value(this_v, callee->str, &fn));
            } else {
                Value k;
                TRY(eval(callee->b, f, &k));
                called_key = k;
                TRY(get_index(this_v, k, &fn));
            }
        }
    } else {
        TRY(eval(callee, f, &fn));
        if (optional_short) {
            if (n->flags & F_CHAIN) {
                *out = v_undef();
                return C_NORMAL;
            }
            optional_short = 0;
        }
    }
    if ((n->flags & F_OPTIONAL) && is_nullish(fn)) {
        optional_short = 1;
        *out = v_undef();
        return C_NORMAL;
    }
    if (!is_callable(fn)) {
        Buf what = {0};
        describe_callee(callee, &what, 0);
        buf_c(&what, " is not a function");
        if (fn.t == V_UNDEF || fn.t == V_NULL) buf_c(&what, fn.t == V_UNDEF ? " (it is undefined)" : " (it is null)");
        if (called_key.t == V_STR && called_key.u.s->len < 60) { /* x[k](): which k */
            buf_c(&what, " (calling '");
            buf_add(&what, called_key.u.s->s, called_key.u.s->len);
            buf_c(&what, "'");
            if (js_trace_throws && this_v.t == V_OBJ) {
                Prop *cp = NULL;
                for (JsObj *p = this_v.u.o; p && !cp; p = p->proto) cp = own_prop(p, S_constructor);
                Prop *np = cp && cp->v.t == V_OBJ ? own_prop(cp->v.u.o, S_name) : NULL;
                if (np && np->v.t == V_STR) {
                    buf_c(&what, " on a ");
                    buf_add(&what, np->v.u.s->s, np->v.u.s->len);
                }
            }
            buf_ch(&what, ')');
        }
        add_line(&what, n->line);
        if (n->cache_len > 0) {
            char t[16];
            number_to_text(n->cache_len - 1, t);
            buf_c(&what, ", character ");
            buf_c(&what, t);
        }
        int r = throw_type(what.p, NULL, NULL);
        free(what.p);
        return r;
    }
    Value small[8], *args;
    int argc;
    TRY(eval_args(n->list, f, &args, &argc, small, 8));
    js_call_pos = n->cache_len - 1;
    if (fn.u.o->native) { /* (for built-ins' error messages: where they were called from) */
        native_call_line = n->line;
        native_call_pos = n->cache_len - 1;
    }
    int r = call_function(fn.u.o, this_v, args, argc, out);
    free_args(args, small);
    return r;
}

/* (Cases of eval with big locals live in functions of their own: eval's
   own stack frame is paid for at every level of every expression.) */

/* a, b, c, ...: a left-leaning chain, walked without recursion */
__attribute__((noinline)) static int eval_seq(Ast *n, Frame *f, Value *out) {
    Ast *chain[256];
    int k = 0;
    Ast *x = n;
    while (x->type == N_SEQ && k < 256) {
        chain[k++] = x;
        x = x->a;
    }
    Value ignored;
    TRY(eval(x, f, &ignored));
    for (int i = k - 1; i > 0; i--) TRY(eval(chain[i]->b, f, &ignored));
    return eval(chain[0]->b, f, out);
}

__attribute__((noinline)) static int eval_tagged(Ast *n, Frame *f, Value *out) {
    Value tag;
    TRY(eval(n->a, f, &tag));
    JsObj *strings = new_array(0);
    Value args[9];
    int argc = 1;
    int i = 0;
    for (Ast *p = n->b->list; p; p = p->next, i++) {
        if (i % 2 == 0) arr_push(strings, v_str(p->str));
        else if (argc < 9) TRY(eval(p, f, &args[argc++]));
    }
    prop_set_raw(strings, intern_c("raw"), v_obj(strings), P_HIDDEN);
    args[0] = v_obj(strings);
    if (!is_callable(tag)) return throw_type("tag is not a function", NULL, NULL);
    return call_function(tag.u.o, v_undef(), args, argc, out);
}

__attribute__((noinline)) static int eval_regex(Ast *n, Value *out) {
    char error[120];
    JsObj *re = regexp_new(n->str, n->a->str, error);
    if (!re) return throw_error(SyntaxErrorProto, error);
    *out = v_obj(re);
    return C_NORMAL;
}

static int eval(Ast *n, Frame *f, Value *out) {
    char here;
    if (js_stack_base - &here > js_stack_room + 300000) return throw_error(RangeErrorProto, "Maximum call stack size exceeded (expression too deeply nested)");
    switch (n->type) {
    case N_NUM: *out = n->op == 'n' ? v_big(n->str) : v_num(n->num); return C_NORMAL;
    case N_STR: *out = v_str(n->str); return C_NORMAL;
    case N_IDENT:
        if (n->op == 'k') {
            *out = n->str == S_null ? v_null() : v_bool(n->str == S_true);
            return C_NORMAL;
        }
        return read_variable(n->str, f, out);
    case N_THIS: *out = f->this_v; return C_NORMAL;
    case N_NEWTARGET: *out = f->new_target ? v_obj(f->new_target) : v_undef(); return C_NORMAL;
    case N_YIELD: return coro_yield(n, f, out);
    case N_TEMPLATE: {
        Buf b = {0};
        int i = 0;
        for (Ast *p = n->list; p; p = p->next, i++) {
            Value v;
            if (i % 2 == 0) {
                buf_add(&b, p->str->s, p->str->len);
                continue;
            }
            int r = eval(p, f, &v);
            JsStr *s;
            if (r == C_NORMAL) r = to_string(v, &s);
            if (r != C_NORMAL) {
                free(b.p);
                return r;
            }
            buf_add(&b, s->s, s->len);
        }
        *out = v_str(buf_done(&b));
        return C_NORMAL;
    }
    case N_TAGGED: return eval_tagged(n, f, out);
    case N_REGEX: return eval_regex(n, out);
    case N_ARRAY: {
        JsObj *a = new_array(0);
        for (Ast *e = n->list; e; e = e->next) {
            Value v;
            if (e->type == N_HOLE) {
                arr_push(a, v_undef());
            } else if (e->type == N_SPREAD) {
                TRY(eval(e->a, f, &v));
                JsObj *items;
                TRY(iterate_to_array(v, &items));
                for (int i = 0; i < items->alen; i++) arr_push(a, items->arr[i]);
            } else {
                TRY(eval(e, f, &v));
                arr_push(a, v);
            }
        }
        *out = v_obj(a);
        return C_NORMAL;
    }
    case N_OBJECT: return eval_object(n, f, out);
    case N_FUNC: {
        if (n->str && !(n->flags & (F_ARROW | F_METHOD))) {
            /* function name() {...} as an expression: inside, `name` is the
               function itself, in a scope of its own (it shadows any outer
               `name`, and is not visible outside) */
            Env *named = env_new(f->env);
            Frame inner = *f;
            inner.env = named;
            JsObj *fn = make_closure(n, &inner);
            env_define(named, n->str, v_obj(fn), 0);
            *out = v_obj(fn);
            return C_NORMAL;
        }
        JsObj *fn = make_closure(n, f);
        *out = v_obj(fn);
        return C_NORMAL;
    }
    case N_CLASS: return eval_class(n, f, out);
    case N_MEMBER:
    case N_INDEX: {
        Value obj;
        if (n->a->type == N_SUPER) {
            JsObj *base = f->home ? f->home->proto : NULL;
            if (!base) return throw_error(SyntaxErrorProto, "'super' keyword unexpected here");
            JsStr *key = n->str;
            if (n->type == N_INDEX) {
                Value k;
                TRY(eval(n->b, f, &k));
                TRY(to_key(k, &key));
            }
            return get_from(base, key, f->this_v, out);
        }
        TRY(eval(n->a, f, &obj));
        if (optional_short) {
            if (n->flags & F_CHAIN) {
                *out = v_undef();
                return C_NORMAL;
            }
            optional_short = 0;
        }
        if ((n->flags & F_OPTIONAL) && is_nullish(obj)) {
            optional_short = 1;
            *out = v_undef();
            return C_NORMAL;
        }
        if (n->type == N_MEMBER) {
            if (is_nullish(obj)) { /* "Cannot read properties of undefined (reading 'x' of a.b)" */
                Buf m = {0};
                buf_c(&m, obj.t == V_NULL ? "null (reading '" : "undefined (reading '");
                buf_add(&m, n->str->s, n->str->len < 60 ? n->str->len : 60);
                buf_c(&m, "' of ");
                describe_callee(n->a, &m, 0);
                buf_ch(&m, ')');
                add_line(&m, n->line);
                int r = throw_type("Cannot read properties of ", m.p, NULL);
                free(m.p);
                return r;
            }
            return get_value(obj, n->str, out);
        }
        Value key;
        TRY(eval(n->b, f, &key));
        if (is_nullish(obj)) { /* "Cannot read properties of undefined (reading 'x' of a.b)" */
            JsStr *k;
            if (key.t == V_SYM) k = key.u.s;
            else if (to_string(key, &k) != C_NORMAL) k = S_empty;
            Buf m = {0};
            buf_c(&m, obj.t == V_NULL ? "null (reading '" : "undefined (reading '");
            buf_add(&m, k->s, k->len < 60 ? k->len : 60);
            buf_c(&m, "' of ");
            describe_callee(n->a, &m, 0);
            buf_ch(&m, ')');
            add_line(&m, n->line);
            int r = throw_type("Cannot read properties of ", m.p, NULL);
            free(m.p);
            return r;
        }
        return get_index(obj, key, out);
    }
    case N_CALL: return eval_call(n, f, out);
    case N_NEW: {
        Value fn;
        TRY(eval(n->a, f, &fn));
        if (!is_callable(fn)) {
            const char *what = n->a->type == N_IDENT || n->a->type == N_MEMBER ? n->a->str->s : "expression";
            return throw_type(what, " is not a constructor", NULL);
        }
        Value small[8], *args;
        int argc;
        TRY(eval_args(n->list, f, &args, &argc, small, 8));
        int r = construct(fn.u.o, args, argc, fn.u.o, out);
        free_args(args, small);
        return r;
    }
    case N_UNARY: {
        if (n->op == 't' && n->a->type == N_IDENT && n->a->op != 'k' && !lookup(f->env, n->a->str, NULL)) {
            if (missing_on) note_missing(global_obj, n->a->str, "typeof");
            *out = v_str(S_undefined); /* typeof an undeclared name */
            return C_NORMAL;
        }
        if (n->op == 'd') {
            Ast *t = n->a;
            if (t->type == N_MEMBER || t->type == N_INDEX) {
                Value obj;
                TRY(eval(t->a, f, &obj));
                JsStr *key = t->str;
                if (t->type == N_INDEX) {
                    Value k;
                    TRY(eval(t->b, f, &k));
                    TRY(to_key(k, &key));
                }
                if (obj.t == V_OBJ) {
                    JsObj *o = obj.u.o;
                    if ((o->cls == CL_ARRAY || o->cls == CL_ARGUMENTS) && key->index >= 0 && key->index < o->alen)
                        o->arr[key->index] = v_undef();
                    else if (!o->frozen)
                        prop_delete(o, key);
                }
            }
            *out = v_bool(1);
            return C_NORMAL;
        }
        Value v;
        TRY(eval(n->a, f, &v));
        double d;
        switch (n->op) {
        case '!': *out = v_bool(!to_boolean(v)); return C_NORMAL;
        case '-':
            if (v.t == V_OBJ) TRY(to_primitive(v, 'n', &v));
            if (v.t == V_BIG) {
                *out = v_big(big_negate(v.u.s));
                return C_NORMAL;
            }
            TRY(to_number(v, &d));
            *out = v_num(-d);
            return C_NORMAL;
        case '+': TRY(to_number(v, &d)); *out = v_num(d); return C_NORMAL;
        case '~':
            if (v.t == V_OBJ) TRY(to_primitive(v, 'n', &v));
            if (v.t == V_BIG) {
                *out = v_big(big_not(v.u.s));
                return C_NORMAL;
            }
            TRY(to_number(v, &d));
            *out = v_num(~to_int32(d));
            return C_NORMAL;
        case 't': *out = v_str(type_of(v)); return C_NORMAL;
        case 'v': *out = v_undef(); return C_NORMAL;
        case 'a': /* await: in an async function, waits for the promise to settle */
            return coro_await(v, out);
        }
        *out = v_undef();
        return C_NORMAL;
    }
    case N_UPDATE: {
        Value base = v_undef(), key = v_undef(), old;
        TRY(read_target(n->a, f, &base, &key, &old));
        if (old.t == V_OBJ) TRY(to_primitive(old, 'n', &old));
        if (old.t == V_BIG) {
            const char *error = NULL;
            Value nv = v_big(big_arith(n->op == P2('+', '+') ? '+' : '-', old.u.s, big_small(1), &error));
            TRY(write_target(n->a, f, base, key, nv));
            *out = (n->flags & F_PREFIX) ? nv : old;
            return C_NORMAL;
        }
        double d;
        TRY(to_number(old, &d));
        double nd = n->op == P2('+', '+') ? d + 1 : d - 1;
        TRY(write_target(n->a, f, base, key, v_num(nd)));
        *out = v_num((n->flags & F_PREFIX) ? nd : d);
        return C_NORMAL;
    }
    case N_BINARY: {
        Value a, b;
        TRY(eval(n->a, f, &a));
        TRY(eval(n->b, f, &b));
        return binary(n->op, a, b, out);
    }
    case N_LOGICAL: {
        Value a;
        TRY(eval(n->a, f, &a));
        if (n->op == P2('&', '&') ? !to_boolean(a) : n->op == P2('|', '|') ? to_boolean(a) : !is_nullish(a)) {
            *out = a;
            return C_NORMAL;
        }
        return eval(n->b, f, out);
    }
    case N_COND: {
        Value t;
        TRY(eval(n->a, f, &t));
        return eval(to_boolean(t) ? n->b : n->c, f, out);
    }
    case N_SEQ: return eval_seq(n, f, out);
    case N_ASSIGN: {
        Ast *t = n->a;
        if (n->op == '=') {
            Value v;
            if (t->type == N_OBJECT || t->type == N_ARRAY) {
                TRY(eval(n->b, f, &v));
                TRY(bind_pattern(t, v, f, 0));
                *out = v;
                return C_NORMAL;
            }
            Value base = v_undef(), key = v_undef();
            if (t->type == N_MEMBER || t->type == N_INDEX) {
                TRY(eval(t->a, f, &base));
                if (t->type == N_INDEX) TRY(eval(t->b, f, &key));
            }
            TRY(eval(n->b, f, &v));
            if (t->type == N_IDENT) name_function(v, t->str);
            TRY(write_target(t, f, base, key, v));
            *out = v;
            return C_NORMAL;
        }
        Value base = v_undef(), key = v_undef(), old, v;
        TRY(read_target(t, f, &base, &key, &old));
        if (n->op == P3('&', '&', '=') || n->op == P3('|', '|', '=') || n->op == P3('?', '?', '=')) {
            int keep = n->op == P3('&', '&', '=') ? !to_boolean(old) : n->op == P3('|', '|', '=') ? to_boolean(old) : !is_nullish(old);
            if (keep) {
                *out = old;
                return C_NORMAL;
            }
            TRY(eval(n->b, f, &v));
        } else {
            Value rhs;
            TRY(eval(n->b, f, &rhs));
            int op = n->op & 0xFFFFFF;
            /* "+=" -> '+', "**=" -> "**", ">>>=" -> ">>>" */
            if ((op & 0xFF00) == ('=' << 8)) op &= 0xFF;
            else if ((op & 0xFF0000) == ('=' << 16)) op &= 0xFFFF;
            else op = n->op & 0xFFFFFF;
            TRY(arith(op, old, rhs, &v));
        }
        TRY(write_target(t, f, base, key, v));
        *out = v;
        return C_NORMAL;
    }
    case N_SPREAD: return throw_error(SyntaxErrorProto, "unexpected ...");
    case N_SUPER: return throw_error(SyntaxErrorProto, "'super' keyword unexpected here");
    case N_EMPTY: *out = v_undef(); return C_NORMAL;
    }
    return throw_error(SyntaxErrorProto, "this kind of expression is not supported");
}

/* ---------------------------------------------------------------- statements */

static int block_is_lexical(Ast *b) {
    if (!b->cache_len) {
        b->cache_len = 1;
        for (Ast *s = b->list; s; s = s->next) {
            if ((s->type == N_VAR && s->op != 'v') || s->type == N_CLASSDECL || s->type == N_FUNCDECL) b->flags |= F_LEXICAL;
        }
    }
    return (b->flags & F_LEXICAL) != 0;
}

/* Declare the function declarations of a statement list (hoisting). */
static void hoist_functions(Ast *first, Frame *f) {
    for (Ast *s = first; s; s = s->next) {
        if (s->type == N_FUNCDECL && s->a && s->a->str) {
            JsObj *fn = make_closure(s->a, f);
            env_define(f->env == global_lex ? global_env : f->env, s->a->str, v_obj(fn), 0);
        }
    }
}

static int exec_list(Ast *first, Frame *f, Value *out) {
    hoist_functions(first, f);
    for (Ast *s = first; s; s = s->next) {
        int r = exec(s, f, out);
        if (r != C_NORMAL) return r;
    }
    return C_NORMAL;
}

static int loop_continues(int r, JsStr *label, int *stop) {
    /* returns the completion to pass on; *stop: leave the loop */
    *stop = 0;
    if (r == C_BREAK) {
        *stop = 1;
        if (!jump_label || jump_label == label) {
            jump_label = NULL;
            return C_NORMAL;
        }
        return C_BREAK;
    }
    if (r == C_CONTINUE) {
        if (!jump_label || jump_label == label) {
            jump_label = NULL;
            return C_NORMAL;
        }
        *stop = 1;
        return C_CONTINUE;
    }
    if (r != C_NORMAL) *stop = 1;
    return r;
}

static int exec_labeled(Ast *n, Frame *f, Value *out, JsStr *label);

static int exec(Ast *n, Frame *f, Value *out) { return exec_labeled(n, f, out, NULL); }

static int exec_labeled(Ast *n, Frame *f, Value *out, JsStr *label) {
    if (!n) return C_NORMAL;
    js_exec_line = n->line;
    TRY(check_limits());
    switch (n->type) {
    case N_EXPR: return eval(n->a, f, out);
    case N_VAR: {
        for (Ast *d = n->list; d; d = d->next) {
            if (!d->b) {
                if (n->op != 'v' && d->a->type == N_IDENT) env_define(f->env, d->a->str, v_undef(), 0);
                continue;
            }
            Value v;
            TRY(eval(d->b, f, &v));
            if (d->a->type == N_IDENT) name_function(v, d->a->str);
            TRY(bind_pattern(d->a, v, f, n->op));
        }
        return C_NORMAL;
    }
    case N_FUNCDECL: return C_NORMAL; /* hoisted */
    case N_CLASSDECL: {
        Value cls;
        TRY(eval_class(n->a, f, &cls));
        env_define(f->env == global_lex ? global_lex : f->env, n->a->str, cls, 0);
        return C_NORMAL;
    }
    case N_EMPTY: return C_NORMAL;
    case N_BLOCK: {
        if (!block_is_lexical(n)) return exec_list(n->list, f, out);
        Frame inner = *f;
        inner.env = env_new(f->env);
        return exec_list(n->list, &inner, out);
    }
    case N_IF: {
        Value t;
        TRY(eval(n->a, f, &t));
        if (to_boolean(t)) return exec(n->b, f, out);
        return n->c ? exec(n->c, f, out) : C_NORMAL;
    }
    case N_WHILE:
        for (;;) {
            Value t;
            TRY(eval(n->a, f, &t));
            if (!to_boolean(t)) return C_NORMAL;
            int stop, r = loop_continues(exec(n->b, f, out), label, &stop);
            if (stop) return r;
            TRY(check_limits());
        }
    case N_DOWHILE:
        for (;;) {
            int stop, r = loop_continues(exec(n->a, f, out), label, &stop);
            if (stop) return r;
            Value t;
            TRY(eval(n->b, f, &t));
            if (!to_boolean(t)) return C_NORMAL;
            TRY(check_limits());
        }
    case N_FOR: {
        Frame lf = *f;
        int per_iteration = n->a && n->a->type == N_VAR && n->a->op != 'v';
        if (per_iteration) lf.env = env_new(f->env);
        if (n->a) {
            if (n->a->type == N_VAR) TRY(exec(n->a, &lf, out));
            else {
                Value ignored;
                TRY(eval(n->a, &lf, &ignored));
            }
        }
        for (;;) {
            if (n->b) {
                Value t;
                TRY(eval(n->b, &lf, &t));
                if (!to_boolean(t)) return C_NORMAL;
            }
            int stop, r = loop_continues(exec(n->d, &lf, out), label, &stop);
            if (stop) return r;
            if (per_iteration) { /* a fresh copy of the loop variables, for closures made in the body */
                Env *next = env_new(f->env);
                for (int i = 0; i < lf.env->n; i++) env_define(next, lf.env->names[i], lf.env->vals[i], lf.env->consts[i]);
                lf.env = next;
            }
            if (n->c) {
                Value ignored;
                TRY(eval(n->c, &lf, &ignored));
            }
            TRY(check_limits());
        }
    }
    case N_FORIN: {
        Value subject;
        TRY(eval(n->b, f, &subject));
        JsObj *items;
        if ((n->flags & F_FOR_OF) && subject.t == V_OBJ && (subject.u.o->cls == CL_GENERATOR || subject.u.o->cls == CL_OBJECT)) {
            /* a generator or a script's own iterator: one step at a time (it may never end) */
            Value iter_fn, it, next;
            TRY(get_value(subject, S_iterator, &iter_fn));
            if (is_callable(iter_fn)) {
                TRY(call_function(iter_fn.u.o, subject, NULL, 0, &it));
                if (it.t != V_OBJ) return throw_type("Result of the Symbol.iterator method is not an object", NULL, NULL);
                TRY(get_value(it, intern_c("next"), &next));
                if (!is_callable(next)) return throw_type("iterator has no next method", NULL, NULL);
                for (;;) {
                    Value step, done, v;
                    TRY(call_function(next.u.o, it, NULL, 0, &step));
                    if (step.t != V_OBJ) return throw_type("Iterator result is not an object", NULL, NULL);
                    TRY(get_value(step, S_done, &done));
                    if (to_boolean(done)) return C_NORMAL;
                    TRY(get_value(step, S_value, &v));
                    Frame lf = *f;
                    int r;
                    if (n->a->type == N_VAR) {
                        if (n->a->op != 'v') lf.env = env_new(f->env);
                        r = bind_pattern(n->a->list->a, v, &lf, n->a->op);
                    } else {
                        r = bind_pattern(n->a, v, &lf, 0);
                    }
                    int stop = r != C_NORMAL;
                    if (!stop) r = loop_continues(exec(n->d, &lf, out), label, &stop);
                    if (stop) { /* left early: the iterator is told (it.return()) */
                        Value ret_fn, ignored;
                        Value saved = js_exception;
                        JsStr *saved_label = jump_label;
                        if (get_value(it, intern_c("return"), &ret_fn) == C_NORMAL && is_callable(ret_fn)) {
                            int cr = call_function(ret_fn.u.o, it, NULL, 0, &ignored);
                            if (cr == C_THROW && r != C_THROW) return cr;
                        }
                        js_exception = saved;
                        jump_label = saved_label;
                        return r;
                    }
                    TRY(check_limits());
                }
            }
        }
        if (n->flags & F_FOR_OF) {
            TRY(iterate_to_array(subject, &items));
        } else {
            items = new_array(0);
            if (subject.t == V_OBJ) enumerable_keys(subject.u.o, items, 0);
            else if (subject.t == V_STR) {
                for (int i = 0; i < subject.u.s->len; i++) arr_push(items, v_str(number_str(i)));
            }
        }
        for (int i = 0; i < items->alen; i++) {
            Frame lf = *f;
            Value v = items->arr[i];
            if (n->a->type == N_VAR) {
                if (n->a->op != 'v') lf.env = env_new(f->env);
                TRY(bind_pattern(n->a->list->a, v, &lf, n->a->op));
            } else {
                TRY(bind_pattern(n->a, v, &lf, 0));
            }
            int stop, r = loop_continues(exec(n->d, &lf, out), label, &stop);
            if (stop) return r;
        }
        return C_NORMAL;
    }
    case N_RETURN:
        if (n->a) TRY(eval(n->a, f, out));
        else *out = v_undef();
        return C_RETURN;
    case N_BREAK:
        jump_label = n->str;
        return C_BREAK;
    case N_CONTINUE:
        jump_label = n->str;
        return C_CONTINUE;
    case N_THROW: {
        Value v;
        TRY(eval(n->a, f, &v));
        if (js_trace_throws) {
            char text[200];
            Value saved = js_exception;
            js_exception = v;
            describe_exception(text, sizeof(text));
            js_exception = saved;
            printf("[throw] (by the script, line %d, last call at character %d) %s\n", n->line, js_call_pos, text);
            if (v.t == V_OBJ) { /* what it is, when it says little */
                Value stack;
                if (get_value(v, intern_c("stack"), &stack) == C_NORMAL && stack.t == V_STR && stack.u.s->len < 400)
                    printf("[throw]   stack: %s\n", stack.u.s->s);
                Prop *cp = NULL;
                for (JsObj *p = v.u.o; p && !cp; p = p->proto) cp = own_prop(p, S_constructor);
                Prop *np = cp && cp->v.t == V_OBJ ? own_prop(cp->v.u.o, S_name) : NULL;
                if (np && np->v.t == V_STR) printf("[throw]   made by %s\n", np->v.u.s->s);
            }
        }
        js_exception = v;
        return C_THROW;
    }
    case N_TRY: {
        int r = exec(n->a, f, out);
        if (r == C_THROW && n->c && !js_abort) {
            Frame cf = *f;
            cf.env = env_new(f->env);
            Value caught = js_exception;
            js_exception = v_undef();
            r = C_NORMAL;
            if (n->b) r = bind_pattern(n->b, caught, &cf, 'l');
            if (r == C_NORMAL) r = exec(n->c, &cf, out);
        }
        if (n->d && !js_abort) {
            Value saved_out = *out, saved_exc = js_exception;
            JsStr *saved_label = jump_label;
            int fr = exec(n->d, f, out);
            if (fr != C_NORMAL) return fr; /* finally's own jump wins */
            *out = saved_out;
            js_exception = saved_exc;
            jump_label = saved_label;
        }
        return r;
    }
    case N_SWITCH: {
        Value d;
        TRY(eval(n->a, f, &d));
        Frame sf = *f;
        sf.env = env_new(f->env);
        Ast *start = NULL, *def = NULL;
        for (Ast *c = n->list; c && !start; c = c->next) {
            if (!c->a) {
                def = c;
                continue;
            }
            Value v;
            TRY(eval(c->a, &sf, &v));
            if (strict_equals(d, v)) start = c;
        }
        if (!start) start = def;
        for (Ast *c = start; c; c = c->next) {
            int r = exec_list(c->list, &sf, out);
            if (r == C_BREAK && (!jump_label || jump_label == label)) {
                jump_label = NULL;
                return C_NORMAL;
            }
            if (r != C_NORMAL) return r;
        }
        return C_NORMAL;
    }
    case N_LABEL: {
        int r = exec_labeled(n->a, f, out, n->str);
        if ((r == C_BREAK) && jump_label == n->str) {
            jump_label = NULL;
            return C_NORMAL;
        }
        return r;
    }
    }
    return eval(n, f, out); /* an expression in statement position */
}

/* ---------------------------------------------------------------- running scripts */

static char js_last_error[300];

static void describe_exception(char *out, int max) {
    Value e = js_exception;
    Buf b = {0};
    if (e.t == V_OBJ && e.u.o->cls == CL_ERROR) {
        Value name, msg;
        if (get_value(e, S_name, &name) != C_NORMAL) name = v_str(intern_c("Error"));
        if (get_value(e, S_message, &msg) != C_NORMAL) msg = v_str(S_empty);
        JsStr *ns = name.t == V_STR ? name.u.s : intern_c("Error"), *ms = msg.t == V_STR ? msg.u.s : S_empty;
        buf_add(&b, ns->s, ns->len);
        if (ms->len) {
            buf_c(&b, ": ");
            buf_add(&b, ms->s, ms->len);
        }
    } else {
        JsStr *s;
        int saved_abort = js_abort;
        js_abort = 0;
        if (to_string(e, &s) == C_NORMAL) buf_add(&b, s->s, s->len);
        js_abort = saved_abort;
        if (!b.len) buf_c(&b, "(an exception)");
    }
    int n = b.len < max - 1 ? b.len : max - 1;
    if (b.p) memcpy(out, b.p, (size_t)n);
    out[n] = 0;
    free(b.p);
}

/* Run a function from outside (an event, a timer). Errors are reported,
   not passed on. Returns 0, or -1 if it threw. */
static void js_report(const char *what);

static int js_call_toplevel(Value fn, Value this_v, Value *args, int argc, Value *ret) {
    if (!is_callable(fn)) return -1;
    char here;
    js_set_stack_base(&here);
    JsRun run;
    js_run_begin(&run, 1000); /* an event handler or timer: a second at most */
    optional_short = 0;
    Value r;
    int code = call_function(fn.u.o, this_v, args, argc, &r);
    if (ret) *ret = code == C_NORMAL ? r : v_undef();
    if (code == C_THROW) {
        describe_exception(js_last_error, sizeof(js_last_error));
        js_report(js_last_error);
        js_abort = 0;
    }
    js_run_end(&run);
    return code == C_THROW ? -1 : 0;
}

/* Run a script's text in the global scope. */
static int js_run_script(const char *src, int len, const char *where) {
    if (js_oom) return -1; /* out of memory: no more scripts on this page */
    int chunks_before = arena_chunks;
    u64 parse_t0 = uptime_ms();
    Ast *program = parse_script(src, len);
    u64 parse_ms = uptime_ms() - parse_t0;
    if (js_trace_throws) printf("[parse] %d bytes of script -> %d KiB of tree (js heap +%d KiB since collection, whole heap %d KiB; lazy %d made %d small %d parsed)\n", len, (arena_chunks - chunks_before) * 64, (int)(heap_since_gc / 1024), (int)(((char *)sbrk(0) - (char *)0x10040000000ull) / 1024), stat_lazy_made, stat_lazy_small, stat_lazy_parsed);
    if (js_oom) {
        strcpy(js_last_error, "the page's scripts used all the memory RDX allows; they were stopped");
        js_report(js_last_error);
        return -1;
    }
    if (!program) {
        strcpy(js_last_error, parse_error);
        js_report(js_last_error);
        return -1;
    }
    (void)where;
    Frame f;
    memset(&f, 0, sizeof(f));
    f.env = global_lex;
    f.var_env = global_env;
    f.this_v = v_obj(global_obj);
    f.strict = program->list && program->list->type == N_EXPR && program->list->a->type == N_STR && program->list->a->str->len == 10 &&
               !memcmp(program->list->a->str->s, "use strict", 10);
    FnInfo info;
    memset(&info, 0, sizeof(info));
    collect_vars(program, &info);
    for (int i = 0; i < info.nvars; i++) {
        if (!own_prop(global_obj, info.vars[i])) env_define(global_env, info.vars[i], v_undef(), 0);
    }
    free(info.vars);
    char here;
    js_set_stack_base(&here);
    JsRun run;
    js_run_begin(&run, 3000); /* a whole script: three seconds at most */
    js_frame_depth = 0;
    optional_short = 0;
    Value out = v_undef();
    hoist_functions(program->list, &f);
    int code = C_NORMAL;
    for (Ast *s = program->list; s && code == C_NORMAL; s = s->next) code = exec(s, &f, &out);
    if (getenv("BROWSER_TIMING") && (len > 20000 || uptime_ms() - parse_t0 > 100))
        printf("[script] %d KiB: parse %d ms, run %d ms, %d steps\n", len / 1024, (int)parse_ms, (int)(uptime_ms() - parse_t0 - parse_ms), (int)js_steps);
    if (code == C_THROW) {
        describe_exception(js_last_error, sizeof(js_last_error));
        if (js_trace_throws) printf("[script] the error below is from %s (stopped at line %d, last call at character %d)\n", where, js_exec_line, js_call_pos);
        js_report(js_last_error);
        js_abort = 0;
        js_run_end(&run);
        return -1;
    }
    js_run_end(&run);
    return 0;
}
