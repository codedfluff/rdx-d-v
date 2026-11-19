/* web/js_lib.c - JavaScript's built-in objects: Object, Function, Array,
   String, Number, Boolean, Math, JSON, Date, RegExp, Map, Set, Promise,
   Symbol, errors, console. Part of the browser. */

#define NATIVE(name) static int name(JsObj *self, Value this_v, Value *args, int argc, Value *ret)
#define ARG(i) arg(args, argc, i)
#define UNUSED (void)self, (void)this_v, (void)args, (void)argc

static int arg_number(Value *args, int argc, int i, double *out) { return to_number(arg(args, argc, i), out); }

static int arg_int(Value *args, int argc, int i, double fallback, double *out) {
    Value v = arg(args, argc, i);
    if (v.t == V_UNDEF) {
        *out = fallback;
        return C_NORMAL;
    }
    double d;
    TRY(to_number(v, &d));
    *out = is_nan(d) ? 0 : js_trunc(d);
    return C_NORMAL;
}

/* Clamp a relative index (negative counts from the end) into [0, len]. */
static int rel_index(double d, int len) {
    if (d < 0) d += len;
    if (d < 0) return 0;
    if (d > len) return len;
    return (int)d;
}

/* ================================================================ console */

static Buf console_text;
static int console_echo; /* print to standard output too (browser --dump) */

static void js_report(const char *what) {
    buf_c(&console_text, "error: ");
    buf_c(&console_text, what);
    buf_ch(&console_text, '\n');
    if (console_echo) printf("[script error] %s\n", what);
}

static int format_value(Value v, Buf *b, int depth);

NATIVE(console_log) {
    UNUSED;
    Buf line = {0};
    if (self->magic & 0xFFFF) buf_c(&line, (self->magic & 0xFFFF) == 1 ? "warning: " : "error: ");
    for (int i = 0; i < argc; i++) {
        if (i) buf_ch(&line, ' ');
        if (args[i].t == V_STR) buf_add(&line, args[i].u.s->s, args[i].u.s->len);
        else format_value(args[i], &line, 0);
    }
    if (line.p) {
        buf_add(&console_text, line.p, line.len);
        if (console_echo) printf("[console] %s\n", line.p);
    }
    buf_ch(&console_text, '\n');
    free(line.p);
    if (console_text.len > 65536) { /* keep the last part */
        memmove(console_text.p, console_text.p + console_text.len - 32768, 32768);
        console_text.len = 32768;
        console_text.p[32768] = 0;
    }
    *ret = v_undef();
    return C_NORMAL;
}

NATIVE(noop) {
    UNUSED;
    *ret = v_undef();
    return C_NORMAL;
}

/* For console.log: a readable rendering of any value. */
static int format_value(Value v, Buf *b, int depth) {
    if (v.t == V_STR) {
        buf_ch(b, '"');
        buf_add(b, v.u.s->s, v.u.s->len);
        buf_ch(b, '"');
        return C_NORMAL;
    }
    if (v.t != V_OBJ) {
        JsStr *s;
        if (to_string(v, &s) == C_NORMAL) buf_add(b, s->s, s->len);
        return C_NORMAL;
    }
    JsObj *o = v.u.o;
    if (o->cls == CL_FUNCTION) {
        Value name;
        get_value(v, S_name, &name);
        buf_c(b, "[Function");
        if (name.t == V_STR && name.u.s->len) {
            buf_ch(b, ' ');
            buf_add(b, name.u.s->s, name.u.s->len);
        }
        buf_ch(b, ']');
        return C_NORMAL;
    }
    if (o->cls == CL_ERROR || o->cls == CL_DATE || o->cls == CL_REGEXP || o->cls == CL_NODE) {
        JsStr *s;
        if (o->cls == CL_ERROR) {
            Value saved = js_exception;
            js_exception = v;
            char text[200];
            describe_exception(text, sizeof(text));
            js_exception = saved;
            buf_c(b, text);
        } else if (to_string(v, &s) == C_NORMAL) {
            buf_add(b, s->s, s->len);
        }
        return C_NORMAL;
    }
    if (depth > 2) {
        buf_c(b, o->cls == CL_ARRAY ? "[Array]" : "[Object]");
        return C_NORMAL;
    }
    if (o->cls == CL_ARRAY) {
        buf_ch(b, '[');
        for (int i = 0; i < o->alen && i < 100; i++) {
            if (i) buf_c(b, ", ");
            format_value(o->arr[i], b, depth + 1);
        }
        if (o->alen > 100) buf_c(b, ", ...");
        buf_ch(b, ']');
        return C_NORMAL;
    }
    buf_c(b, "{");
    int shown = 0;
    for (int i = 0; i < o->nprops && shown < 30; i++) {
        if (o->props[i].flags & P_HIDDEN) continue;
        buf_c(b, shown ? ", " : " ");
        buf_add(b, o->props[i].key->s, o->props[i].key->len);
        buf_c(b, ": ");
        if (o->props[i].flags & P_ACCESSOR) buf_c(b, "[Getter]");
        else format_value(o->props[i].v, b, depth + 1);
        shown++;
    }
    buf_c(b, shown ? " }" : "}");
    return C_NORMAL;
}

/* ================================================================ Object */

static JsObj *to_object_or_null(Value v) {
    if (v.t == V_OBJ) return v.u.o;
    if (v.t == V_STR) {
        JsObj *o = obj_new_raw(CL_STRING, StringProto);
        o->this_val = v;
        return o;
    }
    if (v.t == V_NUM) {
        JsObj *o = obj_new_raw(CL_NUMBER, NumberProto);
        o->this_val = v;
        return o;
    }
    if (v.t == V_BOOL) {
        JsObj *o = obj_new_raw(CL_BOOLEAN, BooleanProto);
        o->this_val = v;
        return o;
    }
    return NULL;
}

NATIVE(object_ctor) {
    UNUSED;
    Value v = ARG(0);
    JsObj *o = to_object_or_null(v);
    *ret = v_obj(o ? o : new_object());
    return C_NORMAL;
}

/* magic: 0 keys, 1 values, 2 entries, 3 getOwnPropertyNames */
NATIVE(object_keys) {
    UNUSED;
    Value v = ARG(0);
    JsObj *o = to_object_or_null(v);
    if (!o) return throw_type("Cannot convert undefined or null to object", NULL, NULL);
    JsObj *keys = new_array(0);
    if ((self->magic & 0xFFFF) == 4) { /* getOwnPropertySymbols: there are none */
        *ret = v_obj(keys);
        return C_NORMAL;
    }
    enumerable_keys(o, keys, 1);
    if ((self->magic & 0xFFFF) == 3) {
        for (int i = 0; i < o->nprops; i++) {
            if (o->props[i].flags & P_HIDDEN) arr_push(keys, v_str(o->props[i].key));
        }
    }
    int mode = self->magic & 0xFFFF;
    if (mode == 0 || mode == 3) {
        *ret = v_obj(keys);
        return C_NORMAL;
    }
    JsObj *out = new_array(keys->alen);
    for (int i = 0; i < keys->alen; i++) {
        Value item;
        TRY(get_value(v_obj(o), interned(keys->arr[i].u.s), &item));
        if (mode == 1) {
            arr_push(out, item);
        } else {
            JsObj *pair = new_array(2);
            arr_push(pair, keys->arr[i]);
            arr_push(pair, item);
            arr_push(out, v_obj(pair));
        }
    }
    *ret = v_obj(out);
    return C_NORMAL;
}

NATIVE(object_assign) {
    UNUSED;
    Value target = ARG(0);
    if (target.t != V_OBJ) return throw_type("Object.assign target must be an object", NULL, NULL);
    for (int i = 1; i < argc; i++) {
        if (args[i].t != V_OBJ) continue;
        JsObj *keys = new_array(0);
        enumerable_keys(args[i].u.o, keys, 1);
        for (int k = 0; k < keys->alen; k++) {
            JsStr *key = interned(keys->arr[k].u.s);
            Value v;
            TRY(get_value(args[i], key, &v));
            TRY(put_value(target, key, v));
        }
    }
    *ret = target;
    return C_NORMAL;
}

NATIVE(object_create) {
    UNUSED;
    Value p = ARG(0);
    if (p.t != V_OBJ && p.t != V_NULL) return throw_type("Object prototype may only be an Object or null", NULL, NULL);
    JsObj *o = obj_new_raw(CL_OBJECT, p.t == V_OBJ ? p.u.o : NULL);
    *ret = v_obj(o);
    return C_NORMAL;
}

static int define_from_descriptor(JsObj *o, JsStr *key, Value desc) {
    if (desc.t != V_OBJ) return throw_type("Property description must be an object", NULL, NULL);
    Value get, set, value, enumerable, writable;
    TRY(get_value(desc, S_get, &get));
    TRY(get_value(desc, S_set, &set));
    TRY(get_value(desc, S_value, &value));
    TRY(get_value(desc, S_enumerable, &enumerable));
    TRY(get_value(desc, S_writable, &writable));
    int flags = to_boolean(enumerable) ? 0 : P_HIDDEN;
    if (is_callable(get) || is_callable(set)) {
        Prop *p = prop_add(o, key);
        p->flags = (unsigned char)(P_ACCESSOR | flags);
        p->get = is_callable(get) ? get.u.o : NULL;
        p->set = is_callable(set) ? set.u.o : NULL;
        return C_NORMAL;
    }
    if ((o->cls == CL_ARRAY) && (key->index >= 0 || key == S_length)) return put_value(v_obj(o), key, value);
    Prop *existing = own_prop(o, key);
    if (existing && desc.t == V_OBJ && !own_prop(desc.u.o, S_value) && !(existing->flags & P_ACCESSOR)) value = existing->v;
    prop_set_raw(o, key, value, flags | (writable.t != V_UNDEF && !to_boolean(writable) ? P_READONLY : 0));
    return C_NORMAL;
}

NATIVE(object_define_property) {
    UNUSED;
    Value o = ARG(0);
    if (o.t != V_OBJ) return throw_type("Object.defineProperty called on non-object", NULL, NULL);
    JsStr *key;
    TRY(to_key(ARG(1), &key));
    TRY(define_from_descriptor(o.u.o, key, ARG(2)));
    *ret = o;
    return C_NORMAL;
}

NATIVE(object_define_properties) {
    UNUSED;
    Value o = ARG(0), descs = ARG(1);
    if (o.t != V_OBJ || descs.t != V_OBJ) return throw_type("Object.defineProperties called on non-object", NULL, NULL);
    JsObj *keys = new_array(0);
    enumerable_keys(descs.u.o, keys, 1);
    for (int i = 0; i < keys->alen; i++) {
        JsStr *key = interned(keys->arr[i].u.s);
        Value d;
        TRY(get_value(descs, key, &d));
        TRY(define_from_descriptor(o.u.o, key, d));
    }
    *ret = o;
    return C_NORMAL;
}

NATIVE(object_get_own_property_descriptor) {
    UNUSED;
    Value o = ARG(0);
    *ret = v_undef();
    if (o.t != V_OBJ) return C_NORMAL;
    JsStr *key;
    TRY(to_key(ARG(1), &key));
    Prop *p = own_prop(o.u.o, key);
    if (!p) {
        if ((o.u.o->cls == CL_ARRAY) && key->index >= 0 && key->index < o.u.o->alen) {
            JsObj *d = new_object();
            prop_set_raw(d, S_value, o.u.o->arr[key->index], 0);
            prop_set_raw(d, S_writable, v_bool(1), 0);
            prop_set_raw(d, S_enumerable, v_bool(1), 0);
            prop_set_raw(d, S_configurable, v_bool(1), 0);
            *ret = v_obj(d);
        }
        return C_NORMAL;
    }
    JsObj *d = new_object();
    if (p->flags & P_ACCESSOR) {
        prop_set_raw(d, S_get, p->get ? v_obj(p->get) : v_undef(), 0);
        prop_set_raw(d, S_set, p->set ? v_obj(p->set) : v_undef(), 0);
    } else {
        prop_set_raw(d, S_value, p->v, 0);
        prop_set_raw(d, S_writable, v_bool(!(p->flags & P_READONLY)), 0);
    }
    prop_set_raw(d, S_enumerable, v_bool(!(p->flags & P_HIDDEN)), 0);
    prop_set_raw(d, S_configurable, v_bool(1), 0);
    *ret = v_obj(d);
    return C_NORMAL;
}

NATIVE(object_get_prototype_of) {
    UNUSED;
    Value v = ARG(0);
    JsObj *p = v.t == V_OBJ ? v.u.o->proto : proto_of_primitive(v);
    *ret = p ? v_obj(p) : v_null();
    return C_NORMAL;
}

NATIVE(object_set_prototype_of) {
    UNUSED;
    Value o = ARG(0), p = ARG(1);
    if (o.t == V_OBJ) o.u.o->proto = p.t == V_OBJ ? p.u.o : NULL;
    *ret = o;
    return C_NORMAL;
}

NATIVE(object_freeze) {
    UNUSED;
    Value o = ARG(0);
    int mode = self->magic & 0xFFFF; /* 0 freeze, 1 isFrozen, 2 seal (no-op), 3 isExtensible */
    if (o.t == V_OBJ && mode == 0) o.u.o->frozen = 1;
    *ret = mode == 1 ? v_bool(o.t != V_OBJ || o.u.o->frozen) : mode == 3 ? v_bool(o.t == V_OBJ && !o.u.o->frozen) : o;
    return C_NORMAL;
}

NATIVE(object_from_entries) {
    UNUSED;
    JsObj *items;
    TRY(iterate_to_array(ARG(0), &items));
    JsObj *o = new_object();
    for (int i = 0; i < items->alen; i++) {
        Value k, v;
        TRY(get_index(items->arr[i], v_num(0), &k));
        TRY(get_index(items->arr[i], v_num(1), &v));
        JsStr *key;
        TRY(to_key(k, &key));
        prop_set_raw(o, key, v, 0);
    }
    *ret = v_obj(o);
    return C_NORMAL;
}

NATIVE(object_is) {
    UNUSED;
    Value a = ARG(0), b = ARG(1);
    int same = same_value_zero(a, b);
    if (a.t == V_NUM && b.t == V_NUM && a.u.n == 0 && b.u.n == 0) {
        union {
            double d;
            u64 i;
        } x, y;
        x.d = a.u.n;
        y.d = b.u.n;
        same = (x.i >> 63) == (y.i >> 63);
    }
    *ret = v_bool(same);
    return C_NORMAL;
}

NATIVE(object_has_own) {
    UNUSED;
    Value o = (self->magic & 0xFFFF) ? ARG(0) : this_v;
    Value k = (self->magic & 0xFFFF) ? ARG(1) : ARG(0);
    JsStr *key;
    TRY(to_key(k, &key));
    int has = 0;
    if (o.t == V_OBJ) {
        JsObj *obj = o.u.o;
        has = own_prop(obj, key) != NULL;
        if (!has && (obj->cls == CL_ARRAY || obj->cls == CL_ARGUMENTS)) has = key == S_length || (key->index >= 0 && key->index < obj->alen);
        if (!has && obj->cls == CL_FUNCTION && (key == S_prototype || key == S_name || key == S_length) && obj->fn) has = 1;
    } else if (o.t == V_STR) {
        has = key == S_length || (key->index >= 0 && key->index < o.u.s->len);
    }
    *ret = v_bool(has);
    return C_NORMAL;
}

NATIVE(object_is_prototype_of) {
    UNUSED;
    Value v = ARG(0);
    int yes = 0;
    if (v.t == V_OBJ && this_v.t == V_OBJ) {
        for (JsObj *p = v.u.o->proto; p && !yes; p = p->proto) yes = p == this_v.u.o;
    }
    *ret = v_bool(yes);
    return C_NORMAL;
}

NATIVE(object_property_is_enumerable) {
    UNUSED;
    JsStr *key;
    TRY(to_key(ARG(0), &key));
    Prop *p = this_v.t == V_OBJ ? own_prop(this_v.u.o, key) : NULL;
    *ret = v_bool(p && !(p->flags & P_HIDDEN));
    if (!p && this_v.t == V_OBJ && this_v.u.o->cls == CL_ARRAY && key->index >= 0 && key->index < this_v.u.o->alen) *ret = v_bool(1);
    return C_NORMAL;
}

NATIVE(object_to_string) {
    UNUSED;
    const char *tag = "Object";
    if (this_v.t == V_UNDEF) tag = "Undefined";
    else if (this_v.t == V_NULL) tag = "Null";
    else if (this_v.t == V_STR) tag = "String";
    else if (this_v.t == V_NUM) tag = "Number";
    else if (this_v.t == V_BOOL) tag = "Boolean";
    else {
        switch (this_v.u.o->cls) {
        case CL_ARRAY: tag = "Array"; break;
        case CL_FUNCTION: tag = "Function"; break;
        case CL_ERROR: tag = "Error"; break;
        case CL_DATE: tag = "Date"; break;
        case CL_REGEXP: tag = "RegExp"; break;
        case CL_ARGUMENTS: tag = "Arguments"; break;
        case CL_MAP: tag = "Map"; break;
        case CL_SET: tag = "Set"; break;
        case CL_PROMISE: tag = "Promise"; break;
        }
    }
    Buf b = {0};
    buf_c(&b, "[object ");
    buf_c(&b, tag);
    buf_ch(&b, ']');
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(object_value_of) {
    UNUSED;
    *ret = this_v;
    return C_NORMAL;
}

/* ================================================================ Function */

NATIVE(function_call) {
    UNUSED;
    if (!is_callable(this_v)) return throw_type("Function.prototype.call called on a non-function", NULL, NULL);
    return call_function(this_v.u.o, ARG(0), argc > 1 ? args + 1 : NULL, argc > 1 ? argc - 1 : 0, ret);
}

NATIVE(function_apply) {
    UNUSED;
    if (!is_callable(this_v)) return throw_type("Function.prototype.apply called on a non-function", NULL, NULL);
    Value list = ARG(1);
    if (is_nullish(list)) return call_function(this_v.u.o, ARG(0), NULL, 0, ret);
    JsObj *items;
    TRY(iterate_to_array(list, &items));
    return call_function(this_v.u.o, ARG(0), items->arr, items->alen, ret);
}

NATIVE(function_bind) {
    UNUSED;
    if (!is_callable(this_v)) return throw_type("Bind must be called on a function", NULL, NULL);
    JsObj *b = obj_new_raw(CL_FUNCTION, FunctionProto);
    b->target = this_v.u.o;
    b->this_val = ARG(0);
    b->fn_kind = FN_NORMAL;
    if (argc > 1) {
        b->bound = malloc(sizeof(Value) * (size_t)(argc - 1));
        if (b->bound) {
            memcpy(b->bound, args + 1, sizeof(Value) * (size_t)(argc - 1));
            b->nbound = argc - 1;
        }
    }
    Value name;
    TRY(get_value(this_v, S_name, &name));
    JsStr *ns;
    TRY(to_string(name, &ns));
    Buf n = {0};
    buf_c(&n, "bound ");
    buf_add(&n, ns->s, ns->len);
    prop_set_raw(b, S_name, v_str(buf_done(&n)), P_HIDDEN);
    *ret = v_obj(b);
    return C_NORMAL;
}

NATIVE(function_to_string) {
    UNUSED;
    *ret = v_str(intern_c("function () { [code] }"));
    return C_NORMAL;
}

NATIVE(function_ctor) {
    UNUSED;
    return throw_error(SyntaxErrorProto, "new Function() is not supported");
}

/* ================================================================ Array */

/* `this` as an array: arrays directly, array-likes copied. */
static int this_array(Value this_v, JsObj **out) {
    if (this_v.t == V_OBJ && (this_v.u.o->cls == CL_ARRAY || this_v.u.o->cls == CL_ARGUMENTS)) {
        *out = this_v.u.o;
        return C_NORMAL;
    }
    if (is_nullish(this_v)) return throw_type("Array method called on null or undefined", NULL, NULL);
    return iterate_to_array(this_v, out);
}

NATIVE(array_ctor) {
    UNUSED;
    JsObj *a = new_array(0);
    if (argc == 1 && args[0].t == V_NUM) {
        double n = args[0].u.n;
        if (n < 0 || n != js_trunc(n) || n > 50000000) return throw_error(RangeErrorProto, "Invalid array length");
        arr_set_len(a, (int)n);
    } else {
        for (int i = 0; i < argc; i++) arr_push(a, args[i]);
    }
    *ret = v_obj(a);
    return C_NORMAL;
}

NATIVE(array_is_array) {
    UNUSED;
    Value v = ARG(0);
    *ret = v_bool(v.t == V_OBJ && v.u.o->cls == CL_ARRAY);
    return C_NORMAL;
}

NATIVE(array_from) {
    UNUSED;
    Value src = ARG(0), fn = ARG(1);
    JsObj *items;
    if (src.t == V_OBJ && src.u.o->cls != CL_ARRAY) {
        Value len;
        TRY(get_value(src, S_length, &len));
        if (len.t == V_NUM && src.u.o->cls == CL_OBJECT && !own_prop(src.u.o, intern_c("next"))) {
            items = new_array(0);
            for (int i = 0; i < (int)len.u.n && i < 50000000; i++) {
                Value item;
                TRY(get_index(src, v_num(i), &item));
                arr_push(items, item);
            }
        } else {
            TRY(iterate_to_array(src, &items));
        }
    } else if (is_nullish(src)) {
        return throw_type("Array.from: argument is not iterable", NULL, NULL);
    } else {
        TRY(iterate_to_array(src, &items));
    }
    if (items == (src.t == V_OBJ ? src.u.o : NULL)) {
        JsObj *copy = new_array(items->alen);
        for (int i = 0; i < items->alen; i++) arr_push(copy, items->arr[i]);
        items = copy;
    }
    if (is_callable(fn)) {
        for (int i = 0; i < items->alen; i++) {
            Value cb_args[2] = {items->arr[i], v_num(i)};
            TRY(call_function(fn.u.o, ARG(2), cb_args, 2, &items->arr[i]));
        }
    }
    *ret = v_obj(items);
    return C_NORMAL;
}

NATIVE(array_of) {
    UNUSED;
    JsObj *a = new_array(argc);
    for (int i = 0; i < argc; i++) arr_push(a, args[i]);
    *ret = v_obj(a);
    return C_NORMAL;
}

NATIVE(array_push) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("push called on a non-array", NULL, NULL);
    JsObj *a = this_v.u.o;
    for (int i = 0; i < argc; i++) arr_push(a, args[i]);
    *ret = v_num(a->alen);
    return C_NORMAL;
}

NATIVE(array_pop) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("pop called on a non-array", NULL, NULL);
    JsObj *a = this_v.u.o;
    *ret = a->alen ? a->arr[--a->alen] : v_undef();
    return C_NORMAL;
}

NATIVE(array_shift) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("shift called on a non-array", NULL, NULL);
    JsObj *a = this_v.u.o;
    if (!a->alen) {
        *ret = v_undef();
        return C_NORMAL;
    }
    *ret = a->arr[0];
    memmove(a->arr, a->arr + 1, sizeof(Value) * (size_t)(a->alen - 1));
    a->alen--;
    return C_NORMAL;
}

NATIVE(array_unshift) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("unshift called on a non-array", NULL, NULL);
    JsObj *a = this_v.u.o;
    arr_reserve(a, a->alen + argc);
    if (js_oom) return C_NORMAL;
    memmove(a->arr + argc, a->arr, sizeof(Value) * (size_t)a->alen);
    for (int i = 0; i < argc; i++) a->arr[i] = args[i];
    a->alen += argc;
    *ret = v_num(a->alen);
    return C_NORMAL;
}

NATIVE(array_slice) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    double s, e;
    TRY(arg_int(args, argc, 0, 0, &s));
    TRY(arg_int(args, argc, 1, a->alen, &e));
    int from = rel_index(s, a->alen), to = rel_index(e, a->alen);
    JsObj *out = new_array(to > from ? to - from : 0);
    for (int i = from; i < to; i++) arr_push(out, a->arr[i]);
    *ret = v_obj(out);
    return C_NORMAL;
}

NATIVE(array_splice) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("splice called on a non-array", NULL, NULL);
    JsObj *a = this_v.u.o;
    double s, d;
    TRY(arg_int(args, argc, 0, 0, &s));
    int start = rel_index(s, a->alen);
    TRY(arg_int(args, argc, 1, a->alen - start, &d));
    int count = argc < 2 && argc >= 1 ? a->alen - start : (int)(d < 0 ? 0 : d);
    if (argc == 0) count = 0;
    if (count > a->alen - start) count = a->alen - start;
    JsObj *removed = new_array(count);
    for (int i = 0; i < count; i++) arr_push(removed, a->arr[start + i]);
    int add = argc > 2 ? argc - 2 : 0;
    int new_len = a->alen - count + add;
    arr_reserve(a, new_len);
    if (js_oom) return C_NORMAL;
    memmove(a->arr + start + add, a->arr + start + count, sizeof(Value) * (size_t)(a->alen - start - count));
    for (int i = 0; i < add; i++) a->arr[start + i] = args[2 + i];
    a->alen = new_len;
    *ret = v_obj(removed);
    return C_NORMAL;
}

NATIVE(array_concat) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    JsObj *out = new_array(a->alen);
    for (int i = 0; i < a->alen; i++) arr_push(out, a->arr[i]);
    for (int k = 0; k < argc; k++) {
        if (args[k].t == V_OBJ && args[k].u.o->cls == CL_ARRAY) {
            JsObj *b = args[k].u.o;
            for (int i = 0; i < b->alen; i++) arr_push(out, b->arr[i]);
        } else {
            arr_push(out, args[k]);
        }
    }
    *ret = v_obj(out);
    return C_NORMAL;
}

static int join_array(JsObj *a, JsStr *sep, JsStr **out, int depth) {
    if (depth > 50) {
        *out = S_empty;
        return C_NORMAL;
    }
    Buf b = {0};
    for (int i = 0; i < a->alen; i++) {
        if (i) buf_add(&b, sep->s, sep->len);
        Value v = a->arr[i];
        if (is_nullish(v)) continue;
        JsStr *s;
        int r = v.t == V_OBJ && v.u.o->cls == CL_ARRAY ? join_array(v.u.o, intern_c(","), &s, depth + 1) : to_string(v, &s);
        if (r != C_NORMAL) {
            free(b.p);
            return r;
        }
        buf_add(&b, s->s, s->len);
    }
    *out = buf_done(&b);
    return C_NORMAL;
}

NATIVE(array_join) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    JsStr *sep = intern_c(",");
    if (ARG(0).t != V_UNDEF) TRY(to_string(ARG(0), &sep));
    JsStr *s;
    TRY(join_array(a, sep, &s, 0));
    *ret = v_str(s);
    return C_NORMAL;
}

NATIVE(array_reverse) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    for (int i = 0, j = a->alen - 1; i < j; i++, j--) {
        Value t = a->arr[i];
        a->arr[i] = a->arr[j];
        a->arr[j] = t;
    }
    *ret = v_obj(a);
    return C_NORMAL;
}

/* magic: 0 indexOf, 1 lastIndexOf, 2 includes */
NATIVE(array_index_of) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    Value want = ARG(0);
    int mode = self->magic & 0xFFFF;
    double from;
    TRY(arg_int(args, argc, 1, mode == 1 ? a->alen - 1 : 0, &from));
    if (mode == 1) {
        int i = from < 0 ? (int)(a->alen + from) : (int)(from >= a->alen ? a->alen - 1 : from);
        for (; i >= 0; i--) {
            if (strict_equals(a->arr[i], want)) {
                *ret = v_num(i);
                return C_NORMAL;
            }
        }
        *ret = v_num(-1);
        return C_NORMAL;
    }
    for (int i = rel_index(from, a->alen); i < a->alen; i++) {
        if (mode == 2 ? same_value_zero(a->arr[i], want) : strict_equals(a->arr[i], want)) {
            *ret = mode == 2 ? v_bool(1) : v_num(i);
            return C_NORMAL;
        }
    }
    *ret = mode == 2 ? v_bool(0) : v_num(-1);
    return C_NORMAL;
}

/* The callback methods. magic: 0 forEach, 1 map, 2 filter, 3 find,
   4 findIndex, 5 some, 6 every, 7 findLast, 8 findLastIndex, 9 flatMap */
NATIVE(array_iterate) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    Value fn = ARG(0), this_arg = ARG(1);
    if (!is_callable(fn)) return throw_type("callback is not a function", NULL, NULL);
    int mode = self->magic & 0xFFFF;
    JsObj *out = mode == 1 || mode == 2 || mode == 9 ? new_array(0) : NULL;
    int n = a->alen;
    int backwards = mode == 7 || mode == 8;
    for (int k = 0; k < n && k < a->alen; k++) {
        int i = backwards ? n - 1 - k : k;
        if (i >= a->alen) continue;
        Value item = a->arr[i], cb_args[3] = {item, v_num(i), v_obj(a)}, r;
        TRY(call_function(fn.u.o, this_arg, cb_args, 3, &r));
        switch (mode) {
        case 1: arr_push(out, r); break;
        case 2: if (to_boolean(r)) arr_push(out, item); break;
        case 3: case 7: if (to_boolean(r)) { *ret = item; return C_NORMAL; } break;
        case 4: case 8: if (to_boolean(r)) { *ret = v_num(i); return C_NORMAL; } break;
        case 5: if (to_boolean(r)) { *ret = v_bool(1); return C_NORMAL; } break;
        case 6: if (!to_boolean(r)) { *ret = v_bool(0); return C_NORMAL; } break;
        case 9:
            if (r.t == V_OBJ && r.u.o->cls == CL_ARRAY) {
                for (int j = 0; j < r.u.o->alen; j++) arr_push(out, r.u.o->arr[j]);
            } else {
                arr_push(out, r);
            }
            break;
        }
    }
    switch (mode) {
    case 0: case 3: case 7: *ret = v_undef(); break;
    case 4: case 8: *ret = v_num(-1); break;
    case 5: *ret = v_bool(0); break;
    case 6: *ret = v_bool(1); break;
    default: *ret = v_obj(out);
    }
    return C_NORMAL;
}

/* magic 0 reduce, 1 reduceRight */
NATIVE(array_reduce) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    Value fn = ARG(0);
    if (!is_callable(fn)) return throw_type("reduce callback is not a function", NULL, NULL);
    int right = (self->magic & 0xFFFF) == 1, n = a->alen, k = 0;
    Value acc;
    if (argc >= 2) {
        acc = args[1];
    } else {
        if (!n) return throw_type("Reduce of empty array with no initial value", NULL, NULL);
        acc = a->arr[right ? n - 1 : 0];
        k = 1;
    }
    for (; k < n; k++) {
        int i = right ? n - 1 - k : k;
        if (i >= a->alen) continue;
        Value cb_args[4] = {acc, a->arr[i], v_num(i), v_obj(a)};
        TRY(call_function(fn.u.o, v_undef(), cb_args, 4, &acc));
    }
    *ret = acc;
    return C_NORMAL;
}

/* Sorting: a merge sort (stable, as the language requires). */
static int sort_compare(Value cmp, Value a, Value b, int *result) {
    if (a.t == V_UNDEF || b.t == V_UNDEF) {
        *result = a.t == V_UNDEF ? (b.t == V_UNDEF ? 0 : 1) : -1;
        return C_NORMAL;
    }
    if (is_callable(cmp)) {
        Value cb_args[2] = {a, b}, r;
        TRY(call_function(cmp.u.o, v_undef(), cb_args, 2, &r));
        double d;
        TRY(to_number(r, &d));
        *result = d < 0 ? -1 : d > 0 ? 1 : 0;
        return C_NORMAL;
    }
    JsStr *sa, *sb;
    TRY(to_string(a, &sa));
    TRY(to_string(b, &sb));
    *result = str_cmp(sa, sb);
    return C_NORMAL;
}

static int merge_sort(Value *v, Value *tmp, int n, Value cmp) {
    if (n < 2) return C_NORMAL;
    int h = n / 2;
    TRY(merge_sort(v, tmp, h, cmp));
    TRY(merge_sort(v + h, tmp, n - h, cmp));
    int i = 0, j = h, k = 0;
    while (i < h && j < n) {
        int c;
        TRY(sort_compare(cmp, v[j], v[i], &c));
        tmp[k++] = c < 0 ? v[j++] : v[i++];
    }
    while (i < h) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, sizeof(Value) * (size_t)n);
    return C_NORMAL;
}

NATIVE(array_sort) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("sort called on a non-array", NULL, NULL);
    JsObj *a = this_v.u.o;
    Value *tmp = malloc(sizeof(Value) * (size_t)(a->alen + 1));
    if (!tmp) return throw_error(RangeErrorProto, "out of memory");
    Value *copy = malloc(sizeof(Value) * (size_t)(a->alen + 1));
    if (!copy) {
        free(tmp);
        return throw_error(RangeErrorProto, "out of memory");
    }
    int n = a->alen;
    memcpy(copy, a->arr, sizeof(Value) * (size_t)n);
    int r = merge_sort(copy, tmp, n, ARG(0));
    if (r == C_NORMAL && a->alen >= n) memcpy(a->arr, copy, sizeof(Value) * (size_t)n);
    free(tmp);
    free(copy);
    *ret = this_v;
    return r;
}

static void flatten(JsObj *src, JsObj *out, int depth) {
    for (int i = 0; i < src->alen; i++) {
        Value v = src->arr[i];
        if (depth > 0 && v.t == V_OBJ && v.u.o->cls == CL_ARRAY) flatten(v.u.o, out, depth - 1);
        else arr_push(out, v);
    }
}

NATIVE(array_flat) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    double depth;
    TRY(arg_int(args, argc, 0, 1, &depth));
    JsObj *out = new_array(0);
    flatten(a, out, depth > 100 ? 100 : (int)depth);
    *ret = v_obj(out);
    return C_NORMAL;
}

NATIVE(array_fill) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    double s, e;
    TRY(arg_int(args, argc, 1, 0, &s));
    TRY(arg_int(args, argc, 2, a->alen, &e));
    for (int i = rel_index(s, a->alen); i < rel_index(e, a->alen); i++) a->arr[i] = ARG(0);
    *ret = v_obj(a);
    return C_NORMAL;
}

/* An iterator over the values of an array (live: it sees later pushes):
   next() gives {value, done}; it is iterable itself. */
static JsObj *IteratorProto;

static JsObj *make_iterator(JsObj *items) {
    JsObj *it = obj_new_raw(CL_OBJECT, IteratorProto);
    it->target = items;
    it->magic = 0;
    return it;
}

NATIVE(iterator_next) {
    UNUSED;
    if (this_v.t != V_OBJ || !this_v.u.o->target) return throw_type("next method called on incompatible receiver", NULL, NULL);
    JsObj *it = this_v.u.o, *items = it->target;
    JsObj *r = new_object();
    if (it->magic < items->alen) {
        prop_set_raw(r, S_value, items->arr[it->magic++], 0);
        prop_set_raw(r, S_done, v_bool(0), 0);
    } else {
        it->magic = 0x7FFFFFFF;
        prop_set_raw(r, S_value, v_undef(), 0);
        prop_set_raw(r, S_done, v_bool(1), 0);
    }
    *ret = v_obj(r);
    return C_NORMAL;
}

NATIVE(iterator_self) {
    UNUSED;
    *ret = this_v;
    return C_NORMAL;
}

/* arr[Symbol.iterator](), str[Symbol.iterator](), and so on */
NATIVE(values_iterator) {
    UNUSED;
    if (this_v.t == V_OBJ && this_v.u.o->cls == CL_ARRAY) {
        *ret = v_obj(make_iterator(this_v.u.o));
        return C_NORMAL;
    }
    JsObj *items;
    TRY(iterate_to_array(this_v, &items));
    *ret = v_obj(make_iterator(items));
    return C_NORMAL;
}

/* magic: 0 keys, 1 values, 2 entries: iterators */
NATIVE(array_keys) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    JsObj *out = new_array(a->alen);
    for (int i = 0; i < a->alen; i++) {
        int mode = self->magic & 0xFFFF;
        if (mode == 0) arr_push(out, v_num(i));
        else if (mode == 1) arr_push(out, a->arr[i]);
        else {
            JsObj *pair = new_array(2);
            arr_push(pair, v_num(i));
            arr_push(pair, a->arr[i]);
            arr_push(out, v_obj(pair));
        }
    }
    *ret = v_obj(make_iterator(out));
    return C_NORMAL;
}

NATIVE(array_at) {
    UNUSED;
    JsObj *a;
    TRY(this_array(this_v, &a));
    double i;
    TRY(arg_int(args, argc, 0, 0, &i));
    if (i < 0) i += a->alen;
    *ret = i >= 0 && i < a->alen ? a->arr[(int)i] : v_undef();
    return C_NORMAL;
}

/* ================================================================ String */

static int this_string(Value this_v, JsStr **out) {
    if (this_v.t == V_STR) {
        *out = this_v.u.s;
        return C_NORMAL;
    }
    if (is_nullish(this_v)) return throw_type("String method called on null or undefined", NULL, NULL);
    if (this_v.t == V_OBJ && this_v.u.o->cls == CL_STRING && this_v.u.o->this_val.t == V_STR) {
        *out = this_v.u.o->this_val.u.s;
        return C_NORMAL;
    }
    return to_string(this_v, out);
}

NATIVE(string_ctor) {
    UNUSED;
    JsStr *s = S_empty;
    if (argc) TRY(to_string(args[0], &s));
    if (native_new_target) {
        JsObj *o = obj_new_raw(CL_STRING, StringProto);
        o->this_val = v_str(s);
        *ret = v_obj(o);
    } else {
        *ret = v_str(s);
    }
    return C_NORMAL;
}

NATIVE(string_from_char_code) {
    UNUSED;
    Buf b = {0};
    for (int i = 0; i < argc; i++) {
        double d;
        TRY(to_number(args[i], &d));
        unsigned code = (unsigned)to_uint32(d);
        if ((self->magic & 0xFFFF) == 0) code &= 0xFFFF;
        put_utf8(&b, code);
    }
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(string_to_string) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    *ret = v_str(s);
    return C_NORMAL;
}

NATIVE(string_char_at) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    double i;
    TRY(arg_int(args, argc, 0, 0, &i));
    int mode = self->magic & 0xFFFF; /* 0 charAt, 1 charCodeAt, 2 at, 3 codePointAt */
    if (mode == 2 && i < 0) i += s->len;
    if (i < 0 || i >= s->len) {
        *ret = mode == 0 ? v_str(S_empty) : mode == 1 ? v_num(js_nan()) : v_undef();
        return C_NORMAL;
    }
    if (mode == 1 || mode == 3) {
        const unsigned char *p = (const unsigned char *)s->s + (int)i;
        unsigned code = p[0];
        if (mode == 3 && code >= 0xC0) { /* decode UTF-8 */
            int n = code >= 0xF0 ? 3 : code >= 0xE0 ? 2 : 1;
            code &= 0x3F >> n;
            for (int k = 1; k <= n && (int)i + k < s->len; k++) code = code << 6 | (p[k] & 0x3F);
        }
        *ret = v_num(code);
    } else {
        *ret = v_str(str_new(s->s + (int)i, 1));
    }
    return C_NORMAL;
}

static int find_text(const JsStr *s, const JsStr *t, int from) {
    if (from < 0) from = 0;
    for (int i = from; i + t->len <= s->len; i++) {
        if (memcmp(s->s + i, t->s, (size_t)t->len) == 0) return i;
    }
    return -1;
}

/* magic: 0 indexOf, 1 lastIndexOf, 2 includes, 3 startsWith, 4 endsWith */
NATIVE(string_index_of) {
    UNUSED;
    JsStr *s, *t;
    TRY(this_string(this_v, &s));
    if (ARG(0).t == V_OBJ && ARG(0).u.o->cls == CL_REGEXP && (self->magic & 0xFFFF) >= 2)
        return throw_type("First argument must not be a regular expression", NULL, NULL);
    TRY(to_string(ARG(0), &t));
    int mode = self->magic & 0xFFFF;
    double pos;
    switch (mode) {
    case 0:
        TRY(arg_int(args, argc, 1, 0, &pos));
        *ret = v_num(find_text(s, t, (int)(pos > s->len ? s->len : pos)));
        return C_NORMAL;
    case 1: {
        TRY(arg_int(args, argc, 1, s->len, &pos));
        int start = (int)(pos > s->len - t->len ? s->len - t->len : pos);
        for (int i = start; i >= 0; i--) {
            if (memcmp(s->s + i, t->s, (size_t)t->len) == 0) {
                *ret = v_num(i);
                return C_NORMAL;
            }
        }
        *ret = v_num(-1);
        return C_NORMAL;
    }
    case 2:
        TRY(arg_int(args, argc, 1, 0, &pos));
        *ret = v_bool(find_text(s, t, (int)(pos > s->len ? s->len : pos)) >= 0);
        return C_NORMAL;
    case 3:
        TRY(arg_int(args, argc, 1, 0, &pos));
        pos = pos < 0 ? 0 : pos > s->len ? s->len : pos;
        *ret = v_bool((int)pos + t->len <= s->len && memcmp(s->s + (int)pos, t->s, (size_t)t->len) == 0);
        return C_NORMAL;
    case 4: {
        TRY(arg_int(args, argc, 1, s->len, &pos));
        int end = (int)(pos < 0 ? 0 : pos > s->len ? s->len : pos);
        *ret = v_bool(end - t->len >= 0 && memcmp(s->s + end - t->len, t->s, (size_t)t->len) == 0);
        return C_NORMAL;
    }
    }
    return C_NORMAL;
}

/* magic: 0 slice, 1 substring, 2 substr */
NATIVE(string_slice) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    double a, b;
    int mode = self->magic & 0xFFFF;
    TRY(arg_int(args, argc, 0, 0, &a));
    TRY(arg_int(args, argc, 1, mode == 2 ? s->len : s->len, &b));
    int from, to;
    if (mode == 0) {
        from = rel_index(a, s->len);
        to = rel_index(b, s->len);
    } else if (mode == 1) {
        from = (int)(a < 0 ? 0 : a > s->len ? s->len : a);
        to = (int)(b < 0 ? 0 : b > s->len ? s->len : b);
        if (from > to) {
            int t = from;
            from = to;
            to = t;
        }
    } else {
        from = rel_index(a, s->len);
        to = from + (int)(b < 0 ? 0 : b > s->len ? s->len : b);
        if (to > s->len) to = s->len;
    }
    *ret = v_str(to > from ? str_new(s->s + from, to - from) : S_empty);
    return C_NORMAL;
}

/* magic: 0 upper, 1 lower */
NATIVE(string_case) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    JsStr *r = str_new(s->s, s->len);
    for (int i = 0; i < r->len; i++) r->s[i] = (char)((self->magic & 0xFFFF) ? tolower((unsigned char)r->s[i]) : toupper((unsigned char)r->s[i]));
    *ret = v_str(r);
    return C_NORMAL;
}

static int js_space(unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f'; }

/* magic: 0 trim, 1 trimStart, 2 trimEnd */
NATIVE(string_trim) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    int a = 0, b = s->len, mode = self->magic & 0xFFFF;
    if (mode != 2) {
        while (a < b && (js_space((unsigned char)s->s[a]) || ((unsigned char)s->s[a] == 0xC2 && a + 1 < b && (unsigned char)s->s[a + 1] == 0xA0))) a += (unsigned char)s->s[a] == 0xC2 ? 2 : 1;
    }
    if (mode != 1) {
        while (b > a && js_space((unsigned char)s->s[b - 1])) b--;
    }
    *ret = v_str(str_new(s->s + a, b - a));
    return C_NORMAL;
}

/* magic: 0 padStart, 1 padEnd */
NATIVE(string_pad) {
    UNUSED;
    JsStr *s, *fill = intern_c(" ");
    TRY(this_string(this_v, &s));
    double target;
    TRY(arg_int(args, argc, 0, 0, &target));
    if (ARG(1).t != V_UNDEF) TRY(to_string(ARG(1), &fill));
    if (target <= s->len || !fill->len || target > 10000000) {
        *ret = v_str(s);
        return C_NORMAL;
    }
    Buf b = {0};
    int need = (int)target - s->len;
    if ((self->magic & 0xFFFF) == 1) buf_add(&b, s->s, s->len);
    for (int i = 0; i < need; i++) buf_ch(&b, fill->s[i % fill->len]);
    if ((self->magic & 0xFFFF) == 0) buf_add(&b, s->s, s->len);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(string_repeat) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    double n;
    TRY(arg_int(args, argc, 0, 0, &n));
    if (n < 0 || (double)s->len * n > 50000000) return throw_error(RangeErrorProto, "Invalid count value");
    Buf b = {0};
    for (int i = 0; i < (int)n; i++) buf_add(&b, s->s, s->len);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(string_concat) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    Buf b = {0};
    buf_add(&b, s->s, s->len);
    for (int i = 0; i < argc; i++) {
        JsStr *t;
        TRY(to_string(args[i], &t));
        buf_add(&b, t->s, t->len);
    }
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(string_locale_compare) {
    UNUSED;
    JsStr *s, *t;
    TRY(this_string(this_v, &s));
    TRY(to_string(ARG(0), &t));
    /* letters before case: compare ignoring case first */
    int n = s->len < t->len ? s->len : t->len;
    for (int i = 0; i < n; i++) {
        int a = tolower((unsigned char)s->s[i]), b = tolower((unsigned char)t->s[i]);
        if (a != b) {
            *ret = v_num(a < b ? -1 : 1);
            return C_NORMAL;
        }
    }
    *ret = v_num(s->len == t->len ? 0 : s->len < t->len ? -1 : 1);
    return C_NORMAL;
}

/* ================================================================ regular expressions */

enum { R_CHAR, R_ANY, R_CLASS, R_START, R_END, R_WORDB, R_NWORDB, R_GROUP, R_LOOK, R_BACKREF };

typedef struct RNode {
    unsigned char type, greedy, negate, capture;
    int min, max;
    int group;               /* capture index */
    unsigned char ch;
    unsigned char set[32];   /* R_CLASS: a byte set */
    struct RNode *next;      /* the rest of the sequence */
    struct RNode **alts;     /* R_GROUP, R_LOOK: alternatives */
    int nalts;
} RNode;

typedef struct {
    RNode *root;
    int groups;
    char **names;   /* group names (index 1..groups) */
    int icase, multiline, dotall, global, sticky;
    RNode **all;    /* every node, to free */
    int nall, capall;
} Regex;

typedef struct {
    const char *p;
    int pos, len;
    Regex *re;
    const char *error;
} RParse;

static RNode *rnode(RParse *P, int type) {
    RNode *n = malloc(sizeof(RNode));
    if (!n) {
        P->error = "out of memory";
        return NULL;
    }
    memset(n, 0, sizeof(*n));
    n->type = (unsigned char)type;
    n->min = n->max = 1;
    n->greedy = 1;
    Regex *re = P->re;
    if (re->nall == re->capall) {
        int cap = re->capall ? re->capall * 2 : 32;
        RNode **all = realloc(re->all, sizeof(RNode *) * (size_t)cap);
        if (!all) {
            free(n);
            P->error = "out of memory";
            return NULL;
        }
        re->all = all;
        re->capall = cap;
    }
    re->all[re->nall++] = n;
    return n;
}

static void set_add(unsigned char *set, int c) { set[(c & 255) >> 3] |= (unsigned char)(1 << (c & 7)); }
static int set_has(const unsigned char *set, int c) { return set[(c & 255) >> 3] >> (c & 7) & 1; }

static void set_class_escape(unsigned char *set, char e) {
    int neg = e == 'D' || e == 'W' || e == 'S';
    unsigned char tmp[32];
    memset(tmp, 0, 32);
    char k = (char)tolower((unsigned char)e);
    for (int c = 0; c < 256; c++) {
        int in = k == 'd' ? (c >= '0' && c <= '9')
                 : k == 'w' ? ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
                            : (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\v' || c == '\f' || c == 0xA0);
        if (in) set_add(tmp, c);
    }
    for (int i = 0; i < 32; i++) set[i] |= neg ? (unsigned char)~tmp[i] : tmp[i];
}

/* An escape's character value (after the backslash), or -1 for a class
   escape (\d \w \s ...). */
static int escape_char(RParse *P) {
    char c = P->p[P->pos++];
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return 0;
    case 'x': {
        int v = 0;
        for (int i = 0; i < 2 && P->pos < P->len && hex_digit(P->p[P->pos]) >= 0; i++) v = v * 16 + hex_digit(P->p[P->pos++]);
        return v;
    }
    case 'u': {
        int v = 0;
        if (P->pos < P->len && P->p[P->pos] == '{') {
            P->pos++;
            while (P->pos < P->len && P->p[P->pos] != '}') v = v * 16 + (hex_digit(P->p[P->pos++]) & 15);
            P->pos++;
        } else {
            for (int i = 0; i < 4 && P->pos < P->len && hex_digit(P->p[P->pos]) >= 0; i++) v = v * 16 + hex_digit(P->p[P->pos++]);
        }
        return v < 256 ? v : '?';
    }
    case 'c': return P->pos < P->len ? (P->p[P->pos++] & 31) : 'c';
    case 'd': case 'D': case 'w': case 'W': case 's': case 'S': return -1;
    }
    return (unsigned char)c;
}

static RNode *parse_alternatives(RParse *P, int capture_index, int type);

static RNode *parse_regex_atom(RParse *P) {
    char c = P->p[P->pos];
    RNode *n;
    if (c == '(') {
        P->pos++;
        int type = R_GROUP, capture = 1, negate = 0;
        char *name = NULL;
        if (P->pos + 1 < P->len && P->p[P->pos] == '?') {
            char k = P->p[P->pos + 1];
            if (k == ':') {
                capture = 0;
                P->pos += 2;
            } else if (k == '=' || k == '!') {
                type = R_LOOK;
                capture = 0;
                negate = k == '!';
                P->pos += 2;
            } else if (k == '<' && P->pos + 2 < P->len && P->p[P->pos + 2] != '=' && P->p[P->pos + 2] != '!') {
                int start = P->pos + 2, end = start;
                while (end < P->len && P->p[end] != '>') end++;
                name = malloc((size_t)(end - start + 1));
                if (name) {
                    memcpy(name, P->p + start, (size_t)(end - start));
                    name[end - start] = 0;
                }
                P->pos = end + 1;
            } else if (k == '<') { /* lookbehind: not supported; treat as always true */
                type = R_LOOK;
                capture = 0;
                negate = 0;
                P->pos += 3;
                P->error = "lookbehind (?<= ) is not supported";
            } else {
                P->error = "unknown group";
                return NULL;
            }
        }
        int index = 0;
        if (capture) {
            index = ++P->re->groups;
            char **names = realloc(P->re->names, sizeof(char *) * (size_t)(index + 1));
            if (names) {
                P->re->names = names;
                names[index] = name;
            } else {
                free(name);
            }
        }
        n = parse_alternatives(P, index, type);
        if (!n) return NULL;
        n->capture = (unsigned char)capture;
        n->negate = (unsigned char)negate;
        if (P->pos >= P->len || P->p[P->pos] != ')') {
            P->error = "unterminated group";
            return NULL;
        }
        P->pos++;
        return n;
    }
    if (c == '[') {
        P->pos++;
        n = rnode(P, R_CLASS);
        if (!n) return NULL;
        if (P->pos < P->len && P->p[P->pos] == '^') {
            n->negate = 1;
            P->pos++;
        }
        int first = 1;
        while (P->pos < P->len && (P->p[P->pos] != ']' || (first && 0))) {
            first = 0;
            int lo;
            if (P->p[P->pos] == '\\') {
                P->pos++;
                if (P->pos >= P->len) break;
                char e = P->p[P->pos];
                if (e == 'p' || e == 'P') { /* \p{...}: letters, roughly */
                    P->pos++;
                    if (P->pos < P->len && P->p[P->pos] == '{') {
                        while (P->pos < P->len && P->p[P->pos] != '}') P->pos++;
                        P->pos++;
                    }
                    for (int ch = 'a'; ch <= 'z'; ch++) set_add(n->set, ch), set_add(n->set, ch - 32);
                    for (int ch = 128; ch < 256; ch++) set_add(n->set, ch);
                    continue;
                }
                lo = escape_char(P);
                if (lo < 0) {
                    set_class_escape(n->set, e);
                    continue;
                }
                if (e == 'b') lo = '\b';
            } else {
                lo = (unsigned char)P->p[P->pos++];
            }
            if (P->pos + 1 < P->len && P->p[P->pos] == '-' && P->p[P->pos + 1] != ']') {
                P->pos++;
                int hi;
                if (P->p[P->pos] == '\\') {
                    P->pos++;
                    hi = escape_char(P);
                    if (hi < 0) hi = lo;
                } else {
                    hi = (unsigned char)P->p[P->pos++];
                }
                if (hi > 255) hi = 255;
                for (int ch = lo; ch <= hi; ch++) set_add(n->set, ch);
                if (hi >= 128) {
                    for (int ch = 128; ch < 256; ch++) set_add(n->set, ch); /* any UTF-8 byte */
                }
            } else {
                set_add(n->set, lo);
            }
        }
        if (P->pos >= P->len) {
            P->error = "unterminated character class";
            return NULL;
        }
        P->pos++;
        return n;
    }
    P->pos++;
    switch (c) {
    case '.': return rnode(P, R_ANY);
    case '^': return rnode(P, R_START);
    case '$': return rnode(P, R_END);
    case '\\': {
        if (P->pos >= P->len) {
            P->error = "\\ at end of pattern";
            return NULL;
        }
        char e = P->p[P->pos];
        if (e == 'b' || e == 'B') {
            P->pos++;
            return rnode(P, e == 'b' ? R_WORDB : R_NWORDB);
        }
        if (e >= '1' && e <= '9') {
            n = rnode(P, R_BACKREF);
            if (!n) return NULL;
            int v = 0;
            while (P->pos < P->len && P->p[P->pos] >= '0' && P->p[P->pos] <= '9') v = v * 10 + (P->p[P->pos++] - '0');
            n->group = v;
            return n;
        }
        if (e == 'k' && P->pos + 1 < P->len && P->p[P->pos + 1] == '<') { /* \k<name> */
            int start = P->pos + 2, end = start;
            while (end < P->len && P->p[end] != '>') end++;
            n = rnode(P, R_BACKREF);
            if (!n) return NULL;
            for (int g = 1; g <= P->re->groups; g++) {
                if (P->re->names && P->re->names[g] && (int)strlen(P->re->names[g]) == end - start &&
                    memcmp(P->re->names[g], P->p + start, (size_t)(end - start)) == 0)
                    n->group = g;
            }
            P->pos = end + 1;
            return n;
        }
        if (e == 'p' || e == 'P') {
            P->pos++;
            if (P->pos < P->len && P->p[P->pos] == '{') {
                while (P->pos < P->len && P->p[P->pos] != '}') P->pos++;
                P->pos++;
            }
            n = rnode(P, R_CLASS);
            if (!n) return NULL;
            for (int ch = 'a'; ch <= 'z'; ch++) set_add(n->set, ch), set_add(n->set, ch - 32);
            for (int ch = 128; ch < 256; ch++) set_add(n->set, ch);
            n->negate = e == 'P';
            return n;
        }
        int v = escape_char(P);
        if (v < 0) {
            n = rnode(P, R_CLASS);
            if (n) set_class_escape(n->set, e);
            return n;
        }
        n = rnode(P, R_CHAR);
        if (n) n->ch = (unsigned char)v;
        return n;
    }
    }
    n = rnode(P, R_CHAR);
    if (n) n->ch = (unsigned char)c;
    return n;
}

static RNode *parse_sequence(RParse *P) {
    RNode *first = NULL, *last = NULL;
    while (P->pos < P->len && P->p[P->pos] != '|' && P->p[P->pos] != ')' && !P->error) {
        RNode *atom = parse_regex_atom(P);
        if (!atom) return NULL;
        /* a quantifier */
        if (P->pos < P->len) {
            char q = P->p[P->pos];
            int min = -1, max = -1;
            if (q == '*') min = 0, max = 1 << 30;
            else if (q == '+') min = 1, max = 1 << 30;
            else if (q == '?') min = 0, max = 1;
            else if (q == '{') {
                int j = P->pos + 1, a = 0, b = -1, digits = 0;
                while (j < P->len && P->p[j] >= '0' && P->p[j] <= '9') a = a * 10 + (P->p[j++] - '0'), digits++;
                if (digits && j < P->len && P->p[j] == '}') {
                    min = max = a;
                    P->pos = j;
                } else if (digits && j < P->len && P->p[j] == ',') {
                    j++;
                    if (j < P->len && P->p[j] == '}') b = 1 << 30;
                    else {
                        b = 0;
                        while (j < P->len && P->p[j] >= '0' && P->p[j] <= '9') b = b * 10 + (P->p[j++] - '0');
                    }
                    if (j < P->len && P->p[j] == '}') {
                        min = a;
                        max = b;
                        P->pos = j;
                    }
                }
            }
            if (min >= 0) {
                P->pos++;
                atom->min = min;
                atom->max = max;
                if (P->pos < P->len && P->p[P->pos] == '?') {
                    atom->greedy = 0;
                    P->pos++;
                }
            }
        }
        if (last) last->next = atom;
        else first = atom;
        last = atom;
    }
    return first;
}

static RNode *parse_alternatives(RParse *P, int capture_index, int type) {
    RNode *g = rnode(P, type);
    if (!g) return NULL;
    g->group = capture_index;
    for (;;) {
        RNode *seq = parse_sequence(P);
        if (P->error) return NULL;
        RNode **alts = realloc(g->alts, sizeof(RNode *) * (size_t)(g->nalts + 1));
        if (!alts) {
            P->error = "out of memory";
            return NULL;
        }
        g->alts = alts;
        g->alts[g->nalts++] = seq;
        if (P->pos < P->len && P->p[P->pos] == '|') {
            P->pos++;
            continue;
        }
        return g;
    }
}

static void regex_free(Regex *re) {
    if (!re) return;
    for (int i = 0; i < re->nall; i++) {
        free(re->all[i]->alts);
        free(re->all[i]);
    }
    free(re->all);
    if (re->names) {
        for (int i = 1; i <= re->groups; i++) free(re->names[i]);
        free(re->names);
    }
    free(re);
}

static Regex *regex_compile(const char *pattern, int len, const char *flags, int flen, const char **error) {
    Regex *re = malloc(sizeof(Regex));
    if (!re) {
        *error = "out of memory";
        return NULL;
    }
    memset(re, 0, sizeof(*re));
    for (int i = 0; i < flen; i++) {
        switch (flags[i]) {
        case 'i': re->icase = 1; break;
        case 'm': re->multiline = 1; break;
        case 's': re->dotall = 1; break;
        case 'g': re->global = 1; break;
        case 'y': re->sticky = 1; break;
        case 'u': case 'd': case 'v': break;
        default:
            *error = "invalid regular expression flags";
            regex_free(re);
            return NULL;
        }
    }
    RParse P = {pattern, 0, len, re, NULL};
    re->root = parse_alternatives(&P, 0, R_GROUP);
    if (!P.error && P.pos < len) P.error = "unmatched )";
    if (P.error) {
        *error = P.error;
        regex_free(re);
        return NULL;
    }
    return re;
}

/* ---- matching: backtracking with continuations ---- */

enum { K_SEQ, K_GROUP, K_REPEAT, K_ACCEPT };

typedef struct Cont {
    int kind;
    RNode *node;
    int count, start;
    struct Cont *next;
} Cont;

typedef struct {
    const unsigned char *s;
    int len;
    Regex *re;
    int *caps; /* 2 per group: start, end (-1 unset) */
    int end;
    long budget;
    char *stack_limit;
} Matcher;

static Matcher M;

static int match_node(RNode *n, int i, Cont *k);
static int match_atom(RNode *n, int i, Cont *k);

static int is_word_byte(int c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

static int run_cont(int i, Cont *k) {
    if (!k) {
        M.end = i;
        return 1;
    }
    switch (k->kind) {
    case K_SEQ: return match_node(k->node, i, k->next);
    case K_ACCEPT: return 1;
    case K_GROUP: {
        int g = k->count, old_s = M.caps[2 * g], old_e = M.caps[2 * g + 1];
        M.caps[2 * g] = k->start;
        M.caps[2 * g + 1] = i;
        if (run_cont(i, k->next)) return 1;
        M.caps[2 * g] = old_s;
        M.caps[2 * g + 1] = old_e;
        return 0;
    }
    case K_REPEAT: {
        RNode *n = k->node;
        int count = k->count;
        if (i == k->start && count > n->min) return run_cont(i, k->next); /* an empty iteration: stop looping */
        Cont again = {K_REPEAT, n, count + 1, i, k->next};
        if (count < n->min) return match_atom(n, i, &again);
        if (n->greedy) {
            if (count < n->max && match_atom(n, i, &again)) return 1;
            return run_cont(i, k->next);
        }
        if (run_cont(i, k->next)) return 1;
        return count < n->max && match_atom(n, i, &again);
    }
    }
    return 0;
}

static int single_matches(RNode *n, int c) {
    switch (n->type) {
    case R_CHAR:
        if (M.re->icase) return tolower(c) == tolower(n->ch);
        return c == n->ch;
    case R_ANY: return M.re->dotall || (c != '\n' && c != '\r');
    case R_CLASS: {
        int in = set_has(n->set, c) || (M.re->icase && (set_has(n->set, tolower(c)) || set_has(n->set, toupper(c))));
        return n->negate ? !in : in;
    }
    }
    return 0;
}

static int match_node(RNode *n, int i, Cont *k) {
    if (!n) return run_cont(i, k);
    if (--M.budget < 0) return 0;
    char here;
    if (&here < M.stack_limit) return 0;
    Cont rest = {K_SEQ, n->next, 0, 0, k};
    if (n->min == 1 && n->max == 1) return match_atom(n, i, &rest);
    /* single characters repeated: count forward, back off (no recursion per character) */
    if (n->type == R_CHAR || n->type == R_ANY || n->type == R_CLASS) {
        int count = 0;
        if (n->greedy) {
            while (count < n->max && i + count < M.len && single_matches(n, M.s[i + count])) count++;
            for (; count >= n->min; count--) {
                if (run_cont(i + count, &rest)) return 1;
                if (M.budget < 0) return 0;
            }
            return 0;
        }
        while (count < n->min) {
            if (i + count >= M.len || !single_matches(n, M.s[i + count])) return 0;
            count++;
        }
        for (;;) {
            if (run_cont(i + count, &rest)) return 1;
            if (count >= n->max || i + count >= M.len || !single_matches(n, M.s[i + count]) || M.budget < 0) return 0;
            count++;
        }
    }
    Cont rep = {K_REPEAT, n, 0, -1, &rest};
    return run_cont(i, &rep);
}

static int match_atom(RNode *n, int i, Cont *k) {
    switch (n->type) {
    case R_CHAR: case R_ANY: case R_CLASS:
        if (i < M.len && single_matches(n, M.s[i])) return run_cont(i + 1, k);
        return 0;
    case R_START:
        if (i == 0 || (M.re->multiline && (M.s[i - 1] == '\n' || M.s[i - 1] == '\r'))) return run_cont(i, k);
        return 0;
    case R_END:
        if (i == M.len || (M.re->multiline && (M.s[i] == '\n' || M.s[i] == '\r'))) return run_cont(i, k);
        return 0;
    case R_WORDB: case R_NWORDB: {
        int a = i > 0 && is_word_byte(M.s[i - 1]), b = i < M.len && is_word_byte(M.s[i]);
        if ((a != b) == (n->type == R_WORDB)) return run_cont(i, k);
        return 0;
    }
    case R_BACKREF: {
        int s = M.caps[2 * n->group], e = M.caps[2 * n->group + 1];
        if (n->group > M.re->groups || s < 0 || e < 0) return run_cont(i, k);
        int len = e - s;
        if (i + len > M.len) return 0;
        for (int j = 0; j < len; j++) {
            int a = M.s[s + j], b = M.s[i + j];
            if (M.re->icase ? tolower(a) != tolower(b) : a != b) return 0;
        }
        return run_cont(i + len, k);
    }
    case R_GROUP: {
        for (int a = 0; a < n->nalts; a++) {
            if (n->capture) {
                Cont close = {K_GROUP, NULL, n->group, i, k};
                if (match_node(n->alts[a], i, &close)) return 1;
            } else if (match_node(n->alts[a], i, k)) {
                return 1;
            }
            if (M.budget < 0) return 0;
        }
        return 0;
    }
    case R_LOOK: {
        int found = 0;
        Cont accept = {K_ACCEPT, NULL, 0, 0, NULL};
        for (int a = 0; a < n->nalts && !found; a++) found = match_node(n->alts[a], i, &accept);
        if (found != n->negate) return run_cont(i, k);
        return 0;
    }
    }
    return 0;
}

/* Find a match at or after `start` (sticky: only at start). Fills caps
   (group 0 is the whole match). 1 if found. */
static int regex_exec(Regex *re, const char *s, int len, int start, int *caps) {
    char here;
    M.s = (const unsigned char *)s;
    M.len = len;
    M.re = re;
    M.caps = caps;
    M.budget = 5000000;
    M.stack_limit = &here - 400000;
    for (int i = start; i <= len; i++) {
        for (int g = 0; g <= re->groups; g++) caps[2 * g] = caps[2 * g + 1] = -1;
        if (match_atom(re->root, i, NULL)) {
            caps[0] = i;
            caps[1] = M.end;
            return 1;
        }
        if (re->sticky || M.budget < 0) break;
    }
    return 0;
}

/* ---- RegExp objects ---- */

static JsObj *regexp_new(JsStr *pattern, JsStr *flags, char *error) {
    const char *why = NULL;
    Regex *re = regex_compile(pattern->s, pattern->len, flags->s, flags->len, &why);
    if (!re) {
        strcpy(error, "Invalid regular expression: /");
        strncat_safe(error, pattern->s, 60);
        strncat_safe(error, "/: ", 4);
        strncat_safe(error, why ? why : "error", 40);
        return NULL;
    }
    JsObj *o = obj_new_raw(CL_REGEXP, RegExpProto);
    o->extra = re;
    prop_set_raw(o, intern_c("source"), v_str(pattern), P_HIDDEN);
    prop_set_raw(o, intern_c("flags"), v_str(flags), P_HIDDEN);
    prop_set_raw(o, intern_c("global"), v_bool(re->global), P_HIDDEN);
    prop_set_raw(o, intern_c("ignoreCase"), v_bool(re->icase), P_HIDDEN);
    prop_set_raw(o, intern_c("multiline"), v_bool(re->multiline), P_HIDDEN);
    prop_set_raw(o, intern_c("sticky"), v_bool(re->sticky), P_HIDDEN);
    prop_set_raw(o, S_lastIndex, v_num(0), P_HIDDEN);
    return o;
}

NATIVE(regexp_ctor) {
    UNUSED;
    Value p = ARG(0), fl = ARG(1);
    JsStr *pattern, *flags = S_empty;
    if (p.t == V_OBJ && p.u.o->cls == CL_REGEXP) {
        Value src, f;
        TRY(get_value(p, intern_c("source"), &src));
        TRY(get_value(p, intern_c("flags"), &f));
        pattern = src.u.s;
        if (fl.t == V_UNDEF) flags = f.u.s;
    } else {
        pattern = S_empty;
        if (p.t != V_UNDEF) TRY(to_string(p, &pattern));
    }
    if (fl.t != V_UNDEF) TRY(to_string(fl, &flags));
    char error[160];
    JsObj *re = regexp_new(pattern, flags, error);
    if (!re) return throw_error(SyntaxErrorProto, error);
    *ret = v_obj(re);
    return C_NORMAL;
}

/* Run a RegExp against a string, as exec() does: the match array, or null. */
static int regexp_run(JsObj *ro, JsStr *s, Value *out) {
    Regex *re = ro->extra;
    int use_last = re->global || re->sticky;
    double last = 0;
    if (use_last) {
        Value li;
        TRY(get_value(v_obj(ro), S_lastIndex, &li));
        TRY(to_number(li, &last));
        if (last > s->len || last < 0) {
            prop_set_raw(ro, S_lastIndex, v_num(0), P_HIDDEN);
            *out = v_null();
            return C_NORMAL;
        }
    }
    int caps_small[40], *caps = re->groups < 20 ? caps_small : malloc(sizeof(int) * (size_t)(2 * re->groups + 2));
    if (!caps) return throw_error(RangeErrorProto, "out of memory");
    int found = regex_exec(re, s->s, s->len, (int)last, caps);
    if (!found) {
        if (use_last) prop_set_raw(ro, S_lastIndex, v_num(0), P_HIDDEN);
        *out = v_null();
    } else {
        JsObj *m = new_array(re->groups + 1);
        for (int g = 0; g <= re->groups; g++) {
            if (caps[2 * g] < 0) arr_push(m, v_undef());
            else arr_push(m, v_str(str_new(s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g])));
        }
        prop_set_raw(m, S_index, v_num(caps[0]), 0);
        prop_set_raw(m, S_input, v_str(s), 0);
        if (re->names) {
            JsObj *groups = new_object();
            for (int g = 1; g <= re->groups; g++) {
                if (re->names[g]) prop_set_raw(groups, intern_c(re->names[g]), m->arr[g], 0);
            }
            prop_set_raw(m, intern_c("groups"), v_obj(groups), 0);
        } else {
            prop_set_raw(m, intern_c("groups"), v_undef(), 0);
        }
        if (use_last) prop_set_raw(ro, S_lastIndex, v_num(caps[1] == caps[0] && re->global ? caps[1] : caps[1]), P_HIDDEN);
        *out = v_obj(m);
    }
    if (caps != caps_small) free(caps);
    return C_NORMAL;
}

NATIVE(regexp_exec) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_REGEXP) return throw_type("RegExp.prototype.exec called on incompatible receiver", NULL, NULL);
    JsStr *s;
    TRY(to_string(ARG(0), &s));
    TRY(regexp_run(this_v.u.o, s, ret));
    if ((self->magic & 0xFFFF) == 1) *ret = v_bool(ret->t != V_NULL); /* test() */
    return C_NORMAL;
}

NATIVE(regexp_to_string) {
    UNUSED;
    Value src, flags;
    TRY(get_value(this_v, intern_c("source"), &src));
    TRY(get_value(this_v, intern_c("flags"), &flags));
    Buf b = {0};
    buf_ch(&b, '/');
    if (src.t == V_STR) buf_add(&b, src.u.s->s, src.u.s->len);
    buf_ch(&b, '/');
    if (flags.t == V_STR) buf_add(&b, flags.u.s->s, flags.u.s->len);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

/* replace / replaceAll: with a string ($1, $&, $<name>...) or a function. */
static int expand_replacement(Buf *b, JsStr *rep, JsStr *s, int *caps, int groups, char **names) {
    for (int i = 0; i < rep->len; i++) {
        char c = rep->s[i];
        if (c != '$' || i + 1 >= rep->len) {
            buf_ch(b, c);
            continue;
        }
        char d = rep->s[i + 1];
        if (d == '$') {
            buf_ch(b, '$');
            i++;
        } else if (d == '&') {
            buf_add(b, s->s + caps[0], caps[1] - caps[0]);
            i++;
        } else if (d == '`') {
            buf_add(b, s->s, caps[0]);
            i++;
        } else if (d == '\'') {
            buf_add(b, s->s + caps[1], s->len - caps[1]);
            i++;
        } else if (d >= '0' && d <= '9') {
            int g = d - '0', used = 1;
            if (i + 2 < rep->len && rep->s[i + 2] >= '0' && rep->s[i + 2] <= '9' && g * 10 + (rep->s[i + 2] - '0') <= groups) {
                g = g * 10 + (rep->s[i + 2] - '0');
                used = 2;
            }
            if (g >= 1 && g <= groups) {
                if (caps[2 * g] >= 0) buf_add(b, s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]);
                i += used;
            } else {
                buf_ch(b, '$');
            }
        } else if (d == '<' && names) {
            int end = i + 2;
            while (end < rep->len && rep->s[end] != '>') end++;
            for (int g = 1; g <= groups; g++) {
                if (names[g] && (int)strlen(names[g]) == end - i - 2 && memcmp(names[g], rep->s + i + 2, (size_t)(end - i - 2)) == 0 && caps[2 * g] >= 0)
                    buf_add(b, s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]);
            }
            i = end;
        } else {
            buf_ch(b, '$');
        }
    }
    return C_NORMAL;
}

/* magic: 0 replace, 1 replaceAll */
NATIVE(string_replace) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    Value pat = ARG(0), rep = ARG(1);
    int all = (self->magic & 0xFFFF) == 1;
    JsStr *rep_text = NULL;
    if (!is_callable(rep)) TRY(to_string(rep, &rep_text));
    Buf b = {0};
    if (pat.t == V_OBJ && pat.u.o->cls == CL_REGEXP) {
        Regex *re = pat.u.o->extra;
        int global = re->global;
        if (all && !global) return throw_type("replaceAll must be called with a global RegExp", NULL, NULL);
        int caps_small[40], *caps = re->groups < 20 ? caps_small : malloc(sizeof(int) * (size_t)(2 * re->groups + 2));
        if (!caps) return throw_error(RangeErrorProto, "out of memory");
        int pos = 0, copied = 0;
        while (pos <= s->len && regex_exec(re, s->s, s->len, pos, caps)) {
            buf_add(&b, s->s + copied, caps[0] - copied);
            if (rep_text) {
                expand_replacement(&b, rep_text, s, caps, re->groups, re->names);
            } else {
                int n = re->groups + 3;
                Value small[24], *cb = n <= 24 ? small : malloc(sizeof(Value) * (size_t)n);
                if (!cb) break;
                for (int g = 0; g <= re->groups; g++)
                    cb[g] = caps[2 * g] < 0 ? v_undef() : v_str(str_new(s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g]));
                cb[re->groups + 1] = v_num(caps[0]);
                cb[re->groups + 2] = v_str(s);
                int cap0 = caps[0], cap1 = caps[1];
                Value r;
                JsStr *rs;
                int code = call_function(rep.u.o, v_undef(), cb, n, &r);
                if (code == C_NORMAL) code = to_string(r, &rs);
                if (cb != small) free(cb);
                if (code != C_NORMAL) {
                    free(b.p);
                    if (caps != caps_small) free(caps);
                    return code;
                }
                buf_add(&b, rs->s, rs->len);
                caps[0] = cap0;
                caps[1] = cap1;
            }
            copied = caps[1];
            pos = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
            if (!global) break;
        }
        if (copied < s->len) buf_add(&b, s->s + copied, s->len - copied);
        if (global) prop_set_raw(pat.u.o, S_lastIndex, v_num(0), P_HIDDEN);
        if (caps != caps_small) free(caps);
        *ret = v_str(buf_done(&b));
        return C_NORMAL;
    }
    JsStr *needle;
    TRY(to_string(pat, &needle));
    int pos = 0, copied = 0;
    for (;;) {
        int at = find_text(s, needle, pos);
        if (at < 0) break;
        buf_add(&b, s->s + copied, at - copied);
        int caps[2] = {at, at + needle->len};
        if (rep_text) {
            expand_replacement(&b, rep_text, s, caps, 0, NULL);
        } else {
            Value cb[3] = {v_str(needle), v_num(at), v_str(s)}, r;
            JsStr *rs;
            int code = call_function(rep.u.o, v_undef(), cb, 3, &r);
            if (code == C_NORMAL) code = to_string(r, &rs);
            if (code != C_NORMAL) {
                free(b.p);
                return code;
            }
            buf_add(&b, rs->s, rs->len);
        }
        copied = at + needle->len;
        pos = needle->len ? copied : copied + 1;
        if (!all || pos > s->len) break;
    }
    if (copied < s->len) buf_add(&b, s->s + copied, s->len - copied);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(string_split) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    Value sep = ARG(0);
    double limit_d;
    TRY(arg_int(args, argc, 1, 4294967295.0, &limit_d));
    int limit = limit_d > 100000000 ? 100000000 : (int)limit_d;
    JsObj *out = new_array(0);
    if (sep.t == V_UNDEF) {
        arr_push(out, v_str(s));
        *ret = v_obj(out);
        return C_NORMAL;
    }
    if (sep.t == V_OBJ && sep.u.o->cls == CL_REGEXP) {
        Regex *re = sep.u.o->extra;
        int caps_small[40], *caps = re->groups < 20 ? caps_small : malloc(sizeof(int) * (size_t)(2 * re->groups + 2));
        if (!caps) return throw_error(RangeErrorProto, "out of memory");
        int start = 0, pos = 0;
        if (s->len == 0) {
            if (!regex_exec(re, s->s, 0, 0, caps)) arr_push(out, v_str(s));
        } else {
            while (pos < s->len && out->alen < limit) {
                if (!regex_exec(re, s->s, s->len, pos, caps)) break;
                if (caps[1] == caps[0] && caps[0] >= s->len) break;
                if (caps[1] == start) { /* an empty match right where the last piece ended: move on */
                    pos = caps[0] + 1;
                    continue;
                }
                arr_push(out, v_str(str_new(s->s + start, caps[0] - start)));
                for (int g = 1; g <= re->groups && out->alen < limit; g++)
                    arr_push(out, caps[2 * g] < 0 ? v_undef() : v_str(str_new(s->s + caps[2 * g], caps[2 * g + 1] - caps[2 * g])));
                start = caps[1];
                pos = caps[1] > caps[0] ? caps[1] : caps[1] + 1;
            }
            if (out->alen < limit) arr_push(out, v_str(str_new(s->s + start, s->len - start)));
        }
        if (caps != caps_small) free(caps);
        *ret = v_obj(out);
        return C_NORMAL;
    }
    JsStr *t;
    TRY(to_string(sep, &t));
    if (t->len == 0) {
        for (int i = 0; i < s->len && out->alen < limit;) {
            unsigned char c = (unsigned char)s->s[i];
            int n = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
            if (i + n > s->len) n = s->len - i;
            arr_push(out, v_str(str_new(s->s + i, n)));
            i += n;
        }
    } else {
        int start = 0;
        for (;;) {
            if (out->alen >= limit) break;
            int at = find_text(s, t, start);
            if (at < 0) {
                arr_push(out, v_str(str_new(s->s + start, s->len - start)));
                break;
            }
            arr_push(out, v_str(str_new(s->s + start, at - start)));
            start = at + t->len;
        }
    }
    *ret = v_obj(out);
    return C_NORMAL;
}

/* magic: 0 match, 1 matchAll, 2 search */
NATIVE(string_match) {
    UNUSED;
    JsStr *s;
    TRY(this_string(this_v, &s));
    Value pat = ARG(0);
    JsObj *ro;
    if (pat.t == V_OBJ && pat.u.o->cls == CL_REGEXP) {
        ro = pat.u.o;
    } else {
        JsStr *p = S_empty;
        if (pat.t != V_UNDEF) TRY(to_string(pat, &p));
        char error[160];
        ro = regexp_new(p, (self->magic & 0xFFFF) == 1 ? intern_c("g") : S_empty, error);
        if (!ro) return throw_error(SyntaxErrorProto, error);
    }
    Regex *re = ro->extra;
    int mode = self->magic & 0xFFFF;
    if (mode == 2) {
        int caps_small[40], *caps = re->groups < 20 ? caps_small : malloc(sizeof(int) * (size_t)(2 * re->groups + 2));
        if (!caps) return throw_error(RangeErrorProto, "out of memory");
        *ret = v_num(regex_exec(re, s->s, s->len, 0, caps) ? caps[0] : -1);
        if (caps != caps_small) free(caps);
        return C_NORMAL;
    }
    if (!re->global && mode == 0) return regexp_run(ro, s, ret);
    /* all matches: match() gives the strings, matchAll() the match arrays */
    prop_set_raw(ro, S_lastIndex, v_num(0), P_HIDDEN);
    JsObj *out = new_array(0);
    int saved_global = re->global;
    re->global = 1;
    for (int guard = 0; guard < 10000000; guard++) {
        Value m;
        TRY(regexp_run(ro, s, &m));
        if (m.t == V_NULL) break;
        Value whole = m.u.o->arr[0];
        arr_push(out, mode == 0 ? whole : m);
        if (whole.t == V_STR && whole.u.s->len == 0) {
            Value li;
            TRY(get_value(v_obj(ro), S_lastIndex, &li));
            prop_set_raw(ro, S_lastIndex, v_num(li.u.n + 1), P_HIDDEN);
        }
    }
    re->global = saved_global;
    *ret = mode == 0 && out->alen == 0 ? v_null() : v_obj(out);
    return C_NORMAL;
}

/* ================================================================ Number, Boolean, Math */

NATIVE(number_ctor) {
    UNUSED;
    double d = 0;
    if (argc) TRY(to_number(args[0], &d));
    if (native_new_target) {
        JsObj *o = obj_new_raw(CL_NUMBER, NumberProto);
        o->this_val = v_num(d);
        *ret = v_obj(o);
    } else {
        *ret = v_num(d);
    }
    return C_NORMAL;
}

static int this_number(Value v, double *out) {
    if (v.t == V_NUM) {
        *out = v.u.n;
        return C_NORMAL;
    }
    if (v.t == V_OBJ && v.u.o->cls == CL_NUMBER) {
        *out = v.u.o->this_val.u.n;
        return C_NORMAL;
    }
    return throw_type("Number.prototype method called on a non-number", NULL, NULL);
}

NATIVE(number_to_string) {
    UNUSED;
    double d;
    TRY(this_number(this_v, &d));
    double radix;
    TRY(arg_int(args, argc, 0, 10, &radix));
    if (radix == 10 || !is_finite(d)) {
        *ret = v_str(number_str(d));
        return C_NORMAL;
    }
    if (radix < 2 || radix > 36) return throw_error(RangeErrorProto, "toString() radix must be between 2 and 36");
    int r = (int)radix, neg = d < 0;
    if (neg) d = -d;
    double ip = js_floor(d), fp = d - ip;
    char digits[1100];
    int n = 0;
    do {
        double q = js_floor(ip / r);
        int dig = (int)(ip - q * r);
        digits[n++] = "0123456789abcdefghijklmnopqrstuvwxyz"[dig];
        ip = q;
    } while (ip > 0 && n < 1080);
    Buf b = {0};
    if (neg) buf_ch(&b, '-');
    while (n) buf_ch(&b, digits[--n]);
    if (fp > 0) {
        buf_ch(&b, '.');
        for (int i = 0; i < 20 && fp > 0; i++) {
            fp *= r;
            int dig = (int)fp;
            buf_ch(&b, "0123456789abcdefghijklmnopqrstuvwxyz"[dig]);
            fp -= dig;
        }
    }
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

/* Decimal text with exactly `places` digits after the point. */
static void fixed_text(double d, int places, Buf *b) {
    if (d < 0) {
        buf_ch(b, '-');
        d = -d;
    }
    double scale = pow10_int(places);
    double scaled = js_floor(d * scale + 0.5);
    if (scaled >= 1e21) {
        char t[40];
        number_to_text(d, t);
        buf_c(b, t);
        return;
    }
    char digits[64];
    int n = 0;
    u64 v = (u64)scaled;
    do digits[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n <= places) digits[n++] = '0';
    for (int i = n - 1; i >= 0; i--) {
        buf_ch(b, digits[i]);
        if (i == places && places > 0) buf_ch(b, '.');
    }
}

NATIVE(number_to_fixed) {
    UNUSED;
    double d, places;
    TRY(this_number(this_v, &d));
    TRY(arg_int(args, argc, 0, 0, &places));
    if (places < 0 || places > 100) return throw_error(RangeErrorProto, "toFixed() digits argument must be between 0 and 100");
    if (!is_finite(d) || js_fabs(d) >= 1e21) {
        *ret = v_str(number_str(d));
        return C_NORMAL;
    }
    Buf b = {0};
    fixed_text(d, (int)places, &b);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(number_to_precision) {
    UNUSED;
    double d, p;
    TRY(this_number(this_v, &d));
    if (ARG(0).t == V_UNDEF || !is_finite(d)) {
        *ret = v_str(number_str(d));
        return C_NORMAL;
    }
    TRY(arg_int(args, argc, 0, 1, &p));
    if (p < 1 || p > 100) return throw_error(RangeErrorProto, "toPrecision() argument must be between 1 and 100");
    if (d == 0) {
        Buf b = {0};
        fixed_text(0, (int)p - 1, &b);
        *ret = v_str(buf_done(&b));
        return C_NORMAL;
    }
    int e = (int)js_floor(js_log(js_fabs(d)) / 2.302585092994046);
    int places = (int)p - 1 - e;
    if (places < 0) places = 0;
    Buf b = {0};
    fixed_text(d, places > 100 ? 100 : places, &b);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(number_to_locale) {
    UNUSED;
    double d;
    TRY(this_number(this_v, &d));
    if (!is_finite(d)) {
        *ret = v_str(number_str(d));
        return C_NORMAL;
    }
    /* thousands separated by commas, at most 3 decimals */
    Buf raw = {0};
    fixed_text(d, 3, &raw);
    int len = raw.len;
    while (len > 0 && raw.p[len - 1] == '0') len--;
    if (len > 0 && raw.p[len - 1] == '.') len--;
    int start = raw.p[0] == '-' ? 1 : 0, dot = start;
    while (dot < len && raw.p[dot] != '.') dot++;
    Buf b = {0};
    if (start) buf_ch(&b, '-');
    for (int i = start; i < dot; i++) {
        buf_ch(&b, raw.p[i]);
        int left = dot - i - 1;
        if (left > 0 && left % 3 == 0) buf_ch(&b, ',');
    }
    buf_add(&b, raw.p + dot, len - dot);
    free(raw.p);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(number_value_of) {
    UNUSED;
    double d;
    TRY(this_number(this_v, &d));
    *ret = v_num(d);
    return C_NORMAL;
}

/* magic: 0 isNaN(global), 1 isFinite(global), 2 Number.isNaN, 3 Number.isFinite, 4 isInteger, 5 isSafeInteger */
NATIVE(number_test) {
    UNUSED;
    Value v = ARG(0);
    int mode = self->magic & 0xFFFF;
    double d;
    if (mode >= 2 && v.t != V_NUM) {
        *ret = v_bool(0);
        return C_NORMAL;
    }
    TRY(to_number(v, &d));
    int r = 0;
    switch (mode) {
    case 0: case 2: r = is_nan(d); break;
    case 1: case 3: r = is_finite(d); break;
    case 4: r = is_finite(d) && d == js_trunc(d); break;
    case 5: r = is_finite(d) && d == js_trunc(d) && js_fabs(d) <= 9007199254740991.0; break;
    }
    *ret = v_bool(r);
    return C_NORMAL;
}

NATIVE(parse_float) {
    UNUSED;
    JsStr *s;
    TRY(to_string(ARG(0), &s));
    int used;
    double d = parse_number(s->s, s->len, &used, 1);
    /* parseFloat does not take hex */
    int i = 0;
    while (i < s->len && js_space((unsigned char)s->s[i])) i++;
    if (i + 1 < s->len && s->s[i] == '0' && (s->s[i + 1] == 'x' || s->s[i + 1] == 'X')) d = 0;
    *ret = v_num(d);
    return C_NORMAL;
}

NATIVE(parse_int) {
    UNUSED;
    JsStr *s;
    TRY(to_string(ARG(0), &s));
    double radix_d;
    TRY(arg_int(args, argc, 1, 0, &radix_d));
    int radix = (int)radix_d, i = 0, neg = 0;
    while (i < s->len && js_space((unsigned char)s->s[i])) i++;
    if (i < s->len && (s->s[i] == '-' || s->s[i] == '+')) neg = s->s[i++] == '-';
    if ((radix == 0 || radix == 16) && i + 1 < s->len && s->s[i] == '0' && (s->s[i + 1] == 'x' || s->s[i + 1] == 'X')) {
        radix = 16;
        i += 2;
    }
    if (radix == 0) radix = 10;
    if (radix < 2 || radix > 36) {
        *ret = v_num(js_nan());
        return C_NORMAL;
    }
    double v = 0;
    int digits = 0;
    for (; i < s->len; i++) {
        char c = s->s[i];
        int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'z' ? c - 'a' + 10 : c >= 'A' && c <= 'Z' ? c - 'A' + 10 : 99;
        if (d >= radix) break;
        v = v * radix + d;
        digits++;
    }
    *ret = v_num(digits ? (neg ? -v : v) : js_nan());
    return C_NORMAL;
}

NATIVE(boolean_ctor) {
    UNUSED;
    int b = to_boolean(ARG(0));
    if (native_new_target) {
        JsObj *o = obj_new_raw(CL_BOOLEAN, BooleanProto);
        o->this_val = v_bool(b);
        *ret = v_obj(o);
    } else {
        *ret = v_bool(b);
    }
    return C_NORMAL;
}

NATIVE(boolean_to_string) {
    UNUSED;
    int b = this_v.t == V_BOOL ? this_v.u.b : this_v.t == V_OBJ && this_v.u.o->cls == CL_BOOLEAN ? this_v.u.o->this_val.u.b : 0;
    *ret = (self->magic & 0xFFFF) ? v_bool(b) : v_str(b ? S_true : S_false);
    return C_NORMAL;
}

/* Math functions of one argument. */
NATIVE(math_1) {
    UNUSED;
    double x;
    TRY(arg_number(args, argc, 0, &x));
    double r;
    switch (self->magic & 0xFFFF) {
    case 0: r = js_fabs(x); break;
    case 1: r = js_floor(x); break;
    case 2: r = js_ceil(x); break;
    case 3: r = is_finite(x) ? js_floor(x + 0.5) : x; break; /* round */
    case 4: r = js_trunc(x); break;
    case 5: r = x > 0 ? 1 : x < 0 ? -1 : x; break;
    case 6: r = x < 0 ? js_nan() : js_sqrt(x); break;
    case 7: r = x < 0 ? -js_pow(-x, 1.0 / 3) : js_pow(x, 1.0 / 3); break;
    case 8: r = js_exp(x); break;
    case 9: r = js_log(x); break;
    case 10: r = js_log(x) / 0.6931471805599453; break;
    case 11: r = js_log(x) / 2.302585092994046; break;
    case 12: r = js_sin(x); break;
    case 13: r = js_cos(x); break;
    case 14: r = js_tan(x); break;
    case 15: r = js_asin(x); break;
    case 16: r = js_acos(x); break;
    case 17: r = js_atan(x); break;
    case 18: r = x; break; /* fround */
    case 19: { /* clz32 */
        unsigned u = to_uint32(x);
        int n = 0;
        while (n < 32 && !(u & 0x80000000u)) u <<= 1, n++;
        r = n;
        break;
    }
    case 20: r = js_log(1 + x); break;
    case 21: r = js_exp(x) - 1; break;
    case 22: r = (js_exp(x) - js_exp(-x)) / 2; break;
    case 23: r = (js_exp(x) + js_exp(-x)) / 2; break;
    case 24: { double a = js_exp(x), b = js_exp(-x); r = (a - b) / (a + b); break; }
    default: r = js_nan();
    }
    *ret = v_num(r);
    return C_NORMAL;
}

/* magic: 0 min, 1 max, 2 pow, 3 atan2, 4 hypot, 5 imul */
NATIVE(math_n) {
    UNUSED;
    int mode = self->magic & 0xFFFF;
    if (mode <= 1) {
        double r = mode ? -js_inf() : js_inf();
        for (int i = 0; i < argc; i++) {
            double d;
            TRY(to_number(args[i], &d));
            if (is_nan(d)) {
                *ret = v_num(js_nan());
                return C_NORMAL;
            }
            if (mode ? d > r : d < r) r = d;
        }
        *ret = v_num(r);
        return C_NORMAL;
    }
    if (mode == 4) {
        double sum = 0;
        for (int i = 0; i < argc; i++) {
            double d;
            TRY(to_number(args[i], &d));
            sum += d * d;
        }
        *ret = v_num(js_sqrt(sum));
        return C_NORMAL;
    }
    double a, b;
    TRY(arg_number(args, argc, 0, &a));
    TRY(arg_number(args, argc, 1, &b));
    *ret = v_num(mode == 2 ? js_pow(a, b) : mode == 3 ? js_atan2(a, b) : (double)(int)((unsigned)to_int32(a) * (unsigned)to_int32(b)));
    return C_NORMAL;
}

NATIVE(math_random) {
    UNUSED;
    *ret = v_num(js_random());
    return C_NORMAL;
}

/* ================================================================ JSON */

typedef struct {
    const char *s;
    int pos, len, depth;
} JsonIn;

static void json_space(JsonIn *J) {
    while (J->pos < J->len && js_space((unsigned char)J->s[J->pos])) J->pos++;
}

static int json_error(JsonIn *J) {
    char msg[80] = "Unexpected token in JSON at position ";
    char d[12];
    int k = 0, v = J->pos;
    do d[k++] = (char)('0' + v % 10);
    while (v /= 10);
    int m = (int)strlen(msg);
    while (k) msg[m++] = d[--k];
    msg[m] = 0;
    return throw_error(SyntaxErrorProto, msg);
}

static int json_value(JsonIn *J, Value *out);

static int json_string(JsonIn *J, JsStr **out) {
    J->pos++; /* " */
    Buf b = {0};
    while (J->pos < J->len && J->s[J->pos] != '"') {
        char c = J->s[J->pos++];
        if (c == '\\' && J->pos < J->len) {
            char e = J->s[J->pos++];
            switch (e) {
            case 'n': buf_ch(&b, '\n'); break;
            case 't': buf_ch(&b, '\t'); break;
            case 'r': buf_ch(&b, '\r'); break;
            case 'b': buf_ch(&b, '\b'); break;
            case 'f': buf_ch(&b, '\f'); break;
            case 'u': {
                unsigned v = 0;
                for (int i = 0; i < 4 && J->pos < J->len; i++) v = v * 16 + (unsigned)(hex_digit(J->s[J->pos++]) & 15);
                if (v >= 0xD800 && v < 0xDC00 && J->pos + 5 < J->len && J->s[J->pos] == '\\' && J->s[J->pos + 1] == 'u') {
                    unsigned lo = 0;
                    for (int i = 0; i < 4; i++) lo = lo * 16 + (unsigned)(hex_digit(J->s[J->pos + 2 + i]) & 15);
                    if (lo >= 0xDC00 && lo < 0xE000) {
                        v = 0x10000 + ((v - 0xD800) << 10) + (lo - 0xDC00);
                        J->pos += 6;
                    }
                }
                put_utf8(&b, v);
                break;
            }
            default: buf_ch(&b, e);
            }
        } else {
            buf_ch(&b, c);
        }
    }
    if (J->pos >= J->len) {
        free(b.p);
        return json_error(J);
    }
    J->pos++;
    *out = buf_done(&b);
    return C_NORMAL;
}

static int json_value(JsonIn *J, Value *out) {
    json_space(J);
    if (J->pos >= J->len) return json_error(J);
    if (++J->depth > 500) return throw_error(RangeErrorProto, "JSON is nested too deeply");
    char c = J->s[J->pos];
    int r = C_NORMAL;
    if (c == '{') {
        J->pos++;
        JsObj *o = new_object();
        json_space(J);
        if (J->pos < J->len && J->s[J->pos] == '}') {
            J->pos++;
        } else {
            for (;;) {
                json_space(J);
                if (J->pos >= J->len || J->s[J->pos] != '"') return json_error(J);
                JsStr *key;
                TRY(json_string(J, &key));
                json_space(J);
                if (J->pos >= J->len || J->s[J->pos] != ':') return json_error(J);
                J->pos++;
                Value v;
                TRY(json_value(J, &v));
                JsStr *k = interned(key);
                if (k == intern_c("__proto__")) prop_set_raw(o, k, v, 0);
                else TRY(put_value(v_obj(o), k, v));
                json_space(J);
                if (J->pos < J->len && J->s[J->pos] == ',') {
                    J->pos++;
                    continue;
                }
                if (J->pos < J->len && J->s[J->pos] == '}') {
                    J->pos++;
                    break;
                }
                return json_error(J);
            }
        }
        *out = v_obj(o);
    } else if (c == '[') {
        J->pos++;
        JsObj *a = new_array(0);
        json_space(J);
        if (J->pos < J->len && J->s[J->pos] == ']') {
            J->pos++;
        } else {
            for (;;) {
                Value v;
                TRY(json_value(J, &v));
                arr_push(a, v);
                json_space(J);
                if (J->pos < J->len && J->s[J->pos] == ',') {
                    J->pos++;
                    continue;
                }
                if (J->pos < J->len && J->s[J->pos] == ']') {
                    J->pos++;
                    break;
                }
                return json_error(J);
            }
        }
        *out = v_obj(a);
    } else if (c == '"') {
        JsStr *s;
        TRY(json_string(J, &s));
        *out = v_str(s);
    } else if (c == '-' || (c >= '0' && c <= '9')) {
        int used;
        double d = parse_number(J->s + J->pos, J->len - J->pos, &used, 1);
        if (used == 0) return json_error(J);
        J->pos += used;
        *out = v_num(d);
    } else if (J->pos + 4 <= J->len && memcmp(J->s + J->pos, "true", 4) == 0) {
        J->pos += 4;
        *out = v_bool(1);
    } else if (J->pos + 5 <= J->len && memcmp(J->s + J->pos, "false", 5) == 0) {
        J->pos += 5;
        *out = v_bool(0);
    } else if (J->pos + 4 <= J->len && memcmp(J->s + J->pos, "null", 4) == 0) {
        J->pos += 4;
        *out = v_null();
    } else {
        r = json_error(J);
    }
    J->depth--;
    return r;
}

NATIVE(json_parse) {
    UNUSED;
    JsStr *s;
    TRY(to_string(ARG(0), &s));
    JsonIn J = {s->s, 0, s->len, 0};
    TRY(json_value(&J, ret));
    json_space(&J);
    if (J.pos < J.len) return json_error(&J);
    return C_NORMAL;
}

static void json_quote(Buf *b, const JsStr *s) {
    buf_ch(b, '"');
    for (int i = 0; i < s->len; i++) {
        unsigned char c = (unsigned char)s->s[i];
        switch (c) {
        case '"': buf_c(b, "\\\""); break;
        case '\\': buf_c(b, "\\\\"); break;
        case '\n': buf_c(b, "\\n"); break;
        case '\r': buf_c(b, "\\r"); break;
        case '\t': buf_c(b, "\\t"); break;
        case '\b': buf_c(b, "\\b"); break;
        case '\f': buf_c(b, "\\f"); break;
        default:
            if (c < 0x20) {
                char t[8] = "\\u00";
                t[4] = "0123456789abcdef"[c >> 4];
                t[5] = "0123456789abcdef"[c & 15];
                t[6] = 0;
                buf_c(b, t);
            } else {
                buf_ch(b, (char)c);
            }
        }
    }
    buf_ch(b, '"');
}

typedef struct {
    Buf out;
    JsStr *indent;
    Value replacer;
    JsObj *stack[200];
    int depth;
} JsonOut;

static void json_newline(JsonOut *J) {
    if (!J->indent || !J->indent->len) return;
    buf_ch(&J->out, '\n');
    for (int i = 0; i < J->depth; i++) buf_add(&J->out, J->indent->s, J->indent->len);
}

/* Write a value; *skip set for undefined / functions (left out of objects). */
static int json_write(JsonOut *J, Value holder, JsStr *key, Value v, int *skip) {
    *skip = 0;
    if (v.t == V_OBJ) {
        Value to_json;
        TRY(get_value(v, S_toJSON, &to_json));
        if (is_callable(to_json)) {
            Value k = v_str(key);
            TRY(call_function(to_json.u.o, v, &k, 1, &v));
        }
    }
    if (is_callable(J->replacer)) {
        Value cb[2] = {v_str(key), v};
        TRY(call_function(J->replacer.u.o, holder, cb, 2, &v));
    }
    if (v.t == V_OBJ && (v.u.o->cls == CL_NUMBER || v.u.o->cls == CL_STRING || v.u.o->cls == CL_BOOLEAN)) v = v.u.o->this_val;
    switch (v.t) {
    case V_UNDEF: *skip = 1; return C_NORMAL;
    case V_NULL: buf_c(&J->out, "null"); return C_NORMAL;
    case V_BOOL: buf_c(&J->out, v.u.b ? "true" : "false"); return C_NORMAL;
    case V_NUM: {
        if (!is_finite(v.u.n)) buf_c(&J->out, "null");
        else {
            char t[40];
            number_to_text(v.u.n, t);
            buf_c(&J->out, t);
        }
        return C_NORMAL;
    }
    case V_STR: json_quote(&J->out, v.u.s); return C_NORMAL;
    }
    JsObj *o = v.u.o;
    if (o->cls == CL_FUNCTION) {
        *skip = 1;
        return C_NORMAL;
    }
    for (int i = 0; i < J->depth; i++) {
        if (J->stack[i] == o) return throw_type("Converting circular structure to JSON", NULL, NULL);
    }
    if (J->depth >= 199) return throw_type("JSON.stringify: too deeply nested", NULL, NULL);
    J->stack[J->depth++] = o;
    if (o->cls == CL_ARRAY) {
        buf_ch(&J->out, '[');
        for (int i = 0; i < o->alen; i++) {
            if (i) buf_ch(&J->out, ',');
            json_newline(J);
            int s;
            TRY(json_write(J, v, number_str(i), o->arr[i], &s));
            if (s) buf_c(&J->out, "null");
        }
        J->depth--;
        if (o->alen) json_newline(J);
        buf_ch(&J->out, ']');
        return C_NORMAL;
    }
    buf_ch(&J->out, '{');
    JsObj *keys = new_array(0);
    enumerable_keys(o, keys, 1);
    if (J->replacer.t == V_OBJ && J->replacer.u.o->cls == CL_ARRAY) keys = J->replacer.u.o;
    int written = 0;
    for (int i = 0; i < keys->alen; i++) {
        JsStr *k;
        TRY(to_key(keys->arr[i], &k));
        Value item;
        TRY(get_value(v, k, &item));
        int mark = J->out.len, s;
        if (written) buf_ch(&J->out, ',');
        json_newline(J);
        json_quote(&J->out, k);
        buf_ch(&J->out, ':');
        if (J->indent && J->indent->len) buf_ch(&J->out, ' ');
        TRY(json_write(J, v, k, item, &s));
        if (s) {
            J->out.len = mark; /* leave it out */
            if (J->out.p) J->out.p[mark] = 0;
        } else {
            written++;
        }
    }
    J->depth--;
    if (written) json_newline(J);
    buf_ch(&J->out, '}');
    return C_NORMAL;
}

NATIVE(json_stringify) {
    UNUSED;
    JsonOut J;
    memset(&J, 0, sizeof(J));
    J.replacer = ARG(1);
    Value space = ARG(2);
    if (space.t == V_NUM) {
        int n = space.u.n > 10 ? 10 : (int)space.u.n;
        char sp[11] = "          ";
        sp[n < 0 ? 0 : n] = 0;
        J.indent = intern_c(sp);
    } else if (space.t == V_STR) {
        J.indent = space.u.s;
    }
    JsObj *holder = new_object();
    int skip;
    int r = json_write(&J, v_obj(holder), S_empty, ARG(0), &skip);
    if (r != C_NORMAL) {
        free(J.out.p);
        return r;
    }
    if (skip) {
        free(J.out.p);
        *ret = v_undef();
        return C_NORMAL;
    }
    *ret = v_str(buf_done(&J.out));
    return C_NORMAL;
}

/* ================================================================ Date */

static double now_ms(void) { return (double)time_now() * 1000.0 + (double)(uptime_ms() % 1000); }

static double days_from_civil(double y, double m, double d) {
    /* m: 1..12 */
    y -= m <= 2;
    double era = js_floor(y / 400);
    double yoe = y - era * 400;
    double mp = m > 2 ? m - 3 : m + 9;
    double doy = js_floor((153 * mp + 2) / 5) + d - 1;
    double doe = yoe * 365 + js_floor(yoe / 4) - js_floor(yoe / 100) + doy;
    return era * 146097 + doe - 719468;
}

static double make_time(double y, double mon, double d, double h, double mi, double s, double ms) {
    /* mon: 0..11, may overflow */
    y += js_floor(mon / 12);
    mon = mon - js_floor(mon / 12) * 12;
    double days = days_from_civil(y, mon + 1, 1) + d - 1;
    return days * 86400000.0 + h * 3600000.0 + mi * 60000.0 + s * 1000.0 + ms;
}

typedef struct {
    double year, month, day, hours, minutes, seconds, ms, weekday;
} DateParts;

static DateParts date_parts(double t) {
    DateParts p;
    double days = js_floor(t / 86400000.0);
    double rem = t - days * 86400000.0;
    double z = days + 719468, era = js_floor(z / 146097), doe = z - era * 146097;
    double yoe = js_floor((doe - js_floor(doe / 1460) + js_floor(doe / 36524) - js_floor(doe / 146096)) / 365);
    double y = yoe + era * 400, doy = doe - (365 * yoe + js_floor(yoe / 4) - js_floor(yoe / 100));
    double mp = js_floor((5 * doy + 2) / 153);
    p.day = doy - js_floor((153 * mp + 2) / 5) + 1;
    p.month = mp < 10 ? mp + 2 : mp - 10; /* 0-based */
    p.year = y + (p.month <= 1);
    p.hours = js_floor(rem / 3600000.0);
    p.minutes = js_floor(js_fmod(rem, 3600000.0) / 60000.0);
    p.seconds = js_floor(js_fmod(rem, 60000.0) / 1000.0);
    p.ms = js_fmod(rem, 1000.0);
    p.weekday = js_fmod(days + 4, 7);
    if (p.weekday < 0) p.weekday += 7;
    return p;
}

/* "2024-03-05", "2024-03-05T10:20:30.123Z", and a few others. NaN if not understood. */
static double parse_date(const JsStr *s) {
    int v[7] = {0, 1, 1, 0, 0, 0, 0}, k = 0, i = 0;
    int neg_year = 0;
    if (s->len && s->s[0] == '-') {
        neg_year = 1;
        i++;
    }
    while (i < s->len && k < 7) {
        if (s->s[i] >= '0' && s->s[i] <= '9') {
            int n = 0, digits = 0;
            while (i < s->len && s->s[i] >= '0' && s->s[i] <= '9') {
                n = n * 10 + (s->s[i++] - '0');
                digits++;
            }
            if (k == 6) {
                while (digits < 3) n *= 10, digits++;
                while (digits > 3) n /= 10, digits--;
            }
            v[k++] = n;
            if (i < s->len && (s->s[i] == '-' || s->s[i] == ':' || s->s[i] == 'T' || s->s[i] == '.' || s->s[i] == ' ' || s->s[i] == '/')) i++;
            else break;
        } else {
            break;
        }
    }
    if (k < 1 || v[0] < 100) {
        /* "Mar 5 2024" style: month names */
        static const char *months[] = {"jan", "feb", "mar", "apr", "may", "jun", "jul", "aug", "sep", "oct", "nov", "dec"};
        for (int m = 0; m < 12; m++) {
            for (int j = 0; j + 3 <= s->len; j++) {
                if (strncasecmp(s->s + j, months[m], 3) == 0) {
                    int nums[4] = {0, 0, 0, 0}, nn = 0;
                    for (int q = 0; q < s->len && nn < 4; q++) {
                        if (s->s[q] >= '0' && s->s[q] <= '9') {
                            int n = 0;
                            while (q < s->len && s->s[q] >= '0' && s->s[q] <= '9') n = n * 10 + (s->s[q++] - '0');
                            nums[nn++] = n;
                        }
                    }
                    if (nn >= 2) {
                        int day = nums[0] > 31 ? nums[1] : nums[0], year = nums[0] > 31 ? nums[0] : nums[1];
                        return make_time(year, m, day, 0, 0, 0, 0);
                    }
                }
            }
        }
        return js_nan();
    }
    double year = neg_year ? -v[0] : v[0];
    return make_time(year, v[1] - 1, v[2], v[3], v[4], v[5], v[6]);
}

NATIVE(date_ctor) {
    UNUSED;
    if (!native_new_target) { /* Date() as a function: a string */
        DateParts p = date_parts(now_ms());
        (void)p;
        JsObj *d = obj_new_raw(CL_DATE, DateProto);
        d->this_val = v_num(now_ms());
        Value s;
        TRY(get_value(v_obj(d), S_toString, &s));
        return call_function(s.u.o, v_obj(d), NULL, 0, ret);
    }
    double t;
    if (argc == 0) {
        t = now_ms();
    } else if (argc == 1) {
        Value a = args[0];
        if (a.t == V_OBJ && a.u.o->cls == CL_DATE) t = a.u.o->this_val.u.n;
        else if (a.t == V_STR) t = parse_date(a.u.s);
        else TRY(to_number(a, &t));
    } else {
        double f[7] = {0, 0, 1, 0, 0, 0, 0};
        for (int i = 0; i < argc && i < 7; i++) TRY(to_number(args[i], &f[i]));
        if (f[0] >= 0 && f[0] <= 99) f[0] += 1900;
        t = make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
    }
    if (is_finite(t)) t = js_trunc(t);
    JsObj *d = obj_new_raw(CL_DATE, DateProto);
    d->this_val = v_num(is_finite(t) && js_fabs(t) <= 8.64e15 ? t : js_nan());
    *ret = v_obj(d);
    return C_NORMAL;
}

NATIVE(date_now) {
    UNUSED;
    *ret = v_num(js_floor(now_ms()));
    return C_NORMAL;
}

NATIVE(date_parse) {
    UNUSED;
    JsStr *s;
    TRY(to_string(ARG(0), &s));
    *ret = v_num(parse_date(s));
    return C_NORMAL;
}

NATIVE(date_utc) {
    UNUSED;
    double f[7] = {0, 0, 1, 0, 0, 0, 0};
    for (int i = 0; i < argc && i < 7; i++) TRY(to_number(args[i], &f[i]));
    *ret = v_num(make_time(f[0], f[1], f[2], f[3], f[4], f[5], f[6]));
    return C_NORMAL;
}

static int this_date(Value v, double *t) {
    if (v.t != V_OBJ || v.u.o->cls != CL_DATE) return throw_type("this is not a Date object.", NULL, NULL);
    *t = v.u.o->this_val.u.n;
    return C_NORMAL;
}

/* magic: 0 time, 1 year, 2 month, 3 date, 4 day, 5 hours, 6 minutes, 7 seconds, 8 ms, 9 tz offset */
NATIVE(date_get) {
    UNUSED;
    double t;
    TRY(this_date(this_v, &t));
    int mode = self->magic & 0xFFFF;
    if (mode == 9) {
        *ret = v_num(0);
        return C_NORMAL;
    }
    if (is_nan(t) || mode == 0) {
        *ret = v_num(t);
        return C_NORMAL;
    }
    DateParts p = date_parts(t);
    double r[] = {t, p.year, p.month, p.day, p.weekday, p.hours, p.minutes, p.seconds, p.ms};
    *ret = v_num(r[mode]);
    return C_NORMAL;
}

/* magic: 0 setTime, 1 setFullYear, 2 setMonth, 3 setDate, 5 setHours, 6 setMinutes, 7 setSeconds, 8 setMilliseconds */
NATIVE(date_set) {
    UNUSED;
    double t;
    TRY(this_date(this_v, &t));
    int mode = self->magic & 0xFFFF;
    double v;
    TRY(arg_number(args, argc, 0, &v));
    if (mode == 0) {
        t = v;
    } else {
        DateParts p = date_parts(is_nan(t) ? 0 : t);
        double f[9] = {0, p.year, p.month, p.day, 0, p.hours, p.minutes, p.seconds, p.ms};
        f[mode] = v;
        /* extra arguments: setHours(h, m, s, ms), setFullYear(y, m, d)... */
        for (int i = 1; i < argc && mode + i <= 8; i++) {
            int slot = mode + i == 4 ? 5 : mode + i;
            if (mode == 3) break;
            TRY(to_number(args[i], &f[slot]));
        }
        t = make_time(f[1], f[2], f[3], f[5], f[6], f[7], f[8]);
    }
    this_v.u.o->this_val = v_num(t);
    *ret = v_num(t);
    return C_NORMAL;
}

static void two(Buf *b, double v) {
    int n = (int)v;
    buf_ch(b, (char)('0' + n / 10 % 10));
    buf_ch(b, (char)('0' + n % 10));
}

/* magic: 0 toISOString, 1 toString, 2 toDateString, 3 toTimeString, 4 toLocaleDateString, 5 toLocaleTimeString, 6 toLocaleString, 7 toUTCString */
NATIVE(date_format) {
    UNUSED;
    double t;
    TRY(this_date(this_v, &t));
    int mode = self->magic & 0xFFFF;
    if (is_nan(t)) {
        if (mode == 0) return throw_error(RangeErrorProto, "Invalid time value");
        *ret = v_str(intern_c("Invalid Date"));
        return C_NORMAL;
    }
    DateParts p = date_parts(t);
    static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    Buf b = {0};
    char year[16];
    number_to_text(p.year, year);
    if (mode == 0) {
        buf_c(&b, year);
        buf_ch(&b, '-');
        two(&b, p.month + 1);
        buf_ch(&b, '-');
        two(&b, p.day);
        buf_ch(&b, 'T');
        two(&b, p.hours);
        buf_ch(&b, ':');
        two(&b, p.minutes);
        buf_ch(&b, ':');
        two(&b, p.seconds);
        buf_ch(&b, '.');
        buf_ch(&b, (char)('0' + (int)p.ms / 100));
        two(&b, js_fmod(p.ms, 100));
        buf_ch(&b, 'Z');
    } else if (mode == 4 || mode == 5 || mode == 6) {
        if (mode != 5) {
            char t2[8];
            number_to_text(p.month + 1, t2);
            buf_c(&b, t2);
            buf_ch(&b, '/');
            number_to_text(p.day, t2);
            buf_c(&b, t2);
            buf_ch(&b, '/');
            buf_c(&b, year);
        }
        if (mode == 6) buf_c(&b, ", ");
        if (mode != 4) {
            int h = (int)p.hours % 12;
            char t2[8];
            number_to_text(h ? h : 12, t2);
            buf_c(&b, t2);
            buf_ch(&b, ':');
            two(&b, p.minutes);
            buf_ch(&b, ':');
            two(&b, p.seconds);
            buf_c(&b, p.hours < 12 ? " AM" : " PM");
        }
    } else if (mode == 7) {
        buf_c(&b, days[(int)p.weekday]);
        buf_c(&b, ", ");
        two(&b, p.day);
        buf_ch(&b, ' ');
        buf_c(&b, months[(int)p.month]);
        buf_ch(&b, ' ');
        buf_c(&b, year);
        buf_ch(&b, ' ');
        two(&b, p.hours);
        buf_ch(&b, ':');
        two(&b, p.minutes);
        buf_ch(&b, ':');
        two(&b, p.seconds);
        buf_c(&b, " GMT");
    } else {
        if (mode != 3) {
            buf_c(&b, days[(int)p.weekday]);
            buf_ch(&b, ' ');
            buf_c(&b, months[(int)p.month]);
            buf_ch(&b, ' ');
            two(&b, p.day);
            buf_ch(&b, ' ');
            buf_c(&b, year);
        }
        if (mode == 1) buf_ch(&b, ' ');
        if (mode != 2) {
            two(&b, p.hours);
            buf_ch(&b, ':');
            two(&b, p.minutes);
            buf_ch(&b, ':');
            two(&b, p.seconds);
            buf_c(&b, " GMT+0000 (Coordinated Universal Time)");
        }
    }
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

/* ================================================================ Map and Set */

/* Entries: pairs of Values in obj->extra; a deleted pair is (undefined, null). */
static int map_find(JsObj *m, Value key) {
    Value *e = m->extra;
    for (int i = 0; i < m->extra_len; i++) {
        if (e[2 * i].t == V_UNDEF && e[2 * i + 1].t == V_NULL) continue;
        if (same_value_zero(e[2 * i], key)) return i;
    }
    return -1;
}

static void map_put(JsObj *m, Value key, Value v) {
    int i = map_find(m, key);
    if (i >= 0) {
        ((Value *)m->extra)[2 * i + 1] = v;
        return;
    }
    if (m->extra_len == m->extra_cap) {
        int cap = m->extra_cap ? m->extra_cap * 2 : 8;
        Value *e = realloc(m->extra, sizeof(Value) * 2 * (size_t)cap);
        if (!e) {
            js_oom = 1;
            return;
        }
        m->extra = e;
        m->extra_cap = cap;
    }
    Value *e = m->extra;
    e[2 * m->extra_len] = key;
    e[2 * m->extra_len + 1] = v;
    m->extra_len++;
}

static int map_size(JsObj *m) {
    int n = 0;
    Value *e = m->extra;
    for (int i = 0; i < m->extra_len; i++) n += !(e[2 * i].t == V_UNDEF && e[2 * i + 1].t == V_NULL);
    return n;
}

NATIVE(map_ctor) {
    UNUSED;
    int is_set = (self->magic & 0xFFFF) == 1;
    if (!native_new_target) return throw_type(is_set ? "Constructor Set requires 'new'" : "Constructor Map requires 'new'", NULL, NULL);
    JsObj *m = obj_new_raw(is_set ? CL_SET : CL_MAP, is_set ? SetProto : MapProto);
    Value init = ARG(0);
    if (!is_nullish(init)) {
        JsObj *items;
        TRY(iterate_to_array(init, &items));
        for (int i = 0; i < items->alen; i++) {
            if (is_set) {
                map_put(m, items->arr[i], v_undef());
            } else {
                Value k, v;
                TRY(get_index(items->arr[i], v_num(0), &k));
                TRY(get_index(items->arr[i], v_num(1), &v));
                map_put(m, k, v);
            }
        }
    }
    *ret = v_obj(m);
    return C_NORMAL;
}

static int this_map(Value v, JsObj **m) {
    if (v.t != V_OBJ || (v.u.o->cls != CL_MAP && v.u.o->cls != CL_SET)) return throw_type("Method called on incompatible receiver", NULL, NULL);
    *m = v.u.o;
    return C_NORMAL;
}

/* magic: 0 get, 1 set, 2 has, 3 delete, 4 clear, 5 add, 6 size, 7 forEach, 8 keys, 9 values, 10 entries */
NATIVE(map_method) {
    UNUSED;
    JsObj *m;
    TRY(this_map(this_v, &m));
    int mode = self->magic & 0xFFFF, i;
    Value *e = m->extra;
    switch (mode) {
    case 0:
        i = map_find(m, ARG(0));
        *ret = i >= 0 ? e[2 * i + 1] : v_undef();
        return C_NORMAL;
    case 1: map_put(m, ARG(0), ARG(1)); *ret = this_v; return C_NORMAL;
    case 5: map_put(m, ARG(0), v_undef()); *ret = this_v; return C_NORMAL;
    case 2: *ret = v_bool(map_find(m, ARG(0)) >= 0); return C_NORMAL;
    case 3:
        i = map_find(m, ARG(0));
        if (i >= 0) {
            e[2 * i] = v_undef();
            e[2 * i + 1] = v_null();
        }
        *ret = v_bool(i >= 0);
        return C_NORMAL;
    case 4: m->extra_len = 0; *ret = v_undef(); return C_NORMAL;
    case 6: *ret = v_num(map_size(m)); return C_NORMAL;
    case 7: {
        Value fn = ARG(0);
        if (!is_callable(fn)) return throw_type("forEach callback is not a function", NULL, NULL);
        for (i = 0; i < m->extra_len; i++) {
            e = m->extra;
            if (e[2 * i].t == V_UNDEF && e[2 * i + 1].t == V_NULL) continue;
            Value cb[3] = {m->cls == CL_SET ? e[2 * i] : e[2 * i + 1], e[2 * i], this_v}, r;
            TRY(call_function(fn.u.o, ARG(1), cb, 3, &r));
        }
        *ret = v_undef();
        return C_NORMAL;
    }
    default: {
        JsObj *out = new_array(0);
        for (i = 0; i < m->extra_len; i++) {
            if (e[2 * i].t == V_UNDEF && e[2 * i + 1].t == V_NULL) continue;
            if (mode == 8 || (mode == 9 && m->cls == CL_SET)) arr_push(out, e[2 * i]);
            else if (mode == 9) arr_push(out, e[2 * i + 1]);
            else {
                JsObj *pair = new_array(2);
                arr_push(pair, e[2 * i]);
                arr_push(pair, m->cls == CL_SET ? e[2 * i] : e[2 * i + 1]);
                arr_push(out, v_obj(pair));
            }
        }
        *ret = v_obj(make_iterator(out));
        return C_NORMAL;
    }
    }
}

/* ================================================================ Promise */

/* A promise: magic 0 pending, 1 fulfilled, 2 rejected; this_val its value;
   target: an array of reactions [onFulfilled, onRejected, child]. */
static JsObj *microtasks; /* an array of jobs: [kind, a, b, c] */

static void queue_job(int kind, Value a, Value b, Value c) {
    JsObj *job = new_array(4);
    arr_push(job, v_num(kind));
    arr_push(job, a);
    arr_push(job, b);
    arr_push(job, c);
    arr_push(microtasks, v_obj(job));
}

static JsObj *promise_new(void) {
    JsObj *p = obj_new_raw(CL_PROMISE, PromiseProto);
    p->target = new_array(0);
    return p;
}

static void promise_settle(JsObj *p, int state, Value v) {
    if (p->magic) return;
    p->magic = state;
    p->this_val = v;
    JsObj *reactions = p->target;
    for (int i = 0; reactions && i < reactions->alen; i++) {
        JsObj *r = reactions->arr[i].u.o;
        Value h = r->arr[state == 1 ? 0 : 1];
        if (is_callable(h)) queue_job(1, h, v, r->arr[2]);
        else queue_job(3, v_num(state), v, r->arr[2]); /* no handler: the outcome passes on */
    }
    p->target = NULL;
}

static JsObj *make_resolver(JsObj *p, int reject);

/* Resolve p with v: adopting v's state if it is a promise (or thenable). */
static void promise_resolve(JsObj *p, Value v) {
    if (p->magic) return;
    if (v.t == V_OBJ && v.u.o == p) {
        promise_settle(p, 2, v_obj(make_error(TypeErrorProto, "Chaining cycle detected for promise")));
        return;
    }
    if (v.t == V_OBJ) {
        Value then;
        if (get_value(v, S_then, &then) != C_NORMAL) {
            Value e = js_exception;
            js_exception = v_undef();
            promise_settle(p, 2, e);
            return;
        }
        if (is_callable(then)) {
            queue_job(2, then, v, v_obj(p)); /* call then(resolve, reject) later */
            return;
        }
    }
    promise_settle(p, 1, v);
}

NATIVE(resolver_call) {
    UNUSED;
    JsObj *p = self->target;
    JsObj *shared = self->home; /* an "already resolved" flag shared by the pair */
    if (shared && shared->magic) {
        *ret = v_undef();
        return C_NORMAL;
    }
    if (shared) shared->magic = 1;
    if ((self->magic & 0xFFFF) == 1) promise_settle(p, 2, ARG(0));
    else promise_resolve(p, ARG(0));
    *ret = v_undef();
    return C_NORMAL;
}

static JsObj *make_resolver(JsObj *p, int reject) {
    JsObj *f = make_native(NULL, resolver_call, 1, reject);
    f->target = p;
    return f;
}

static void make_resolver_pair(JsObj *p, JsObj **res, JsObj **rej) {
    JsObj *flag = new_object();
    *res = make_resolver(p, 0);
    *rej = make_resolver(p, 1);
    (*res)->home = flag;
    (*rej)->home = flag;
}

/* Run the queued promise jobs (after every script, event and timer). */
static void run_microtasks(void) {
    for (int guard = 0; microtasks && microtasks->alen && guard < 100000; guard++) {
        Value jv = microtasks->arr[0];
        memmove(microtasks->arr, microtasks->arr + 1, sizeof(Value) * (size_t)(microtasks->alen - 1));
        microtasks->alen--;
        JsObj *job = jv.u.o;
        int kind = (int)job->arr[0].u.n;
        Value a = job->arr[1], b = job->arr[2], c = job->arr[3];
        char here;
        js_stack_base = &here;
        js_steps = 0;
        js_abort = 0;
        if (kind == 1) { /* a reaction: handler a, value b, child promise c (state in the reaction) */
            JsObj *child = c.t == V_OBJ ? c.u.o : NULL;
            Value r;
            int code;
            if (is_callable(a)) {
                code = call_function(a.u.o, v_undef(), &b, 1, &r);
                if (code == C_THROW) {
                    r = js_exception;
                    js_exception = v_undef();
                    if (js_abort) {
                        js_report("a promise handler ran too long and was stopped");
                        js_abort = 0;
                    }
                }
                if (child) {
                    if (code == C_THROW) promise_settle(child, 2, r);
                    else promise_resolve(child, r);
                }
            }
        } else if (kind == 3) { /* a pass-through: settle child c with state a, value b */
            JsObj *child = c.u.o;
            if ((int)a.u.n == 1) promise_resolve(child, b);
            else promise_settle(child, 2, b);
        } else if (kind == 2) { /* thenable: call a (then) on b with resolvers for c */
            JsObj *res, *rej;
            make_resolver_pair(c.u.o, &res, &rej);
            Value cb[2] = {v_obj(res), v_obj(rej)}, r;
            if (call_function(a.u.o, b, cb, 2, &r) == C_THROW) {
                Value e = js_exception;
                js_exception = v_undef();
                Value ignored;
                call_function(rej, v_undef(), &e, 1, &ignored);
            }
        } else if (kind == 4) { /* queueMicrotask(fn) */
            if (js_call_toplevel(a, v_undef(), NULL, 0, NULL) != 0) {
            }
        }
    }
}

NATIVE(promise_ctor) {
    UNUSED;
    if (!native_new_target) return throw_type("Promise constructor cannot be invoked without 'new'", NULL, NULL);
    Value exec_fn = ARG(0);
    if (!is_callable(exec_fn)) return throw_type("Promise resolver is not a function", NULL, NULL);
    JsObj *p = promise_new();
    JsObj *res, *rej;
    make_resolver_pair(p, &res, &rej);
    Value cb[2] = {v_obj(res), v_obj(rej)}, r;
    if (call_function(exec_fn.u.o, v_undef(), cb, 2, &r) == C_THROW) {
        if (js_abort) return C_THROW;
        Value e = js_exception;
        js_exception = v_undef();
        Value ignored;
        TRY(call_function(rej, v_undef(), &e, 1, &ignored));
    }
    *ret = v_obj(p);
    return C_NORMAL;
}

/* p.then(onFulfilled, onRejected) */
static JsObj *promise_then(JsObj *p, Value on_ful, Value on_rej) {
    JsObj *child = promise_new();
    /* without a handler, the outcome passes through: use pass-through jobs */
    if (p->magic == 0) {
        JsObj *r = new_array(4);
        arr_push(r, on_ful);
        arr_push(r, on_rej);
        arr_push(r, v_obj(child));
        arr_push(r, v_num(0));
        if (!is_callable(on_ful) || !is_callable(on_rej)) {
            /* store pass-through markers: handled when settled */
            if (!is_callable(on_ful)) r->arr[0] = v_null();
            if (!is_callable(on_rej)) r->arr[1] = v_null();
        }
        if (p->target) arr_push(p->target, v_obj(r));
    } else {
        Value h = p->magic == 1 ? on_ful : on_rej;
        if (is_callable(h)) queue_job(1, h, p->this_val, v_obj(child));
        else queue_job(3, v_num(p->magic), p->this_val, v_obj(child));
    }
    return child;
}

NATIVE(promise_then_native) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_PROMISE) return throw_type("Method Promise.prototype.then called on incompatible receiver", NULL, NULL);
    int mode = self->magic & 0xFFFF; /* 0 then, 1 catch, 2 finally */
    Value f = mode == 0 ? ARG(0) : v_undef(), r = mode == 0 ? ARG(1) : ARG(0);
    if (mode == 2) {
        /* finally(fn): fn runs either way, and the outcome passes through */
        Value fn = ARG(0);
        JsObj *p = this_v.u.o;
        if (is_callable(fn)) promise_then(p, fn, fn);
        *ret = v_obj(promise_then(p, v_undef(), v_undef()));
        return C_NORMAL;
    }
    *ret = v_obj(promise_then(this_v.u.o, f, r));
    return C_NORMAL;
}

/* magic: 0 resolve, 1 reject */
NATIVE(promise_static_resolve) {
    UNUSED;
    Value v = ARG(0);
    if ((self->magic & 0xFFFF) == 0 && v.t == V_OBJ && v.u.o->cls == CL_PROMISE) {
        *ret = v;
        return C_NORMAL;
    }
    JsObj *p = promise_new();
    if (self->magic & 0xFFFF) promise_settle(p, 2, v);
    else promise_resolve(p, v);
    *ret = v_obj(p);
    return C_NORMAL;
}

/* Promise.all / allSettled / race / any: magic 0 all, 1 allSettled, 2 race, 3 any.
   The combined promise is settled when the inputs settle, checked by an
   aggregator native per input. */
NATIVE(promise_aggregate_step) {
    UNUSED;
    /* self->target: the state object {result array, remaining, out promise, mode}; magic: index | rejected flag */
    JsObj *st = self->target;
    JsObj *results = st->arr[0].u.o, *out = st->arr[2].u.o;
    int mode = (int)st->arr[3].u.n, index = self->magic & 0x7FFF, rejected = (self->magic >> 15) & 1;
    Value v = ARG(0);
    *ret = v_undef();
    if (out->magic) return C_NORMAL;
    if (mode == 2) {
        promise_settle(out, rejected ? 2 : 1, v);
        return C_NORMAL;
    }
    if (mode == 0 && rejected) {
        promise_settle(out, 2, v);
        return C_NORMAL;
    }
    if (mode == 3 && !rejected) {
        promise_settle(out, 1, v);
        return C_NORMAL;
    }
    if (mode == 1) {
        JsObj *o = new_object();
        prop_set_raw(o, intern_c("status"), v_str(intern_c(rejected ? "rejected" : "fulfilled")), 0);
        prop_set_raw(o, intern_c(rejected ? "reason" : "value"), v, 0);
        v = v_obj(o);
    }
    if (index < results->alen) results->arr[index] = v;
    st->arr[1] = v_num(st->arr[1].u.n - 1);
    if (st->arr[1].u.n <= 0) {
        if (mode == 3) promise_settle(out, 2, v_obj(make_error(ErrorProto, "All promises were rejected")));
        else promise_settle(out, 1, v_obj(results));
    }
    return C_NORMAL;
}

NATIVE(promise_aggregate) {
    UNUSED;
    JsObj *items;
    TRY(iterate_to_array(ARG(0), &items));
    int mode = self->magic & 0xFFFF;
    JsObj *out = promise_new();
    JsObj *results = new_array(items->alen);
    arr_set_len(results, items->alen);
    JsObj *st = new_array(4);
    arr_push(st, v_obj(results));
    arr_push(st, v_num(items->alen));
    arr_push(st, v_obj(out));
    arr_push(st, v_num(mode));
    if (items->alen == 0) {
        if (mode == 3) promise_settle(out, 2, v_obj(make_error(ErrorProto, "All promises were rejected")));
        else if (mode != 2) promise_settle(out, 1, v_obj(results));
    }
    for (int i = 0; i < items->alen && i < 0x7FFF; i++) {
        Value item = items->arr[i];
        JsObj *p;
        if (item.t == V_OBJ && item.u.o->cls == CL_PROMISE) {
            p = item.u.o;
        } else {
            p = promise_new();
            promise_resolve(p, item);
        }
        JsObj *ok = make_native(NULL, promise_aggregate_step, 1, i);
        JsObj *bad = make_native(NULL, promise_aggregate_step, 1, i | 0x8000);
        ok->target = st;
        bad->target = st;
        promise_then(p, v_obj(ok), v_obj(bad));
    }
    *ret = v_obj(out);
    return C_NORMAL;
}

NATIVE(queue_microtask) {
    UNUSED;
    if (!is_callable(ARG(0))) return throw_type("queueMicrotask: argument is not a function", NULL, NULL);
    queue_job(4, ARG(0), v_undef(), v_undef());
    *ret = v_undef();
    return C_NORMAL;
}

/* ================================================================ errors, Symbol, globals */

NATIVE(error_ctor) {
    UNUSED;
    JsObj *proto = self->home ? self->home : ErrorProto;
    JsObj *e;
    if (native_new_target && this_v.t == V_OBJ) {
        e = this_v.u.o;
        e->cls = CL_ERROR;
    } else {
        e = obj_new_raw(CL_ERROR, proto);
    }
    Value msg = ARG(0);
    if (msg.t != V_UNDEF) {
        JsStr *s;
        TRY(to_string(msg, &s));
        prop_set_raw(e, S_message, v_str(s), P_HIDDEN);
    }
    Value opts = ARG(1);
    if (opts.t == V_OBJ) {
        Value cause;
        TRY(get_value(opts, intern_c("cause"), &cause));
        if (cause.t != V_UNDEF) prop_set_raw(e, intern_c("cause"), cause, P_HIDDEN);
    }
    prop_set_raw(e, S_stack, v_str(intern_c("    at <anonymous>")), P_HIDDEN);
    *ret = v_obj(e);
    return C_NORMAL;
}

NATIVE(error_to_string) {
    UNUSED;
    Value name, msg;
    TRY(get_value(this_v, S_name, &name));
    TRY(get_value(this_v, S_message, &msg));
    JsStr *n = intern_c("Error"), *m = S_empty;
    if (name.t != V_UNDEF) TRY(to_string(name, &n));
    if (msg.t != V_UNDEF) TRY(to_string(msg, &m));
    Buf b = {0};
    buf_add(&b, n->s, n->len);
    if (n->len && m->len) buf_c(&b, ": ");
    buf_add(&b, m->s, m->len);
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

static int symbol_counter;

NATIVE(symbol_ctor) {
    UNUSED;
    if (native_new_target) return throw_type("Symbol is not a constructor", NULL, NULL);
    Buf b = {0};
    char n[16];
    number_to_text(++symbol_counter, n);
    buf_c(&b, "@@symbol:");
    buf_c(&b, n);
    buf_ch(&b, ':');
    if (ARG(0).t != V_UNDEF) {
        JsStr *d;
        TRY(to_string(ARG(0), &d));
        buf_add(&b, d->s, d->len);
    }
    *ret = v_str(interned(buf_done(&b)));
    return C_NORMAL;
}

NATIVE(symbol_for) {
    UNUSED;
    JsStr *d;
    TRY(to_string(ARG(0), &d));
    Buf b = {0};
    buf_c(&b, "@@symbol.for:");
    buf_add(&b, d->s, d->len);
    *ret = v_str(interned(buf_done(&b)));
    return C_NORMAL;
}

/* encodeURIComponent (magic 0), encodeURI (1), decodeURIComponent (2), decodeURI (3), escape (4), unescape (5) */
NATIVE(uri_code) {
    UNUSED;
    JsStr *s;
    TRY(to_string(ARG(0), &s));
    int mode = self->magic & 0xFFFF;
    Buf b = {0};
    if (mode == 0 || mode == 1 || mode == 4) {
        const char *keep = mode == 0 ? "-_.!~*'()" : mode == 1 ? "-_.!~*'();/?:@&=+$,#" : "@*_+-./";
        for (int i = 0; i < s->len; i++) {
            unsigned char c = (unsigned char)s->s[i];
            if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c && strchr(keep, c))) {
                buf_ch(&b, (char)c);
            } else {
                char t[4] = {'%', "0123456789ABCDEF"[c >> 4], "0123456789ABCDEF"[c & 15], 0};
                buf_c(&b, t);
            }
        }
    } else {
        for (int i = 0; i < s->len; i++) {
            if (s->s[i] == '%' && i + 2 < s->len && hex_digit(s->s[i + 1]) >= 0 && hex_digit(s->s[i + 2]) >= 0) {
                char c = (char)(hex_digit(s->s[i + 1]) * 16 + hex_digit(s->s[i + 2]));
                if (mode == 3 && c && strchr(";/?:@&=+$,#", c)) {
                    buf_add(&b, s->s + i, 3);
                } else {
                    buf_ch(&b, c);
                }
                i += 2;
            } else if (s->s[i] == '%' && mode != 5) {
                free(b.p);
                return throw_error(ErrorProto, "URI malformed");
            } else {
                buf_ch(&b, s->s[i]);
            }
        }
    }
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(global_eval) {
    UNUSED;
    Value code = ARG(0);
    if (code.t != V_STR) {
        *ret = code;
        return C_NORMAL;
    }
    Ast *program = parse_script(code.u.s->s, code.u.s->len);
    if (!program) return throw_error(SyntaxErrorProto, parse_error);
    Frame f;
    memset(&f, 0, sizeof(f));
    f.env = global_lex;
    f.var_env = global_env;
    f.this_v = v_obj(global_obj);
    *ret = v_undef();
    int r = exec_list(program->list, &f, ret);
    return r == C_THROW ? C_THROW : C_NORMAL;
}

/* ================================================================ typed arrays */

static JsObj *make_ctor(const char *name, Native fn, int nargs, int magic, JsObj *proto);

static JsObj *TypedArrayProto;
static const int TYPED_BYTES[10] = {0, 1, 1, 1, 2, 2, 4, 4, 4, 8};
static const char *TYPED_NAMES[10] = {NULL, "Int8Array", "Uint8Array", "Uint8ClampedArray", "Int16Array", "Uint16Array",
                                      "Int32Array", "Uint32Array", "Float32Array", "Float64Array"};

static JsObj *typed_new(int kind, int length) {
    JsObj *a = new_array(length);
    a->fn_kind = (unsigned char)kind;
    a->proto = TypedArrayProto;
    for (int i = 0; i < length; i++) arr_push(a, v_num(0));
    return a;
}

NATIVE(typed_ctor) {
    UNUSED;
    int kind = self->magic & 0xFFFF;
    Value src = ARG(0);
    JsObj *a;
    if (src.t == V_UNDEF || src.t == V_NUM) {
        double n = src.t == V_NUM ? src.u.n : 0;
        if (n < 0 || n != js_trunc(n) || n > 50000000) return throw_error(RangeErrorProto, "Invalid typed array length");
        a = typed_new(kind, (int)n);
    } else if (src.t == V_OBJ && src.u.o->cls == CL_OBJECT && own_prop(src.u.o, intern_c("byteLength"))) { /* an ArrayBuffer */
        Value bl;
        TRY(get_value(src, intern_c("byteLength"), &bl));
        double off = 0, len;
        TRY(arg_int(args, argc, 1, 0, &off));
        TRY(arg_int(args, argc, 2, (bl.u.n - off) / TYPED_BYTES[kind], &len));
        a = typed_new(kind, len < 0 ? 0 : (int)len);
    } else {
        JsObj *items;
        TRY(iterate_to_array(src, &items));
        a = typed_new(kind, items->alen);
        for (int i = 0; i < items->alen; i++) TRY(typed_put(a, i, items->arr[i]));
    }
    *ret = v_obj(a);
    return C_NORMAL;
}

/* set(array, offset) */
NATIVE(typed_set) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("not a typed array", NULL, NULL);
    JsObj *a = this_v.u.o, *items;
    TRY(iterate_to_array(ARG(0), &items));
    double off;
    TRY(arg_int(args, argc, 1, 0, &off));
    if (off < 0 || off + items->alen > a->alen) return throw_error(RangeErrorProto, "offset is out of bounds");
    for (int i = 0; i < items->alen; i++) TRY(put_index(this_v, v_num(off + i), items->arr[i]));
    *ret = v_undef();
    return C_NORMAL;
}

/* subarray / slice (copies: views over shared memory are not modelled) */
NATIVE(typed_subarray) {
    UNUSED;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_ARRAY) return throw_type("not a typed array", NULL, NULL);
    JsObj *a = this_v.u.o;
    double s, e;
    TRY(arg_int(args, argc, 0, 0, &s));
    TRY(arg_int(args, argc, 1, a->alen, &e));
    int from = rel_index(s, a->alen), to = rel_index(e, a->alen);
    JsObj *out = typed_new(a->fn_kind ? a->fn_kind : 9, to > from ? to - from : 0);
    for (int i = from; i < to; i++) out->arr[i - from] = a->arr[i];
    *ret = v_obj(out);
    return C_NORMAL;
}

NATIVE(array_buffer_ctor) {
    UNUSED;
    double n;
    TRY(arg_int(args, argc, 0, 0, &n));
    JsObj *b = new_object();
    prop_set_raw(b, intern_c("byteLength"), v_num(n < 0 ? 0 : n), P_HIDDEN);
    *ret = v_obj(b);
    return C_NORMAL;
}

/* TextEncoder().encode(s): the UTF-8 bytes; TextDecoder().decode(bytes): the text */
NATIVE(text_encode) {
    UNUSED;
    JsStr *s = S_empty;
    if (ARG(0).t != V_UNDEF) TRY(to_string(ARG(0), &s));
    JsObj *a = typed_new(2, s->len);
    for (int i = 0; i < s->len; i++) a->arr[i] = v_num((unsigned char)s->s[i]);
    *ret = v_obj(a);
    return C_NORMAL;
}

NATIVE(text_decode) {
    UNUSED;
    Buf b = {0};
    if (ARG(0).t == V_OBJ) {
        JsObj *items;
        TRY(iterate_to_array(ARG(0), &items));
        for (int i = 0; i < items->alen; i++) {
            double d;
            TRY(to_number(items->arr[i], &d));
            buf_ch(&b, (char)(to_int32(d) & 255));
        }
    }
    *ret = v_str(buf_done(&b));
    return C_NORMAL;
}

NATIVE(coder_ctor) {
    UNUSED;
    JsObj *o = new_object();
    if ((self->magic & 0xFFFF) == 0) define_method(o, "encode", text_encode, 1, 0);
    else define_method(o, "decode", text_decode, 1, 0);
    prop_set_raw(o, intern_c("encoding"), v_str(intern_c("utf-8")), 0);
    *ret = v_obj(o);
    return C_NORMAL;
}

NATIVE(get_random_values) {
    UNUSED;
    Value a = ARG(0);
    if (a.t != V_OBJ || a.u.o->cls != CL_ARRAY) return throw_type("getRandomValues: argument is not a typed array", NULL, NULL);
    JsObj *arr = a.u.o;
    for (int i = 0; i < arr->alen; i++) {
        unsigned r;
        getrandom(&r, sizeof(r));
        double d = arr->fn_kind == 8 || arr->fn_kind == 9 ? js_random() : (double)r;
        arr->arr[i] = v_num(arr->fn_kind ? typed_coerce(arr->fn_kind, d) : d);
    }
    *ret = a;
    return C_NORMAL;
}

/* Reflect: magic 0 construct, 1 apply, 2 ownKeys, 3 has, 4 get, 5 set, 6 getPrototypeOf,
   7 setPrototypeOf, 8 defineProperty, 9 deleteProperty, 10 getOwnPropertyDescriptor */
NATIVE(reflect_method) {
    UNUSED;
    Value t = ARG(0);
    int mode = self->magic & 0xFFFF;
    if (t.t != V_OBJ) return throw_type("Reflect method called on non-object", NULL, NULL);
    switch (mode) {
    case 0: {
        JsObj *items;
        TRY(iterate_to_array(ARG(1), &items));
        Value nt = argc > 2 ? args[2] : t;
        if (!is_callable(t) || !is_callable(nt)) return throw_type("Reflect.construct: not a constructor", NULL, NULL);
        return construct(t.u.o, items->arr, items->alen, nt.u.o, ret);
    }
    case 1: {
        JsObj *items;
        TRY(iterate_to_array(ARG(2), &items));
        if (!is_callable(t)) return throw_type("Reflect.apply: not a function", NULL, NULL);
        return call_function(t.u.o, ARG(1), items->arr, items->alen, ret);
    }
    case 2: {
        JsObj *keys = new_array(0);
        for (int i = 0; i < t.u.o->alen && (t.u.o->cls == CL_ARRAY || t.u.o->cls == CL_ARGUMENTS); i++) arr_push(keys, v_str(number_str(i)));
        for (int i = 0; i < t.u.o->nprops; i++) arr_push(keys, v_str(t.u.o->props[i].key));
        *ret = v_obj(keys);
        return C_NORMAL;
    }
    case 3: {
        JsStr *k;
        TRY(to_key(ARG(1), &k));
        *ret = v_bool(has_property(t.u.o, k));
        return C_NORMAL;
    }
    case 4: return get_index(t, ARG(1), ret);
    case 5: TRY(put_index(t, ARG(1), ARG(2))); *ret = v_bool(1); return C_NORMAL;
    case 6: *ret = t.u.o->proto ? v_obj(t.u.o->proto) : v_null(); return C_NORMAL;
    case 7: t.u.o->proto = ARG(1).t == V_OBJ ? ARG(1).u.o : NULL; *ret = v_bool(1); return C_NORMAL;
    case 8: {
        Value r;
        TRY(object_define_property(self, this_v, args, argc, &r));
        *ret = v_bool(1);
        return C_NORMAL;
    }
    case 9: {
        JsStr *k;
        TRY(to_key(ARG(1), &k));
        *ret = v_bool(prop_delete(t.u.o, k) || 1);
        return C_NORMAL;
    }
    case 10: return object_get_own_property_descriptor(self, this_v, args, argc, ret);
    }
    *ret = v_undef();
    return C_NORMAL;
}

static void init_reflect(void) {
    JsObj *r = new_object();
    define_hidden(global_obj, "Reflect", v_obj(r));
    static const char *names[] = {"construct", "apply", "ownKeys", "has", "get", "set", "getPrototypeOf", "setPrototypeOf",
                                  "defineProperty", "deleteProperty", "getOwnPropertyDescriptor"};
    for (int i = 0; i < 11; i++) define_method(r, names[i], reflect_method, 2, i);
}

static void init_iterators(void) {
    IteratorProto = obj_new_raw(CL_OBJECT, ObjectProto);
    define_method(IteratorProto, "next", iterator_next, 0, 0);
    define_method(IteratorProto, "@@iterator", iterator_self, 0, 0);
    define_method(ArrayProto, "@@iterator", values_iterator, 0, 0);
    define_method(StringProto, "@@iterator", values_iterator, 0, 0);
    define_method(MapProto, "@@iterator", map_method, 0, 10);
    define_method(SetProto, "@@iterator", map_method, 0, 9);
}

static void init_typed_arrays(void) {
    TypedArrayProto = obj_new_raw(CL_OBJECT, ArrayProto);
    define_method(TypedArrayProto, "set", typed_set, 2, 0);
    define_method(TypedArrayProto, "subarray", typed_subarray, 2, 0);
    define_method(TypedArrayProto, "slice", typed_subarray, 2, 0);
    for (int k = 1; k <= 9; k++) {
        JsObj *c = make_ctor(TYPED_NAMES[k], typed_ctor, 3, k, NULL);
        prop_set_raw(c, S_prototype, v_obj(TypedArrayProto), P_HIDDEN);
        define_hidden(c, "BYTES_PER_ELEMENT", v_num(TYPED_BYTES[k]));
        define_method(c, "from", typed_ctor, 1, k);
    }
    make_ctor("ArrayBuffer", array_buffer_ctor, 1, 0, NULL);
    make_ctor("TextEncoder", coder_ctor, 0, 0, NULL);
    make_ctor("TextDecoder", coder_ctor, 0, 1, NULL);
}

/* ================================================================ setting it all up */

static JsObj *make_ctor(const char *name, Native fn, int nargs, int magic, JsObj *proto) {
    JsObj *c = make_native(name, fn, nargs, magic);
    c->fn_kind = FN_NORMAL;
    if (proto) {
        prop_set_raw(c, S_prototype, v_obj(proto), P_HIDDEN);
        prop_set_raw(proto, S_constructor, v_obj(c), P_HIDDEN);
    }
    define_hidden(global_obj, name, v_obj(c));
    return c;
}

static JsObj *error_type(const char *name, JsObj *proto) {
    JsObj *c = make_ctor(name, error_ctor, 1, 0, proto);
    c->home = proto;
    prop_set_raw(proto, S_name, v_str(intern_c(name)), P_HIDDEN);
    prop_set_raw(proto, S_message, v_str(S_empty), P_HIDDEN);
    return c;
}

static void js_init_builtins(void) {
    init_names();
    ObjectProto = obj_new_raw(CL_OBJECT, NULL);
    FunctionProto = obj_new_raw(CL_FUNCTION, ObjectProto);
    FunctionProto->native = noop;
    ArrayProto = obj_new_raw(CL_ARRAY, ObjectProto);
    StringProto = obj_new_raw(CL_STRING, ObjectProto);
    StringProto->this_val = v_str(S_empty);
    NumberProto = obj_new_raw(CL_NUMBER, ObjectProto);
    NumberProto->this_val = v_num(0);
    BooleanProto = obj_new_raw(CL_BOOLEAN, ObjectProto);
    BooleanProto->this_val = v_bool(0);
    ErrorProto = obj_new_raw(CL_OBJECT, ObjectProto);
    TypeErrorProto = obj_new_raw(CL_OBJECT, ErrorProto);
    RangeErrorProto = obj_new_raw(CL_OBJECT, ErrorProto);
    SyntaxErrorProto = obj_new_raw(CL_OBJECT, ErrorProto);
    ReferenceErrorProto = obj_new_raw(CL_OBJECT, ErrorProto);
    DateProto = obj_new_raw(CL_OBJECT, ObjectProto);
    RegExpProto = obj_new_raw(CL_OBJECT, ObjectProto);
    MapProto = obj_new_raw(CL_OBJECT, ObjectProto);
    SetProto = obj_new_raw(CL_OBJECT, ObjectProto);
    PromiseProto = obj_new_raw(CL_OBJECT, ObjectProto);
    global_obj = obj_new_raw(CL_OBJECT, ObjectProto);
    global_env = env_new(NULL);
    global_env->object = global_obj;
    global_lex = env_new(global_env);
    microtasks = new_array(0);

    /* Object */
    JsObj *O = make_ctor("Object", object_ctor, 1, 0, ObjectProto);
    define_method(O, "keys", object_keys, 1, 0);
    define_method(O, "values", object_keys, 1, 1);
    define_method(O, "entries", object_keys, 1, 2);
    define_method(O, "getOwnPropertyNames", object_keys, 1, 3);
    define_method(O, "assign", object_assign, 2, 0);
    define_method(O, "create", object_create, 2, 0);
    define_method(O, "defineProperty", object_define_property, 3, 0);
    define_method(O, "defineProperties", object_define_properties, 2, 0);
    define_method(O, "getOwnPropertyDescriptor", object_get_own_property_descriptor, 2, 0);
    define_method(O, "getPrototypeOf", object_get_prototype_of, 1, 0);
    define_method(O, "setPrototypeOf", object_set_prototype_of, 2, 0);
    define_method(O, "freeze", object_freeze, 1, 0);
    define_method(O, "seal", object_freeze, 1, 2);
    define_method(O, "preventExtensions", object_freeze, 1, 2);
    define_method(O, "isFrozen", object_freeze, 1, 1);
    define_method(O, "isSealed", object_freeze, 1, 1);
    define_method(O, "isExtensible", object_freeze, 1, 3);
    define_method(O, "fromEntries", object_from_entries, 1, 0);
    define_method(O, "is", object_is, 2, 0);
    define_method(O, "hasOwn", object_has_own, 2, 1);
    define_method(O, "getOwnPropertySymbols", object_keys, 1, 4);
    define_method(ObjectProto, "hasOwnProperty", object_has_own, 1, 0);
    define_method(ObjectProto, "isPrototypeOf", object_is_prototype_of, 1, 0);
    define_method(ObjectProto, "propertyIsEnumerable", object_property_is_enumerable, 1, 0);
    define_method(ObjectProto, "toString", object_to_string, 0, 0);
    define_method(ObjectProto, "toLocaleString", object_to_string, 0, 0);
    define_method(ObjectProto, "valueOf", object_value_of, 0, 0);

    /* Function */
    make_ctor("Function", function_ctor, 1, 0, FunctionProto);
    define_method(FunctionProto, "call", function_call, 1, 0);
    define_method(FunctionProto, "apply", function_apply, 2, 0);
    define_method(FunctionProto, "bind", function_bind, 1, 0);
    define_method(FunctionProto, "toString", function_to_string, 0, 0);

    /* Array */
    JsObj *A = make_ctor("Array", array_ctor, 1, 0, ArrayProto);
    define_method(A, "isArray", array_is_array, 1, 0);
    define_method(A, "from", array_from, 1, 0);
    define_method(A, "of", array_of, 0, 0);
    define_method(ArrayProto, "push", array_push, 1, 0);
    define_method(ArrayProto, "pop", array_pop, 0, 0);
    define_method(ArrayProto, "shift", array_shift, 0, 0);
    define_method(ArrayProto, "unshift", array_unshift, 1, 0);
    define_method(ArrayProto, "slice", array_slice, 2, 0);
    define_method(ArrayProto, "splice", array_splice, 2, 0);
    define_method(ArrayProto, "concat", array_concat, 1, 0);
    define_method(ArrayProto, "join", array_join, 1, 0);
    define_method(ArrayProto, "toString", array_join, 0, 0);
    define_method(ArrayProto, "toLocaleString", array_join, 0, 0);
    define_method(ArrayProto, "reverse", array_reverse, 0, 0);
    define_method(ArrayProto, "indexOf", array_index_of, 1, 0);
    define_method(ArrayProto, "lastIndexOf", array_index_of, 1, 1);
    define_method(ArrayProto, "includes", array_index_of, 1, 2);
    static const char *iters[] = {"forEach", "map", "filter", "find", "findIndex", "some", "every", "findLast", "findLastIndex", "flatMap"};
    for (int i = 0; i < 10; i++) define_method(ArrayProto, iters[i], array_iterate, 1, i);
    define_method(ArrayProto, "reduce", array_reduce, 1, 0);
    define_method(ArrayProto, "reduceRight", array_reduce, 1, 1);
    define_method(ArrayProto, "sort", array_sort, 1, 0);
    define_method(ArrayProto, "flat", array_flat, 0, 0);
    define_method(ArrayProto, "fill", array_fill, 1, 0);
    define_method(ArrayProto, "keys", array_keys, 0, 0);
    define_method(ArrayProto, "values", array_keys, 0, 1);
    define_method(ArrayProto, "entries", array_keys, 0, 2);
    define_method(ArrayProto, "at", array_at, 1, 0);

    /* String */
    JsObj *S = make_ctor("String", string_ctor, 1, 0, StringProto);
    define_method(S, "fromCharCode", string_from_char_code, 1, 0);
    define_method(S, "fromCodePoint", string_from_char_code, 1, 1);
    define_method(StringProto, "toString", string_to_string, 0, 0);
    define_method(StringProto, "valueOf", string_to_string, 0, 0);
    define_method(StringProto, "normalize", string_to_string, 0, 0);
    define_method(StringProto, "charAt", string_char_at, 1, 0);
    define_method(StringProto, "charCodeAt", string_char_at, 1, 1);
    define_method(StringProto, "at", string_char_at, 1, 2);
    define_method(StringProto, "codePointAt", string_char_at, 1, 3);
    define_method(StringProto, "indexOf", string_index_of, 1, 0);
    define_method(StringProto, "lastIndexOf", string_index_of, 1, 1);
    define_method(StringProto, "includes", string_index_of, 1, 2);
    define_method(StringProto, "startsWith", string_index_of, 1, 3);
    define_method(StringProto, "endsWith", string_index_of, 1, 4);
    define_method(StringProto, "slice", string_slice, 2, 0);
    define_method(StringProto, "substring", string_slice, 2, 1);
    define_method(StringProto, "substr", string_slice, 2, 2);
    define_method(StringProto, "toUpperCase", string_case, 0, 0);
    define_method(StringProto, "toLocaleUpperCase", string_case, 0, 0);
    define_method(StringProto, "toLowerCase", string_case, 0, 1);
    define_method(StringProto, "toLocaleLowerCase", string_case, 0, 1);
    define_method(StringProto, "trim", string_trim, 0, 0);
    define_method(StringProto, "trimStart", string_trim, 0, 1);
    define_method(StringProto, "trimLeft", string_trim, 0, 1);
    define_method(StringProto, "trimEnd", string_trim, 0, 2);
    define_method(StringProto, "trimRight", string_trim, 0, 2);
    define_method(StringProto, "padStart", string_pad, 2, 0);
    define_method(StringProto, "padEnd", string_pad, 2, 1);
    define_method(StringProto, "repeat", string_repeat, 1, 0);
    define_method(StringProto, "concat", string_concat, 1, 0);
    define_method(StringProto, "localeCompare", string_locale_compare, 1, 0);
    define_method(StringProto, "replace", string_replace, 2, 0);
    define_method(StringProto, "replaceAll", string_replace, 2, 1);
    define_method(StringProto, "split", string_split, 2, 0);
    define_method(StringProto, "match", string_match, 1, 0);
    define_method(StringProto, "matchAll", string_match, 1, 1);
    define_method(StringProto, "search", string_match, 1, 2);

    /* Number, Boolean */
    JsObj *N = make_ctor("Number", number_ctor, 1, 0, NumberProto);
    define_method(NumberProto, "toString", number_to_string, 1, 0);
    define_method(NumberProto, "toFixed", number_to_fixed, 1, 0);
    define_method(NumberProto, "toPrecision", number_to_precision, 1, 0);
    define_method(NumberProto, "toLocaleString", number_to_locale, 0, 0);
    define_method(NumberProto, "valueOf", number_value_of, 0, 0);
    define_method(N, "isNaN", number_test, 1, 2);
    define_method(N, "isFinite", number_test, 1, 3);
    define_method(N, "isInteger", number_test, 1, 4);
    define_method(N, "isSafeInteger", number_test, 1, 5);
    define_method(N, "parseFloat", parse_float, 1, 0);
    define_method(N, "parseInt", parse_int, 2, 0);
    define_hidden(N, "MAX_SAFE_INTEGER", v_num(9007199254740991.0));
    define_hidden(N, "MIN_SAFE_INTEGER", v_num(-9007199254740991.0));
    define_hidden(N, "EPSILON", v_num(2.220446049250313e-16));
    define_hidden(N, "MAX_VALUE", v_num(1.7976931348623157e308));
    define_hidden(N, "MIN_VALUE", v_num(5e-324));
    define_hidden(N, "POSITIVE_INFINITY", v_num(js_inf()));
    define_hidden(N, "NEGATIVE_INFINITY", v_num(-js_inf()));
    define_hidden(N, "NaN", v_num(js_nan()));
    make_ctor("Boolean", boolean_ctor, 1, 0, BooleanProto);
    define_method(BooleanProto, "toString", boolean_to_string, 0, 0);
    define_method(BooleanProto, "valueOf", boolean_to_string, 0, 1);

    /* Math */
    JsObj *Mth = new_object();
    define_hidden(global_obj, "Math", v_obj(Mth));
    static const char *m1[] = {"abs", "floor", "ceil", "round", "trunc", "sign", "sqrt", "cbrt", "exp", "log", "log2", "log10", "sin",
                               "cos", "tan", "asin", "acos", "atan", "fround", "clz32", "log1p", "expm1", "sinh", "cosh", "tanh"};
    for (int i = 0; i < 25; i++) define_method(Mth, m1[i], math_1, 1, i);
    define_method(Mth, "min", math_n, 2, 0);
    define_method(Mth, "max", math_n, 2, 1);
    define_method(Mth, "pow", math_n, 2, 2);
    define_method(Mth, "atan2", math_n, 2, 3);
    define_method(Mth, "hypot", math_n, 2, 4);
    define_method(Mth, "imul", math_n, 2, 5);
    define_method(Mth, "random", math_random, 0, 0);
    define_hidden(Mth, "PI", v_num(PI));
    define_hidden(Mth, "E", v_num(2.718281828459045));
    define_hidden(Mth, "LN2", v_num(0.6931471805599453));
    define_hidden(Mth, "LN10", v_num(2.302585092994046));
    define_hidden(Mth, "LOG2E", v_num(1.4426950408889634));
    define_hidden(Mth, "LOG10E", v_num(0.4342944819032518));
    define_hidden(Mth, "SQRT2", v_num(1.4142135623730951));
    define_hidden(Mth, "SQRT1_2", v_num(0.7071067811865476));

    /* JSON */
    JsObj *J = new_object();
    define_hidden(global_obj, "JSON", v_obj(J));
    define_method(J, "parse", json_parse, 2, 0);
    define_method(J, "stringify", json_stringify, 3, 0);

    /* Date */
    JsObj *D = make_ctor("Date", date_ctor, 7, 0, DateProto);
    define_method(D, "now", date_now, 0, 0);
    define_method(D, "parse", date_parse, 1, 0);
    define_method(D, "UTC", date_utc, 7, 0);
    static const char *getters[] = {"getTime", "getFullYear", "getMonth", "getDate", "getDay", "getHours", "getMinutes", "getSeconds", "getMilliseconds", "getTimezoneOffset"};
    static const char *utc_getters[] = {"valueOf", "getUTCFullYear", "getUTCMonth", "getUTCDate", "getUTCDay", "getUTCHours", "getUTCMinutes", "getUTCSeconds", "getUTCMilliseconds", NULL};
    for (int i = 0; i < 10; i++) {
        define_method(DateProto, getters[i], date_get, 0, i);
        if (utc_getters[i]) define_method(DateProto, utc_getters[i], date_get, 0, i);
    }
    define_method(DateProto, "getYear", date_get, 0, 1);
    static const char *setters[] = {"setTime", "setFullYear", "setMonth", "setDate", NULL, "setHours", "setMinutes", "setSeconds", "setMilliseconds"};
    static const char *utc_setters[] = {NULL, "setUTCFullYear", "setUTCMonth", "setUTCDate", NULL, "setUTCHours", "setUTCMinutes", "setUTCSeconds", "setUTCMilliseconds"};
    for (int i = 0; i < 9; i++) {
        if (setters[i]) define_method(DateProto, setters[i], date_set, 1, i);
        if (utc_setters[i]) define_method(DateProto, utc_setters[i], date_set, 1, i);
    }
    static const char *formats[] = {"toISOString", "toString", "toDateString", "toTimeString", "toLocaleDateString", "toLocaleTimeString", "toLocaleString", "toUTCString"};
    for (int i = 0; i < 8; i++) define_method(DateProto, formats[i], date_format, 0, i);
    define_method(DateProto, "toJSON", date_format, 0, 0);
    define_method(DateProto, "toGMTString", date_format, 0, 7);

    /* RegExp */
    make_ctor("RegExp", regexp_ctor, 2, 0, RegExpProto);
    define_method(RegExpProto, "exec", regexp_exec, 1, 0);
    define_method(RegExpProto, "test", regexp_exec, 1, 1);
    define_method(RegExpProto, "toString", regexp_to_string, 0, 0);

    /* Map, Set (and the weak kinds, which here hold on strongly) */
    make_ctor("Map", map_ctor, 0, 0, MapProto);
    make_ctor("Set", map_ctor, 0, 1, SetProto);
    JsObj *wm = make_native("WeakMap", map_ctor, 0, 0);
    wm->fn_kind = FN_NORMAL;
    prop_set_raw(wm, S_prototype, v_obj(MapProto), P_HIDDEN);
    define_hidden(global_obj, "WeakMap", v_obj(wm));
    JsObj *ws = make_native("WeakSet", map_ctor, 0, 1);
    ws->fn_kind = FN_NORMAL;
    prop_set_raw(ws, S_prototype, v_obj(SetProto), P_HIDDEN);
    define_hidden(global_obj, "WeakSet", v_obj(ws));
    static const char *map_methods[] = {"get", "set", "has", "delete", "clear", "add", NULL, "forEach", "keys", "values", "entries"};
    for (int i = 0; i < 11; i++) {
        if (!map_methods[i]) continue;
        if (i != 5) define_method(MapProto, map_methods[i], map_method, 1, i);
        if (i != 0 && i != 1) define_method(SetProto, map_methods[i], map_method, 1, i);
    }
    define_getter(MapProto, "size", map_method, NULL, 6);
    define_getter(SetProto, "size", map_method, NULL, 6);

    /* Promise */
    JsObj *P = make_ctor("Promise", promise_ctor, 1, 0, PromiseProto);
    define_method(PromiseProto, "then", promise_then_native, 2, 0);
    define_method(PromiseProto, "catch", promise_then_native, 1, 1);
    define_method(PromiseProto, "finally", promise_then_native, 1, 2);
    define_method(P, "resolve", promise_static_resolve, 1, 0);
    define_method(P, "reject", promise_static_resolve, 1, 1);
    define_method(P, "all", promise_aggregate, 1, 0);
    define_method(P, "allSettled", promise_aggregate, 1, 1);
    define_method(P, "race", promise_aggregate, 1, 2);
    define_method(P, "any", promise_aggregate, 1, 3);
    define_method(global_obj, "queueMicrotask", queue_microtask, 1, 0);

    /* errors */
    error_type("Error", ErrorProto);
    define_method(ErrorProto, "toString", error_to_string, 0, 0);
    error_type("TypeError", TypeErrorProto);
    error_type("RangeError", RangeErrorProto);
    error_type("SyntaxError", SyntaxErrorProto);
    error_type("ReferenceError", ReferenceErrorProto);
    error_type("EvalError", obj_new_raw(CL_OBJECT, ErrorProto));
    error_type("URIError", obj_new_raw(CL_OBJECT, ErrorProto));

    /* Symbol */
    JsObj *Sym = make_native("Symbol", symbol_ctor, 0, 0);
    define_hidden(global_obj, "Symbol", v_obj(Sym));
    define_method(Sym, "for", symbol_for, 1, 0);
    define_hidden(Sym, "iterator", v_str(intern_c("@@iterator")));
    define_hidden(Sym, "asyncIterator", v_str(intern_c("@@asyncIterator")));
    define_hidden(Sym, "hasInstance", v_str(intern_c("@@hasInstance")));
    define_hidden(Sym, "toPrimitive", v_str(intern_c("@@toPrimitive")));
    define_hidden(Sym, "toStringTag", v_str(intern_c("@@toStringTag")));

    /* globals */
    define_hidden(global_obj, "NaN", v_num(js_nan()));
    define_hidden(global_obj, "Infinity", v_num(js_inf()));
    define_hidden(global_obj, "undefined", v_undef());
    define_hidden(global_obj, "globalThis", v_obj(global_obj));
    define_method(global_obj, "parseInt", parse_int, 2, 0);
    define_method(global_obj, "parseFloat", parse_float, 1, 0);
    define_method(global_obj, "isNaN", number_test, 1, 0);
    define_method(global_obj, "isFinite", number_test, 1, 1);
    define_method(global_obj, "encodeURIComponent", uri_code, 1, 0);
    define_method(global_obj, "encodeURI", uri_code, 1, 1);
    define_method(global_obj, "decodeURIComponent", uri_code, 1, 2);
    define_method(global_obj, "decodeURI", uri_code, 1, 3);
    define_method(global_obj, "escape", uri_code, 1, 4);
    define_method(global_obj, "unescape", uri_code, 1, 5);
    define_method(global_obj, "eval", global_eval, 1, 0);

    /* console */
    JsObj *con = new_object();
    define_hidden(global_obj, "console", v_obj(con));
    define_method(con, "log", console_log, 0, 0);
    define_method(con, "info", console_log, 0, 0);
    define_method(con, "debug", console_log, 0, 0);
    define_method(con, "trace", console_log, 0, 0);
    define_method(con, "dir", console_log, 0, 0);
    define_method(con, "table", console_log, 0, 0);
    define_method(con, "warn", console_log, 0, 1);
    define_method(con, "error", console_log, 0, 2);
    define_method(con, "assert", noop, 0, 0);
    define_method(con, "group", noop, 0, 0);
    define_method(con, "groupCollapsed", noop, 0, 0);
    define_method(con, "groupEnd", noop, 0, 0);
    define_method(con, "time", noop, 0, 0);
    define_method(con, "timeEnd", noop, 0, 0);
    define_method(con, "count", noop, 0, 0);
    init_typed_arrays();
    init_iterators();
    init_reflect();
}
