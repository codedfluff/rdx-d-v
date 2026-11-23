/* web/js_frames.c - what scripts see of the other realms (web/realm.c):
   window proxies (iframe.contentWindow, window.parent and top, frames),
   postMessage and message events, Worker objects and a worker's own
   global scope, and the structured clone that copies messages from one
   realm to another (also structuredClone()). Part of the browser. */

static JsObj *WindowProxyProto, *WorkerProto;
static int proxy_realm[MAX_REALMS];
static JsObj *proxy_obj[MAX_REALMS];
static int proxy_n;
static int worker_realm[MAX_REALMS]; /* Worker objects this realm made, by their worker's realm id */
static JsObj *worker_obj[MAX_REALMS];
static int worker_n;

static int my_realm_id(void) { return realm_list[realm_now].id; }

/* scripts' view of realm `id`: this window itself, or a proxy for another */
static JsObj *proxy_for(int id) {
    if (id == my_realm_id()) return global_obj;
    for (int i = 0; i < proxy_n; i++)
        if (proxy_realm[i] == id) return proxy_obj[i];
    JsObj *p = obj_new_raw(CL_OBJECT, WindowProxyProto);
    p->magic = id;
    if (proxy_n < MAX_REALMS) {
        proxy_realm[proxy_n] = id;
        proxy_obj[proxy_n++] = p;
    }
    return p;
}

static void frames_js_mark(void) {
    if (WindowProxyProto) gc_push(&WindowProxyProto->gc);
    if (WorkerProto) gc_push(&WorkerProto->gc);
    for (int i = 0; i < proxy_n; i++) gc_push(&proxy_obj[i]->gc);
    for (int i = 0; i < worker_n; i++) gc_push(&worker_obj[i]->gc);
}

/* "https://host[:port]" of an address ("null" for files and the like) */
static void origin_of(const char *url, char *out, size_t max) {
    if (!strncasecmp(url, "blob:", 5)) url += 5;
    const char *p = strstr(url, "://");
    if (!p || (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8))) {
        strcpy(out, "null");
        return;
    }
    p += 3;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    size_t n = (size_t)(p - url);
    if (n >= max) n = max - 1;
    memcpy(out, url, n);
    out[n] = 0;
    for (size_t i = 0; i < n && out[i] != ':'; i++)
        if (out[i] >= 'A' && out[i] <= 'Z') out[i] += 32;
}

/* ---------------------------------------------------------------- the structured clone */

/* Values as bytes: a tag, then what it needs. Objects seen before are
   written as 'r' and their number, so shared and circular references
   come out the same. */
typedef struct {
    Buf out;
    JsObj **seen;
    int nseen, cap;
    const char *error;
} Clone;

static void put_u32(Buf *b, unsigned v) { buf_add(b, (const char *)&v, 4); }
static void put_bytes(Buf *b, const char *p, int n) {
    put_u32(b, (unsigned)n);
    buf_add(b, p, n);
}

static int clone_write(Clone *c, Value v, int depth);

static int clone_object_props(Clone *c, JsObj *o, int depth) {
    JsObj *keys = new_array(0);
    enumerable_keys(o, keys, 1);
    put_u32(&c->out, (unsigned)keys->alen);
    for (int i = 0; i < keys->alen; i++) {
        JsStr *k = keys->arr[i].u.s;
        put_bytes(&c->out, k->s, k->len);
        Value item;
        TRY(get_value(v_obj(o), interned(k), &item));
        TRY(clone_write(c, item, depth + 1));
    }
    return C_NORMAL;
}

static int clone_error(Clone *c, const char *what) {
    c->error = what;
    return throw_error(ErrorProto, what);
}

/* the name of the constructor an object was made by ("Blob", ...) */
static int made_by(JsObj *o, const char *name) {
    for (JsObj *p = o->proto; p; p = p->proto) {
        Prop *cp = own_prop(p, S_constructor);
        if (!cp || cp->v.t != V_OBJ) continue;
        Prop *np = own_prop(cp->v.u.o, S_name);
        return np && np->v.t == V_STR && !strcmp(np->v.u.s->s, name);
    }
    return 0;
}

static int clone_write(Clone *c, Value v, int depth) {
    Buf *b = &c->out;
    if (depth > 200) return clone_error(c, "DataCloneError: the message is nested too deeply to be cloned");
    switch (v.t) {
    case V_UNDEF: buf_ch(b, 'u'); return C_NORMAL;
    case V_NULL: buf_ch(b, 'n'); return C_NORMAL;
    case V_BOOL: buf_ch(b, v.u.b ? 't' : 'f'); return C_NORMAL;
    case V_NUM: buf_ch(b, 'd'); buf_add(b, (const char *)&v.u.n, 8); return C_NORMAL;
    case V_STR: buf_ch(b, 's'); put_bytes(b, v.u.s->s, v.u.s->len); return C_NORMAL;
    case V_BIG: buf_ch(b, 'B'); put_bytes(b, v.u.s->s, v.u.s->len); return C_NORMAL;
    case V_SYM: return clone_error(c, "DataCloneError: a Symbol could not be cloned");
    }
    JsObj *o = v.u.o;
    for (int i = 0; i < c->nseen; i++) {
        if (c->seen[i] == o) {
            buf_ch(b, 'r');
            put_u32(b, (unsigned)i);
            return C_NORMAL;
        }
    }
    if (o->cls == CL_FUNCTION) return clone_error(c, "DataCloneError: a function could not be cloned");
    if (o->cls >= CL_NODE || o->cls == CL_PROMISE || o == global_obj || o->proto == WindowProxyProto || o->proto == WorkerProto)
        return clone_error(c, "DataCloneError: this kind of object could not be cloned");
    if (c->nseen == c->cap) {
        int cap = c->cap ? c->cap * 2 : 64;
        JsObj **s = realloc(c->seen, sizeof(JsObj *) * (size_t)cap);
        if (!s) return clone_error(c, "DataCloneError: out of memory");
        c->seen = s;
        c->cap = cap;
    }
    c->seen[c->nseen++] = o;
    switch (o->cls) {
    case CL_ARRAY:
        if (o->fn_kind) { /* a typed array: its kind and bytes */
            JsStr *bytes;
            TRY(bytes_of(v, &bytes));
            buf_ch(b, 'T');
            buf_ch(b, (char)o->fn_kind);
            put_bytes(b, bytes->s, bytes->len);
            return C_NORMAL;
        }
        buf_ch(b, 'a');
        put_u32(b, (unsigned)o->alen);
        for (int i = 0; i < o->alen; i++) TRY(clone_write(c, o->arr[i], depth + 1));
        return C_NORMAL;
    case CL_BUFFER:
        buf_ch(b, 'b');
        put_bytes(b, o->extra, o->extra_len);
        return C_NORMAL;
    case CL_DATE:
        buf_ch(b, 'D');
        buf_add(b, (const char *)&o->this_val.u.n, 8);
        return C_NORMAL;
    case CL_BOOLEAN: case CL_NUMBER: case CL_STRING:
        buf_ch(b, 'W');
        return clone_write(c, o->this_val, depth + 1);
    case CL_REGEXP: {
        Value src, flags;
        TRY(get_value(v, intern_c("source"), &src));
        TRY(get_value(v, intern_c("flags"), &flags));
        buf_ch(b, 'R');
        put_bytes(b, src.t == V_STR ? src.u.s->s : "", src.t == V_STR ? src.u.s->len : 0);
        put_bytes(b, flags.t == V_STR ? flags.u.s->s : "", flags.t == V_STR ? flags.u.s->len : 0);
        return C_NORMAL;
    }
    case CL_MAP: case CL_SET: {
        Value *e = o->extra;
        int n = 0;
        for (int i = 0; i < o->extra_len; i++) n += !(e[2 * i].t == V_UNDEF && e[2 * i + 1].t == V_NULL);
        buf_ch(b, o->cls == CL_MAP ? 'M' : 'S');
        put_u32(b, (unsigned)n);
        for (int i = 0; i < o->extra_len; i++) {
            e = o->extra; /* (getters may have grown it) */
            if (e[2 * i].t == V_UNDEF && e[2 * i + 1].t == V_NULL) continue;
            TRY(clone_write(c, e[2 * i], depth + 1));
            if (o->cls == CL_MAP) TRY(clone_write(c, ((Value *)o->extra)[2 * i + 1], depth + 1));
        }
        return C_NORMAL;
    }
    case CL_ERROR: {
        Value name, message;
        TRY(get_value(v, S_name, &name));
        TRY(get_value(v, intern_c("message"), &message));
        JsStr *ns = S_empty, *ms = S_empty;
        if (name.t != V_UNDEF) TRY(to_string(name, &ns));
        if (message.t != V_UNDEF) TRY(to_string(message, &ms));
        buf_ch(b, 'E');
        put_bytes(b, ns->s, ns->len);
        put_bytes(b, ms->s, ms->len);
        return C_NORMAL;
    }
    }
    if (DataViewProto && o->proto == DataViewProto && o->target) {
        buf_ch(b, 'V');
        put_bytes(b, (char *)o->target->extra + o->magic, o->extra_len);
        return C_NORMAL;
    }
    if (made_by(o, "Blob") || made_by(o, "File")) { /* (the prelude's: its bytes and type) */
        Value text, type, name;
        TRY(get_value(v, intern_c("_text"), &text));
        TRY(get_value(v, intern_c("type"), &type));
        TRY(get_value(v, S_name, &name));
        buf_ch(b, 'L');
        put_bytes(b, text.t == V_STR ? text.u.s->s : "", text.t == V_STR ? text.u.s->len : 0);
        put_bytes(b, type.t == V_STR ? type.u.s->s : "", type.t == V_STR ? type.u.s->len : 0);
        put_bytes(b, name.t == V_STR ? name.u.s->s : "", name.t == V_STR ? name.u.s->len : 0);
        return C_NORMAL;
    }
    buf_ch(b, 'o');
    return clone_object_props(c, o, depth);
}

/* v as bytes (malloc'd, in *out / *len); throws DataCloneError if it cannot be. */
static int clone_serialize(Value v, char **out, int *len) {
    Clone c;
    memset(&c, 0, sizeof(c));
    int r = clone_write(&c, v, 0);
    free(c.seen);
    if (r != C_NORMAL) {
        free(c.out.p);
        return r;
    }
    *out = c.out.p ? c.out.p : malloc(1);
    *len = c.out.len;
    return C_NORMAL;
}

typedef struct {
    const unsigned char *p, *end;
    Value *made;
    int nmade, cap;
    int bad;
} Unclone;

static unsigned get_u32(Unclone *u) {
    if (u->end - u->p < 4) {
        u->bad = 1;
        return 0;
    }
    unsigned v;
    memcpy(&v, u->p, 4);
    u->p += 4;
    return v;
}

static JsStr *get_str(Unclone *u) {
    unsigned n = get_u32(u);
    if (u->bad || (size_t)(u->end - u->p) < n) {
        u->bad = 1;
        return S_empty;
    }
    JsStr *s = str_new((const char *)u->p, (int)n);
    u->p += n;
    return s;
}

static void remember(Unclone *u, Value v) {
    if (u->nmade == u->cap) {
        int cap = u->cap ? u->cap * 2 : 64;
        Value *m = malloc(sizeof(Value) * (size_t)cap);
        if (!m) {
            u->bad = 1;
            return;
        }
        if (u->made) {
            memcpy(m, u->made, sizeof(Value) * (size_t)u->nmade);
            gc_extra_remove(u->made);
            free(u->made);
        }
        u->made = m;
        u->cap = cap;
        gc_extra_add(m, sizeof(Value) * (size_t)cap); /* (what is made so far stays alive) */
    }
    u->made[u->nmade++] = v;
}

static Value clone_read(Unclone *u, int depth) {
    if (u->bad || u->p >= u->end || depth > 210) {
        u->bad = 1;
        return v_undef();
    }
    char tag = (char)*u->p++;
    switch (tag) {
    case 'u': return v_undef();
    case 'n': return v_null();
    case 't': return v_bool(1);
    case 'f': return v_bool(0);
    case 'd': {
        double d = 0;
        if (u->end - u->p < 8) u->bad = 1;
        else memcpy(&d, u->p, 8), u->p += 8;
        return v_num(d);
    }
    case 's': return v_str(get_str(u));
    case 'B': return v_big(get_str(u));
    case 'r': {
        unsigned i = get_u32(u);
        if (i >= (unsigned)u->nmade) {
            u->bad = 1;
            return v_undef();
        }
        return u->made[i];
    }
    case 'a': {
        JsObj *a = new_array(0);
        remember(u, v_obj(a));
        unsigned n = get_u32(u);
        for (unsigned i = 0; i < n && !u->bad; i++) arr_push(a, clone_read(u, depth + 1));
        return v_obj(a);
    }
    case 'o': {
        JsObj *o = new_object();
        remember(u, v_obj(o));
        unsigned n = get_u32(u);
        for (unsigned i = 0; i < n && !u->bad; i++) {
            JsStr *k = interned(get_str(u));
            Value item = clone_read(u, depth + 1);
            prop_set_raw(o, k, item, 0);
        }
        return v_obj(o);
    }
    case 'T': {
        int kind = u->p < u->end ? *u->p++ : 2;
        JsStr *bytes = get_str(u);
        if (kind < 1 || kind > 9) kind = 2;
        JsObj *buf = buffer_new(bytes->len);
        if (buf->extra_len == bytes->len && bytes->len) memcpy(buf->extra, bytes->s, (size_t)bytes->len);
        JsObj *a = view_new(kind, buf, 0, bytes->len / TYPED_BYTES[kind]);
        remember(u, v_obj(a));
        return v_obj(a);
    }
    case 'b': {
        JsStr *bytes = get_str(u);
        JsObj *buf = buffer_new(bytes->len);
        if (buf->extra_len == bytes->len && bytes->len) memcpy(buf->extra, bytes->s, (size_t)bytes->len);
        remember(u, v_obj(buf));
        return v_obj(buf);
    }
    case 'V': {
        JsStr *bytes = get_str(u);
        JsObj *buf = buffer_new(bytes->len);
        if (buf->extra_len == bytes->len && bytes->len) memcpy(buf->extra, bytes->s, (size_t)bytes->len);
        JsObj *dv = obj_new_raw(CL_OBJECT, DataViewProto);
        dv->target = buf;
        dv->extra_len = bytes->len;
        remember(u, v_obj(dv));
        return v_obj(dv);
    }
    case 'D': {
        double d = 0;
        if (u->end - u->p < 8) u->bad = 1;
        else memcpy(&d, u->p, 8), u->p += 8;
        JsObj *o = obj_new_raw(CL_DATE, DateProto);
        o->this_val = v_num(d);
        remember(u, v_obj(o));
        return v_obj(o);
    }
    case 'W': {
        Value inner = clone_read(u, depth + 1);
        JsObj *o = to_object_or_null(inner);
        Value r = o ? v_obj(o) : v_undef();
        remember(u, r);
        return r;
    }
    case 'R': {
        JsStr *src = get_str(u), *flags = get_str(u);
        Value ctor, made = v_undef(), a[2] = {v_str(src), v_str(flags)};
        if (get_value(v_obj(global_obj), intern_c("RegExp"), &ctor) == C_NORMAL && is_callable(ctor) &&
            construct(ctor.u.o, a, 2, ctor.u.o, &made) != C_NORMAL)
            js_exception = v_undef();
        remember(u, made);
        return made;
    }
    case 'M': case 'S': {
        JsObj *m = obj_new_raw(tag == 'M' ? CL_MAP : CL_SET, tag == 'M' ? MapProto : SetProto);
        remember(u, v_obj(m));
        unsigned n = get_u32(u);
        for (unsigned i = 0; i < n && !u->bad; i++) {
            Value k = clone_read(u, depth + 1);
            Value item = tag == 'M' ? clone_read(u, depth + 1) : k;
            map_put(m, k, item);
        }
        return v_obj(m);
    }
    case 'E': {
        JsStr *name = get_str(u), *message = get_str(u);
        JsObj *proto = !strcmp(name->s, "TypeError") ? TypeErrorProto : !strcmp(name->s, "RangeError") ? RangeErrorProto
                     : !strcmp(name->s, "SyntaxError") ? SyntaxErrorProto : !strcmp(name->s, "ReferenceError") ? ReferenceErrorProto : ErrorProto;
        JsObj *e = obj_new_raw(CL_ERROR, proto);
        prop_set_raw(e, intern_c("message"), v_str(message), P_HIDDEN);
        remember(u, v_obj(e));
        return v_obj(e);
    }
    case 'L': {
        JsStr *text = get_str(u), *type = get_str(u), *name = get_str(u);
        Value ctor, made = v_undef();
        int file = name->len > 0;
        if (get_value(v_obj(global_obj), intern_c(file ? "File" : "Blob"), &ctor) == C_NORMAL && is_callable(ctor)) {
            JsObj *parts = new_array(0), *opts = new_object();
            arr_push(parts, v_str(text));
            prop_set_raw(opts, intern_c("type"), v_str(type), 0);
            Value a[3] = {v_obj(parts), file ? v_str(name) : v_obj(opts), v_obj(opts)};
            if (construct(ctor.u.o, a, file ? 3 : 2, ctor.u.o, &made) != C_NORMAL) js_exception = v_undef();
        }
        remember(u, made);
        return made;
    }
    }
    u->bad = 1;
    return v_undef();
}

/* bytes back into a value in the running realm (undefined if they are bad) */
static Value clone_deserialize(const char *data, int len, int *ok) {
    Unclone u;
    memset(&u, 0, sizeof(u));
    u.p = (const unsigned char *)data;
    u.end = u.p + len;
    Value v = clone_read(&u, 0);
    if (u.made) {
        gc_extra_remove(u.made);
        free(u.made);
    }
    *ok = !u.bad;
    return u.bad ? v_undef() : v;
}

NATIVE(structured_clone) {
    UNUSED;
    char *data;
    int len, ok;
    TRY(clone_serialize(ARG(0), &data, &len));
    *ret = clone_deserialize(data, len, &ok);
    free(data);
    return C_NORMAL;
}

/* ---------------------------------------------------------------- postMessage */

enum { MSG_WINDOW, MSG_WORKER_SCOPE, MSG_WORKER_OBJECT, MSG_WORKER_ERROR };

static int post_to(int to, int kind, Value data) {
    char *bytes;
    int len;
    TRY(clone_serialize(data, &bytes, &len));
    char origin[256];
    origin_of(current_url, origin, sizeof(origin));
    message_post(to, kind, my_realm_id(), origin, bytes, len);
    return C_NORMAL;
}

/* window.postMessage(data, targetOrigin) / proxy.postMessage(...): to the
   window `this` stands for, if its origin is the one asked for */
NATIVE(window_post_message) {
    UNUSED;
    int to = this_v.t == V_OBJ && this_v.u.o->proto == WindowProxyProto ? this_v.u.o->magic : my_realm_id();
    int r = realm_index(to);
    *ret = v_undef();
    if (r < 0) return C_NORMAL; /* (closed) */
    Value want = ARG(1);
    if (want.t == V_OBJ && !is_callable(want)) TRY(get_value(want, intern_c("targetOrigin"), &want));
    JsStr *target = NULL;
    if (want.t != V_UNDEF && want.t != V_OBJ) TRY(to_string(want, &target));
    if (target && strcmp(target->s, "*")) {
        char mine[256], theirs[256];
        origin_of(current_url, mine, sizeof(mine));
        origin_of(realm_list[r].url[0] ? realm_list[r].url : (r == realm_now ? current_url : ""), theirs, sizeof(theirs));
        const char *asked = !strcmp(target->s, "/") ? mine : target->s;
        char asked_origin[256];
        origin_of(asked, asked_origin, sizeof(asked_origin));
        if (strcmp(asked_origin, theirs)) return C_NORMAL; /* (not for them: dropped, as browsers do) */
    }
    return post_to(to, MSG_WINDOW, ARG(0));
}

/* proxy: window, self, frames (0); parent (1); top (2); closed (3); length (4) */
NATIVE(proxy_info) {
    UNUSED;
    int id = this_v.t == V_OBJ && this_v.u.o->proto == WindowProxyProto ? this_v.u.o->magic : my_realm_id();
    int r = realm_index(id);
    switch (self->magic & 0xFFFF) {
    case 0: *ret = this_v; break;
    case 1: case 2: {
        if (r < 0) {
            *ret = v_null();
            break;
        }
        int at = r;
        if ((self->magic & 0xFFFF) == 1) at = realm_list[r].parent >= 0 && realm_index(realm_list[r].parent) >= 0 ? realm_index(realm_list[r].parent) : r;
        else
            while (realm_list[at].parent >= 0 && realm_index(realm_list[at].parent) >= 0) at = realm_index(realm_list[at].parent);
        *ret = v_obj(proxy_for(realm_list[at].id));
        break;
    }
    case 3: *ret = v_bool(r < 0); break;
    default: {
        int n = 0;
        for (int i = 1; i < MAX_REALMS; i++) n += realm_list[i].used && realm_list[i].kind == REALM_FRAME && realm_list[i].parent == id;
        *ret = v_num(n);
        break;
    }
    }
    return C_NORMAL;
}

/* iframe.contentWindow (magic 0) / contentDocument (1) */
static int iframe_content(Node *n, int document_too, Value *out) {
    int i = frame_of(n);
    if (i < 0) { /* empty (about:blank): this realm stands in for it */
        *out = document_too ? v_null() : v_obj(global_obj);
        return C_NORMAL;
    }
    *out = document_too ? v_null() : v_obj(proxy_for(realm_list[i].id)); /* (another page: no direct access) */
    return C_NORMAL;
}

/* ---------------------------------------------------------------- workers */

static int worker_index_of(int id) {
    for (int i = 0; i < worker_n; i++)
        if (worker_realm[i] == id) return i;
    return -1;
}

NATIVE(worker_ctor) {
    UNUSED;
    if (!native_new_target) return throw_type("Failed to construct 'Worker': Please use the 'new' operator", NULL, NULL);
    JsStr *src;
    TRY(to_string(ARG(0), &src));
    char url[1024];
    resolve_url(base_url, src->s, url, sizeof(url));
    if (!fetch_allowed(url)) return throw_error(ErrorProto, "SecurityError: Failed to construct 'Worker': the script may not be loaded");
    int i = realm_new(REALM_WORKER);
    if (i < 0) return throw_error(ErrorProto, "Failed to construct 'Worker': too many workers");
    strncpy(realm_list[i].url, url, sizeof(realm_list[i].url) - 1);
    realm_list[i].next_due = uptime_ms(); /* (it starts when this task is over) */
    JsObj *w = obj_new_raw(CL_OBJECT, WorkerProto);
    w->magic = realm_list[i].id;
    if (worker_n < MAX_REALMS) {
        worker_realm[worker_n] = realm_list[i].id;
        worker_obj[worker_n++] = w;
    }
    *ret = v_obj(w);
    return C_NORMAL;
}

/* worker.postMessage (magic 0) / terminate (1) */
NATIVE(worker_method) {
    UNUSED;
    *ret = v_undef();
    if (this_v.t != V_OBJ || this_v.u.o->proto != WorkerProto) return throw_type("not a Worker", NULL, NULL);
    int id = this_v.u.o->magic, r = realm_index(id);
    if (r < 0) return C_NORMAL;
    if ((self->magic & 0xFFFF) == 1) {
        realm_list[r].doomed = 1;
        return C_NORMAL;
    }
    return post_to(id, MSG_WORKER_SCOPE, ARG(0));
}

/* in a worker: postMessage (magic 0), close (1), importScripts (2) */
NATIVE(worker_scope_method) {
    UNUSED;
    *ret = v_undef();
    Realm *me = &realm_list[realm_now];
    switch (self->magic & 0xFFFF) {
    case 0: return post_to(me->parent, MSG_WORKER_OBJECT, ARG(0));
    case 1: me->doomed = 1; return C_NORMAL;
    }
    for (int k = 0; k < argc; k++) {
        JsStr *src;
        TRY(to_string(args[k], &src));
        char url[1024], error[400];
        resolve_url(base_url, src->s, url, sizeof(url));
        Fetched f;
        if (!fetch_allowed(url) || fetch_url(url, NULL, &f, error) != 0)
            return throw_error(ErrorProto, "NetworkError: Failed to execute 'importScripts' on 'WorkerGlobalScope': the script could not be loaded");
        Value code = v_str(str_new(f.body, (int)f.len));
        free(f.body);
        TRY(global_eval(self, this_v, &code, 1, ret));
    }
    *ret = v_undef();
    return C_NORMAL;
}

/* Make the running realm a worker's global scope, and run its script. */
static void worker_start(void) {
    Realm *me = &realm_list[realm_now];
    realm_settings();
    strcpy(current_url, me->url);
    strcpy(base_url, me->url);
    static const char blank[] = "<html><head></head><body></body></html>";
    parse_html(blank, (int)sizeof(blank) - 1);
    js_page_start();
    if (!global_obj) return;
    JsObj *g = global_obj;
    /* no document or window in a worker: self is its global */
    static const char *gone[] = {"document", "window", "alert", "confirm", "prompt", "localStorage", "sessionStorage", "parent", "top", "frames",
                                 "opener", "frameElement", "getComputedStyle", "matchMedia", "scrollTo", "scrollBy", "open", "print", NULL};
    for (int i = 0; gone[i]; i++) prop_delete(g, intern_c(gone[i]));
    define_method(g, "postMessage", worker_scope_method, 1, 0);
    define_method(g, "close", worker_scope_method, 0, 1);
    define_method(g, "importScripts", worker_scope_method, 1, 2);
    JsObj *scope = make_native("DedicatedWorkerGlobalScope", noop, 0, 0);
    prop_set_raw(scope, S_prototype, v_obj(g->proto ? g->proto : ObjectProto), P_HIDDEN);
    define_hidden(g, "DedicatedWorkerGlobalScope", v_obj(scope));
    define_hidden(g, "WorkerGlobalScope", v_obj(scope));
    define_hidden(g, "name", v_str(S_empty));
    Fetched f;
    char error[400];
    if (fetch_url(me->url, NULL, &f, error) != 0) {
        Value e = v_str(str_c(error));
        post_to(me->parent, MSG_WORKER_ERROR, e);
        return;
    }
    if (js_run_script(f.body, (int)f.len, me->url) < 0) {
        Value e = v_str(str_c(js_last_error));
        post_to(me->parent, MSG_WORKER_ERROR, e);
    }
    free(f.body);
    run_microtasks();
}

/* ---------------------------------------------------------------- receiving */

static void dispatch_on(JsObj *target, JsObj *event) {
    char here;
    js_set_stack_base(&here);
    js_start_run(3000);
    if (dispatch_event(target, event) < 0) {
        describe_exception(js_last_error, sizeof(js_last_error));
        js_report(js_last_error);
        js_errors++;
        js_abort = 0;
    }
    run_microtasks();
}

/* One message for the running realm. */
static void message_receive(int kind, int from, const char *origin, const char *data, int len) {
    if (!global_obj || !js_enabled) return;
    int ok;
    Value v = clone_deserialize(data, len, &ok);
    JsObj *target = global_obj;
    if (kind == MSG_WORKER_OBJECT || kind == MSG_WORKER_ERROR) {
        int w = worker_index_of(from);
        if (w < 0) return;
        target = worker_obj[w];
    }
    JsObj *e = new_event(kind == MSG_WORKER_ERROR ? "error" : ok ? "message" : "messageerror", 0, 0);
    if (kind == MSG_WORKER_ERROR) {
        if (js_trace_throws && v.t == V_STR) printf("[worker] %s\n", v.u.s->s);
        prop_set_raw(e, intern_c("message"), v, 0);
    } else {
        prop_set_raw(e, intern_c("data"), v, 0);
        prop_set_raw(e, intern_c("origin"), v_str(str_c(kind == MSG_WINDOW ? origin : "")), 0);
        prop_set_raw(e, intern_c("lastEventId"), v_str(S_empty), 0);
        prop_set_raw(e, intern_c("source"), kind == MSG_WINDOW && realm_index(from) >= 0 ? v_obj(proxy_for(from)) : v_null(), 0);
        prop_set_raw(e, intern_c("ports"), v_obj(new_array(0)), 0);
    }
    dispatch_on(target, e);
}

/* ---------------------------------------------------------------- setting up */

static void frames_js_init(JsObj *w) {
    proxy_n = worker_n = 0;
    WindowProxyProto = obj_new_raw(CL_OBJECT, ObjectProto);
    define_method(WindowProxyProto, "postMessage", window_post_message, 2, 0);
    define_getter(WindowProxyProto, "window", proxy_info, NULL, 0);
    define_getter(WindowProxyProto, "self", proxy_info, NULL, 0);
    define_getter(WindowProxyProto, "frames", proxy_info, NULL, 0);
    define_getter(WindowProxyProto, "parent", proxy_info, NULL, 1);
    define_getter(WindowProxyProto, "top", proxy_info, NULL, 2);
    define_getter(WindowProxyProto, "closed", proxy_info, NULL, 3);
    define_getter(WindowProxyProto, "length", proxy_info, NULL, 4);
    define_method(WindowProxyProto, "focus", noop, 0, 0);
    define_method(WindowProxyProto, "blur", noop, 0, 0);
    define_method(WindowProxyProto, "close", noop, 0, 0);
    define_method(w, "postMessage", window_post_message, 2, 0);
    define_method(w, "structuredClone", structured_clone, 1, 0);
    /* in a frame: its parent and top are other windows */
    Realm *me = &realm_list[realm_now];
    if (me->kind == REALM_FRAME) {
        define_getter(w, "parent", proxy_info, NULL, 1);
        define_getter(w, "top", proxy_info, NULL, 2);
        define_hidden(w, "frameElement", v_null());
    }
    define_getter(w, "length", proxy_info, NULL, 4);
    WorkerProto = obj_new_raw(CL_OBJECT, ObjectProto);
    JsObj *wc = make_ctor("Worker", worker_ctor, 1, 0, WorkerProto);
    (void)wc;
    define_method(WorkerProto, "postMessage", worker_method, 1, 0);
    define_method(WorkerProto, "terminate", worker_method, 0, 1);
    define_method(WorkerProto, "addEventListener", event_listener, 2, 0);
    define_method(WorkerProto, "removeEventListener", event_listener, 2, 1);
    define_method(WorkerProto, "dispatchEvent", dispatch_event_native, 1, 0);
}
