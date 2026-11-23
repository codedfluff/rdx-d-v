/* web/js_coro.c - generators (function*, yield) and async functions that
   really wait (await). Part of the browser's JavaScript engine.

   The interpreter walks the syntax tree with C recursion, so a function
   that stops half way keeps its place on its own C stack: each running
   generator or async function gets one, and control switches between
   stacks (a coroutine). The collector scans the stacks of the stopped
   ones for the values they hold. */

/* ---------------------------------------------------------------- switching stacks */

/* coro_switch(save, to): keep the registers the caller expects kept (as
   Windows x64 says: rbx rbp rdi rsi r12-r15, xmm6-xmm15) on this stack,
   store the stack pointer in *save, and carry on from stack pointer `to`. */
void coro_switch(void **save, void *to);
void coro_call_on(void *top, void (*fn)(void *), void *arg);
__asm__(".text\n"
        ".globl coro_switch\n"
        "coro_switch:\n"
        "    pushq %rbp\n    pushq %rbx\n    pushq %rdi\n    pushq %rsi\n"
        "    pushq %r12\n    pushq %r13\n    pushq %r14\n    pushq %r15\n"
        "    subq $168, %rsp\n"
        "    movdqu %xmm6, 0(%rsp)\n    movdqu %xmm7, 16(%rsp)\n    movdqu %xmm8, 32(%rsp)\n    movdqu %xmm9, 48(%rsp)\n"
        "    movdqu %xmm10, 64(%rsp)\n    movdqu %xmm11, 80(%rsp)\n    movdqu %xmm12, 96(%rsp)\n    movdqu %xmm13, 112(%rsp)\n"
        "    movdqu %xmm14, 128(%rsp)\n    movdqu %xmm15, 144(%rsp)\n"
        "    movq %rsp, (%rcx)\n"
        "    movq %rdx, %rsp\n"
        "    movdqu 0(%rsp), %xmm6\n    movdqu 16(%rsp), %xmm7\n    movdqu 32(%rsp), %xmm8\n    movdqu 48(%rsp), %xmm9\n"
        "    movdqu 64(%rsp), %xmm10\n    movdqu 80(%rsp), %xmm11\n    movdqu 96(%rsp), %xmm12\n    movdqu 112(%rsp), %xmm13\n"
        "    movdqu 128(%rsp), %xmm14\n    movdqu 144(%rsp), %xmm15\n"
        "    addq $168, %rsp\n"
        "    popq %r15\n    popq %r14\n    popq %r13\n    popq %r12\n"
        "    popq %rsi\n    popq %rdi\n    popq %rbx\n    popq %rbp\n"
        "    ret\n"
        /* coro_call_on(top, fn, arg): fn(arg) on the stack that ends at top */
        ".globl coro_call_on\n"
        "coro_call_on:\n"
        "    pushq %rbp\n"
        "    movq %rsp, %rbp\n"
        "    movq %rcx, %rsp\n"
        "    andq $-16, %rsp\n"
        "    subq $32, %rsp\n"
        "    movq %r8, %rcx\n"
        "    callq *%rdx\n"
        "    movq %rbp, %rsp\n"
        "    popq %rbp\n"
        "    ret\n"
        /* a new stack starts here: rbx holds the coroutine */
        ".globl coro_thunk\n"
        "coro_thunk:\n"
        "    andq $-16, %rsp\n"
        "    subq $32, %rsp\n"
        "    movq %rbx, %rcx\n"
        "    call coro_main\n"
        "    ud2\n");

enum { CO_NEW, CO_SUSPENDED, CO_RUNNING, CO_DONE };
enum { CO_GENERATOR, CO_ASYNC };
enum { CO_YIELDED, CO_RETURNED, CO_THREW, CO_WAITING };

#define CORO_STACK (1024 * 1024)
#define CORO_CANARY 0x52445843414E4152ull /* at the bottom of each stack: still there, or it overflowed */
#define CORO_POOL 24

typedef struct Coro {
    void *rsp;              /* while stopped: where its stack resumes */
    char *stack;
    struct Coro *caller;    /* who started or resumed it: control goes back there */
    struct Coro *next_live, *prev_live;
    int state, kind, marked;
    /* the call it runs */
    JsObj *fo;
    Value this_v;
    Value *args;
    int argc;
    /* across a switch */
    int in_throw;           /* resumed by a rejected await */
    int in_mode;            /* generators: resumed by next / return / throw (0 / 1 / 2) */
    Value in_value;
    int returning;          /* finishing because of return(v): */
    Value return_value;     /*   v */
    JsObj *gen;             /* generators: the generator object */
    int async_gen;          /* an async generator: it can await */
    int out_kind;
    Value out_value;
    JsObj *promise;         /* async: what the call returned */
    /* the interpreter's own position while it runs */
    char *stack_base;
    long long stack_room;
    int frame_depth;
} Coro;

static Coro main_coro;          /* the browser's own stack */
static Coro *current_coro;      /* NULL: the main stack */
static Coro *live_coros;        /* every coroutine with a stack */
static JsObj *GeneratorProto, *AsyncGeneratorProto;

/* Big built-in work (layout) asked for by a script in a coroutine runs on
   the main stack, which is idle below where it stopped: a coroutine's
   stack is small. coro_detour_sp: where the coroutine was meanwhile. */
static char *coro_detour_sp;

static char *current_coro_top(void) {
    return current_coro && !coro_detour_sp ? current_coro->stack + CORO_STACK : js_stack_top;
}

static void coro_on_main_stack(void (*fn)(void *), void *arg) {
    Coro *c = current_coro;
    if (!c || coro_detour_sp || !main_coro.rsp) {
        fn(arg);
        return;
    }
    char here;
    char *saved_base = js_stack_base, *saved_floor = js_stack_floor;
    long long saved_room = js_stack_room;
    coro_detour_sp = &here - 256 > c->stack ? &here - 256 : c->stack;
    js_stack_base = main_coro.stack_base;
    js_stack_room = main_coro.stack_room;
    js_stack_floor = NULL;
    coro_call_on((char *)main_coro.rsp - 512, fn, arg);
    js_stack_base = saved_base;
    js_stack_room = saved_room;
    js_stack_floor = saved_floor;
    coro_detour_sp = NULL;
}

NATIVE(noop_this) {
    UNUSED;
    *ret = this_v;
    return C_NORMAL;
}

static void coro_unlink(Coro *c) {
    if (c->prev_live) c->prev_live->next_live = c->next_live;
    else if (live_coros == c) live_coros = c->next_live;
    if (c->next_live) c->next_live->prev_live = c->prev_live;
    c->next_live = c->prev_live = NULL;
}

/* Stacks of finished coroutines, to use again. */
static char *stack_pool[CORO_POOL];
static int stack_pool_n;

static char *stack_get(void) {
    if (stack_pool_n) return stack_pool[--stack_pool_n];
    heap_since_gc += CORO_STACK; /* (a new stack counts towards the next collection) */
    if (heap_since_gc > GC_TRIGGER_BYTES) js_gc_during();
    char *s = malloc(CORO_STACK);
    if (!s) { /* collect, and try again */
        js_gc_during();
        if (stack_pool_n) return stack_pool[--stack_pool_n];
        s = malloc(CORO_STACK);
    }
    return s;
}

static void stack_put(char *s) {
    if (!s) return;
    if (stack_pool_n < CORO_POOL) stack_pool[stack_pool_n++] = s;
    else free(s);
}

static void coro_free(void *p) {
    Coro *c = p;
    if (!c) return;
    if (c->state == CO_RUNNING) return; /* (never while it runs) */
    coro_unlink(c);
    stack_put(c->stack);
    free(c->args);
    free(c);
}

static void coro_check(Coro *c) {
    if (c && c != &main_coro && *(unsigned long long *)c->stack != CORO_CANARY) {
        printf("[js] a generator or async function overflowed its stack\n");
        exit(1);
    }
}

/* Go from the running coroutine (or the main stack) to `to`. */
static void coro_enter(Coro *to) {
    Coro *from = current_coro ? current_coro : &main_coro;
    from->stack_base = js_stack_base;
    from->stack_room = js_stack_room;
    from->frame_depth = js_frame_depth;
    to->caller = from;
    current_coro = to == &main_coro ? NULL : to;
    js_stack_base = to->stack_base;
    js_stack_room = to->stack_room;
    js_stack_floor = to == &main_coro ? NULL : to->stack + 64 * 1024;
    js_frame_depth = to->frame_depth;
    to->state = CO_RUNNING;
    if (from != &main_coro) from->state = CO_SUSPENDED;
    coro_check(from);
    coro_switch(&from->rsp, to->rsp);
    /* back here when something switches to `from` again */
}

/* From the running coroutine back to whoever resumed it. */
static void coro_leave(Coro *c, int state) {
    Coro *to = c->caller ? c->caller : &main_coro;
    c->state = state;
    c->stack_base = js_stack_base;
    c->stack_room = js_stack_room;
    c->frame_depth = js_frame_depth;
    current_coro = to == &main_coro ? NULL : to;
    js_stack_base = to->stack_base;
    js_stack_room = to->stack_room;
    js_stack_floor = to == &main_coro ? NULL : to->stack + 64 * 1024;
    js_frame_depth = to->frame_depth;
    if (to != &main_coro) to->state = CO_RUNNING;
    coro_check(c);
    coro_switch(&c->rsp, to->rsp);
}

static void async_finish(Coro *c);

/* Where a new coroutine starts (on its own stack). */
__attribute__((used)) void coro_main(Coro *c) {
    Value ret = v_undef();
    int r = run_function_body(c->fo, c->this_v, c->args, c->argc, NULL, &ret);
    if (r != C_THROW && c->returning) ret = c->return_value; /* (ended by the generator's return(v)) */
    c->out_kind = r == C_THROW ? CO_THREW : CO_RETURNED;
    c->out_value = r == C_THROW ? js_exception : ret;
    if (r == C_THROW && !js_abort) js_exception = v_undef(); /* (a stopped script stays stopped: the resumer passes it on) */
    if (c->kind == CO_ASYNC) async_finish(c);
    coro_leave(c, CO_DONE);
    for (;;) __asm__ volatile("ud2"); /* (never switched to again) */
}

static Coro *coro_new(int kind, JsObj *fo, Value this_v, Value *args, int argc) {
    Coro *c = malloc(sizeof(Coro));
    if (!c) return NULL;
    memset(c, 0, sizeof(*c));
    c->stack = stack_get();
    c->args = argc ? malloc(sizeof(Value) * (size_t)argc) : NULL;
    if (!c->stack || (argc && !c->args)) {
        stack_put(c->stack);
        free(c->args);
        free(c);
        return NULL;
    }
    for (int i = 0; i < argc; i++) c->args[i] = args[i];
    c->argc = argc;
    c->fo = fo;
    c->this_v = this_v;
    c->kind = kind;
    c->state = CO_NEW;
    /* its first switch "returns" into coro_thunk with rbx = c */
    char *top = (char *)(((unsigned long long)(c->stack + CORO_STACK) & ~15ull) - 64);
    unsigned long long *frame = (unsigned long long *)(top - 240);
    memset(frame, 0, 240);
    extern void coro_thunk(void);
    frame[216 / 8] = (unsigned long long)c;           /* rbx */
    frame[232 / 8] = (unsigned long long)coro_thunk;  /* where ret goes */
    c->rsp = frame;
    c->stack_base = top;
    *(unsigned long long *)c->stack = CORO_CANARY;
    c->stack_room = 600 * 1024; /* (scripts stop there; the rest is for the parser and natives they call) */
    c->next_live = live_coros;
    if (live_coros) live_coros->prev_live = c;
    live_coros = c;
    return c;
}

/* ---------------------------------------------------------------- generators */

static JsObj *iter_result(Value v, int done) {
    JsObj *r = new_object();
    prop_set_raw(r, S_value, v, 0);
    prop_set_raw(r, S_done, v_bool(done), 0);
    return r;
}

/* Calling a function*: a generator object; nothing runs yet. */
static int coro_make_generator(JsObj *fo, Value this_v, Value *args, int argc, Value *ret) {
    Coro *c = coro_new(CO_GENERATOR, fo, this_v, args, argc);
    if (!c) return throw_error(RangeErrorProto, "out of memory");
    int async = fo->fn && (fo->fn->flags & F_ASYNC);
    JsObj *proto = async ? AsyncGeneratorProto : GeneratorProto;
    Value p;
    if (get_value(v_obj(fo), S_prototype, &p) == C_NORMAL && p.t == V_OBJ) { /* (fn.prototype: under the generator prototype) */
        if (p.u.o->proto == ObjectProto) p.u.o->proto = proto;
        proto = p.u.o;
    }
    JsObj *g = obj_new_raw(CL_GENERATOR, proto);
    g->extra = c;
    g->magic = async;
    c->gen = g;
    c->async_gen = async;
    *ret = v_obj(g);
    return C_NORMAL;
}

/* The generator is finished: let go of its coroutine. */
static void generator_end(Coro *c) {
    if (c->gen && c->gen->extra == c) c->gen->extra = NULL;
    coro_free(c);
}

/* Run a generator from where it stopped, with next / return / throw
   (mode 0 / 1 / 2) of v, until it yields, finishes, or (async) waits. */
static void generator_step(Coro *c, int mode, Value v) {
    c->in_mode = mode;
    c->in_value = v;
    c->in_throw = 0;
    coro_enter(c);
}

static void agen_start(JsObj *g, int mode, Value v, JsObj *P);

/* The script was stopped (ran too long, out of memory) while the coroutine
   ran: the whole script stops, with that reason. */
static int coro_stopped(void) {
    if (js_exception.t == V_UNDEF) js_exception = v_str(str_c("the script was stopped"));
    return C_THROW;
}

/* An async generator's step ended (or stopped at an await: then later). */
static void agen_after(Coro *c) {
    if (c->out_kind == CO_WAITING) return;
    JsObj *g = c->gen, *P = c->promise;
    c->promise = NULL;
    Value out = c->out_value;
    if (c->out_kind == CO_YIELDED) {
        if (P) promise_resolve(P, v_obj(iter_result(out, 0)));
    } else {
        generator_end(c);
        if (P) {
            if (c->out_kind == CO_THREW) promise_settle(P, 2, out);
            else promise_resolve(P, v_obj(iter_result(out, 1)));
        }
    }
    /* the next request that waited for this one */
    JsObj *q = g ? g->target : NULL;
    if (q && q->alen >= 3) {
        int mode = (int)q->arr[0].u.n;
        Value v = q->arr[1];
        JsObj *next_p = q->arr[2].u.o;
        memmove(q->arr, q->arr + 3, sizeof(Value) * (size_t)(q->alen - 3));
        q->alen -= 3;
        agen_start(g, mode, v, next_p);
    }
}

static void agen_start(JsObj *g, int mode, Value v, JsObj *P) {
    Coro *c = g->extra;
    if (!c || c->state == CO_DONE || (c->state == CO_NEW && mode != 0)) { /* over (or ended before it began) */
        if (c) generator_end(c);
        if (mode == 2) promise_settle(P, 2, v);
        else promise_resolve(P, v_obj(iter_result(mode == 1 ? v : v_undef(), 1)));
        JsObj *q = g->target;
        if (q && q->alen >= 3) {
            int m = (int)q->arr[0].u.n;
            Value nv = q->arr[1];
            JsObj *np = q->arr[2].u.o;
            memmove(q->arr, q->arr + 3, sizeof(Value) * (size_t)(q->alen - 3));
            q->alen -= 3;
            agen_start(g, m, nv, np);
        }
        return;
    }
    c->promise = P;
    generator_step(c, mode, v);
    agen_after(c);
}

/* next / return / throw (magic 0 / 1 / 2) */
NATIVE(generator_resume) {
    int mode = self->magic & 0xFFFF;
    if (this_v.t != V_OBJ || this_v.u.o->cls != CL_GENERATOR || this_v.u.o->magic == 2)
        return throw_type("not a generator", NULL, NULL);
    JsObj *g = this_v.u.o;
    Coro *c = g->extra;
    Value v = ARG(0);
    if (g->magic) { /* an async generator: answers come as promises, one request at a time */
        JsObj *P = promise_new();
        *ret = v_obj(P);
        if (c && (c->promise || c->state == CO_RUNNING)) { /* busy: after the ones before it */
            if (!g->target) g->target = new_array(3);
            arr_push(g->target, v_num(mode));
            arr_push(g->target, v);
            arr_push(g->target, v_obj(P));
            return C_NORMAL;
        }
        agen_start(g, mode, v, P);
        return js_abort ? coro_stopped() : C_NORMAL;
    }
    if (c && c->state == CO_RUNNING) return throw_type("Generator is already running", NULL, NULL);
    if (!c || c->state == CO_DONE || (c->state == CO_NEW && mode != 0)) { /* over (or ended before it began) */
        if (c) generator_end(c);
        if (mode == 2) {
            js_exception = v;
            return C_THROW;
        }
        *ret = v_obj(iter_result(mode == 1 ? v : v_undef(), 1));
        return C_NORMAL;
    }
    generator_step(c, mode, v);
    if (js_abort) {
        if (c->state == CO_DONE) generator_end(c);
        return coro_stopped();
    }
    Value out = c->out_value;
    if (c->out_kind == CO_YIELDED) {
        *ret = v_obj(iter_result(out, 0));
        return C_NORMAL;
    }
    int threw = c->out_kind == CO_THREW;
    generator_end(c);
    if (threw) {
        js_exception = out;
        return C_THROW;
    }
    *ret = v_obj(iter_result(out, 1));
    return C_NORMAL;
}

/* Stop at a yield of v: what the generator is resumed with comes back
   (a value, or the throw / return it was resumed by). */
static int yield_value(Coro *c, Value v, Value *out) {
    c->out_kind = CO_YIELDED;
    c->out_value = v;
    coro_leave(c, CO_SUSPENDED);
    if (c->in_mode == 2) {
        js_exception = c->in_value;
        if (js_trace_throws) printf("[throw] (generator.throw())\n");
        return C_THROW;
    }
    if (c->in_mode == 1) { /* return(v): finish, running finally blocks on the way */
        c->returning = 1;
        c->return_value = c->in_value;
        return C_RETURN;
    }
    *out = c->in_value;
    return C_NORMAL;
}

/* yield / yield* inside a generator's body. */
static int coro_yield(Ast *n, Frame *f, Value *out) {
    Value v = v_undef();
    if (n->a) TRY(eval(n->a, f, &v));
    Coro *c = current_coro;
    if (!c || c->kind != CO_GENERATOR) return throw_error(SyntaxErrorProto, "yield outside a generator");
    if (n->op != '*') {
        if (c->async_gen) TRY(coro_await(v, &v)); /* (an async generator yields what its promise gives) */
        return yield_value(c, v, out);
    }
    /* yield*: hand each step on to another iterator */
    Value it = v_undef(), method;
    if (c->async_gen && v.t == V_OBJ && get_value(v, intern_c("@@asyncIterator"), &method) == C_NORMAL && is_callable(method)) {
        TRY(call_function(method.u.o, v, NULL, 0, &it));
    } else if (v.t == V_OBJ && get_value(v, S_iterator, &method) == C_NORMAL && is_callable(method)) {
        TRY(call_function(method.u.o, v, NULL, 0, &it));
    } else { /* (strings and array-likes: through a copy) */
        JsObj *values;
        TRY(iterate_to_array(v, &values));
        Value values_fn;
        TRY(get_value(v_obj(values), S_iterator, &values_fn));
        if (!is_callable(values_fn)) return throw_type("value is not iterable", NULL, NULL);
        TRY(call_function(values_fn.u.o, v_obj(values), NULL, 0, &it));
    }
    if (it.t != V_OBJ) return throw_type("Result of the Symbol.iterator method is not an object", NULL, NULL);
    int mode = 0;
    Value sent = v_undef();
    for (;;) {
        static JsStr *names[3];
        if (!names[0]) {
            names[0] = intern_c("next");
            names[1] = intern_c("return");
            names[2] = intern_c("throw");
        }
        Value fn, r;
        TRY(get_value(it, names[mode], &fn));
        if (!is_callable(fn)) {
            if (mode == 1) { /* no return(): just finish */
                c->returning = 1;
                c->return_value = sent;
                return C_RETURN;
            }
            if (mode == 2) {
                js_exception = sent;
                return C_THROW;
            }
            return throw_type("iterator has no next method", NULL, NULL);
        }
        TRY(call_function(fn.u.o, it, &sent, 1, &r));
        if (c->async_gen) TRY(coro_await(r, &r));
        if (r.t != V_OBJ) return throw_type("Iterator result is not an object", NULL, NULL);
        Value done, value;
        TRY(get_value(r, S_done, &done));
        TRY(get_value(r, S_value, &value));
        if (to_boolean(done)) {
            if (mode == 1) {
                c->returning = 1;
                c->return_value = value;
                return C_RETURN;
            }
            *out = value;
            return C_NORMAL;
        }
        c->out_kind = CO_YIELDED;
        c->out_value = value;
        coro_leave(c, CO_SUSPENDED);
        mode = c->in_mode;
        sent = c->in_value;
    }
}

/* ---------------------------------------------------------------- async functions */

static void async_finish(Coro *c) {
    if (!c->promise) return;
    if (c->out_kind == CO_THREW) promise_settle(c->promise, 2, c->out_value);
    else promise_resolve(c->promise, c->out_value);
}

/* Calling an async function: it runs until it waits; the promise comes back. */
static int coro_run_async(JsObj *fo, Value this_v, Value *args, int argc, Value *ret) {
    JsObj *p = promise_new();
    Coro *c = coro_new(CO_ASYNC, fo, this_v, args, argc);
    if (!c) { /* no memory for a stack: run it straight through */
        Value v;
        int r = run_function_body(fo, this_v, args, argc, NULL, &v);
        if (r == C_THROW) {
            promise_settle(p, 2, js_exception);
            js_exception = v_undef();
        } else promise_resolve(p, v);
        *ret = v_obj(p);
        return C_NORMAL;
    }
    c->promise = p;
    *ret = v_obj(p); /* (kept alive on this stack and in c) */
    coro_enter(c);
    if (c->state == CO_DONE) coro_free(c);
    return js_abort ? coro_stopped() : C_NORMAL;
}

/* A waited-for promise settled: carry on (magic 0 fulfilled, 1 rejected). */
NATIVE(async_continue) {
    (void)this_v;
    Value holder;
    if (get_value(v_obj(self), intern_c("__coro"), &holder) != C_NORMAL || holder.t != V_OBJ) return C_NORMAL;
    JsObj *h = holder.u.o;
    Coro *c = h->extra;
    *ret = v_undef();
    if (!c || c->state != CO_SUSPENDED) return C_NORMAL;
    h->extra = NULL; /* (each wait is resumed once) */
    c->in_throw = (self->magic & 0xFFFF) == 1;
    c->in_value = ARG(0);
    coro_enter(c);
    if (c->kind == CO_GENERATOR) agen_after(c);
    else if (c->state == CO_DONE) coro_free(c);
    return js_abort ? coro_stopped() : C_NORMAL;
}

/* await v */
static int coro_await(Value v, Value *out) {
    Coro *c = current_coro;
    if (c && (c->kind == CO_ASYNC || c->async_gen)) { /* stop here until v settles (always: a later microtask) */
        JsObj *p;
        if (v.t == V_OBJ && v.u.o->cls == CL_PROMISE) p = v.u.o;
        else {
            p = promise_new();
            promise_resolve(p, v);
        }
        JsObj *holder = obj_new_raw(CL_GENERATOR, ObjectProto); /* (holds c for the two callbacks) */
        holder->extra = c;
        holder->magic = 2;
        JsObj *ok = make_native("", async_continue, 1, 0), *bad = make_native("", async_continue, 1, 1);
        prop_set_raw(ok, intern_c("__coro"), v_obj(holder), P_HIDDEN);
        prop_set_raw(bad, intern_c("__coro"), v_obj(holder), P_HIDDEN);
        promise_then(p, v_obj(ok), v_obj(bad));
        c->out_kind = CO_WAITING;
        coro_leave(c, CO_SUSPENDED);
        if (c->in_throw) {
            js_exception = c->in_value;
            if (js_trace_throws) printf("[throw] (an awaited promise was rejected)\n");
            c->in_throw = 0;
            return C_THROW;
        }
        *out = c->in_value;
        return C_NORMAL;
    }
    /* nowhere to wait (no stack for it): a settled promise's outcome now */
    JsObj *p = v.t == V_OBJ && v.u.o->cls == CL_PROMISE ? v.u.o : NULL;
    if (p) {
        if (p->magic == 2) {
            js_exception = p->this_val;
            return C_THROW;
        }
        *out = p->magic == 1 ? p->this_val : v_undef();
        return C_NORMAL;
    }
    *out = v;
    return C_NORMAL;
}

/* ---------------------------------------------------------------- the collector */

/* Values held by coroutines: their own fields (exactly) and, for stopped
   ones, their stacks (conservatively; needs gc_index built). */
static int coro_any(void) { return live_coros != NULL; }

static void coro_gc_mark(void *p) {
    Coro *c = p;
    if (!c || c->marked) return;
    c->marked = 1;
    if (c->fo) gc_push(&c->fo->gc);
    gc_value(c->this_v);
    for (int i = 0; i < c->argc; i++) gc_value(c->args[i]);
    gc_value(c->in_value);
    gc_value(c->out_value);
    if (c->promise) gc_push(&c->promise->gc);
    if (c->gen) gc_push(&c->gen->gc);
    gc_value(c->return_value);
    /* a stopped one's stack (the running one's is scanned by the caller) */
    if (gc_index_fresh && c != current_coro && c->state == CO_SUSPENDED && c->rsp)
        gc_scan_range((const char *)c->rsp, c->stack + CORO_STACK);
}

/* Roots: async functions waiting (a promise will resume them), and the
   coroutines that resumed the running one. A stopped generator is marked
   only through its generator object. */
static void coro_gc_roots(int scan_stacks) {
    (void)scan_stacks;
    for (Coro *c = live_coros; c; c = c->next_live) c->marked = 0;
    for (Coro *c = live_coros; c; c = c->next_live)
        if (c->kind == CO_ASYNC) coro_gc_mark(c);
    for (Coro *c = current_coro; c && c != &main_coro; c = c->caller) coro_gc_mark(c);
    if (gc_index_fresh && current_coro && main_coro.rsp) gc_scan_range((const char *)main_coro.rsp, js_stack_top);
    if (gc_index_fresh && current_coro && coro_detour_sp) gc_scan_range(coro_detour_sp, current_coro->stack + CORO_STACK);
}

static void coro_init(JsObj *global) {
    (void)global;
    GeneratorProto = obj_new_raw(CL_OBJECT, IteratorProto);
    define_method(GeneratorProto, "next", generator_resume, 1, 0);
    define_method(GeneratorProto, "return", generator_resume, 1, 1);
    define_method(GeneratorProto, "throw", generator_resume, 1, 2);
    prop_set_raw(GeneratorProto, intern_c("@@toStringTag"), v_str(intern_c("Generator")), P_HIDDEN);
    AsyncGeneratorProto = obj_new_raw(CL_OBJECT, ObjectProto);
    define_method(AsyncGeneratorProto, "next", generator_resume, 1, 0);
    define_method(AsyncGeneratorProto, "return", generator_resume, 1, 1);
    define_method(AsyncGeneratorProto, "throw", generator_resume, 1, 2);
    prop_set_raw(AsyncGeneratorProto, intern_c("@@toStringTag"), v_str(intern_c("AsyncGenerator")), P_HIDDEN);
    JsObj *self_iter = make_native("[Symbol.asyncIterator]", noop_this, 0, 0);
    prop_set_raw(AsyncGeneratorProto, intern_c("@@asyncIterator"), v_obj(self_iter), P_HIDDEN);
}
