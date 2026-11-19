/* c-comp.exe - a C compiler that runs inside RDX.

   Usage:  c-comp file.c            writes file.exe
           c-comp file.c -o name    writes name

   It compiles a practical subset of C straight to x86-64 machine code and
   writes a PE executable the RDX kernel can load. No assembler, no linker.

   Supported: char/short/int/long/long long (signed and unsigned), pointers,
   arrays, structs, unions, enums, typedef, globals with initializers,
   functions (including variadic ones), every C operator, if/else, while,
   do-while, for, switch, break, continue, return, sizeof, casts, string and
   character literals, #include "file" and simple #define NAME value.

   Not supported: floating point, function pointers, goto, bit-fields,
   function-like macros, #if/#ifdef, passing or returning structs by value.

   Every program is compiled together with clib.c (the runtime: printf,
   malloc, strings, system calls), so #include "rdx.h" needs no file.

   How the generated code works: every expression leaves its value in rax.
   Binary operators evaluate the left side, push it, evaluate the right
   side, then combine. Locals live at [rbp - n]. Arguments are passed on the
   stack in 8-byte slots, first argument lowest, and the caller cleans up. */
#include "rdx.h"

#define IMAGE_BASE 0x10000400000ull

/* ================================================================ basics */

static const char *cur_file = "c-comp";
static int cur_line;

__attribute__((noreturn)) static void error(const char *fmt, ...) {
    screen_color(0xFF5C57);
    printf("%s:%d: error: ", cur_file, cur_line);
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    putchar('\n');
    screen_color(COLOR_DEFAULT);
    exit(1);
}

static void *alloc(size_t size) {
    void *p = malloc(size);
    if (!p) error("out of memory");
    memset(p, 0, size);
    return p;
}

static int align_up(int value, int to) { return (value + to - 1) / to * to; }

typedef struct {
    unsigned char *p;
    int len, cap;
} Buf;

static void buf_reserve(Buf *b, int len) {
    if (len > b->cap) {
        int cap = b->cap ? b->cap : 4096;
        while (cap < len) cap *= 2;
        b->p = realloc(b->p, (size_t)cap);
        if (!b->p) error("out of memory");
        memset(b->p + b->cap, 0, (size_t)(cap - b->cap));
        b->cap = cap;
    }
}

static void buf_put(Buf *b, const void *bytes, int n) {
    buf_reserve(b, b->len + n);
    memcpy(b->p + b->len, bytes, (size_t)n);
    b->len += n;
}

static void store32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void store64(unsigned char *p, u64 v) {
    store32(p, (unsigned)v);
    store32(p + 4, (unsigned)(v >> 32));
}

typedef struct {
    int *v;
    int n, cap;
} IntList;

static void list_add(IntList *l, int x) {
    if (l->n == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->v = realloc(l->v, sizeof(int) * (size_t)l->cap);
    }
    l->v[l->n++] = x;
}

/* Names are interned so they can be compared by pointer. */
static char **names;
static int name_count, name_cap;

static char *intern(const char *s, int len) {
    for (int i = 0; i < name_count; i++) {
        if ((int)strlen(names[i]) == len && memcmp(names[i], s, (size_t)len) == 0) return names[i];
    }
    if (name_count == name_cap) {
        name_cap = name_cap ? name_cap * 2 : 256;
        names = realloc(names, sizeof(char *) * (size_t)name_cap);
    }
    char *copy = alloc((size_t)len + 1);
    memcpy(copy, s, (size_t)len);
    return names[name_count++] = copy;
}

static char *load_file(const char *name) {
    i64 size = file_read(name, NULL, 0);
    if (size < 0) error("cannot find the file '%s'", name);
    char *text = alloc((size_t)size + 1);
    file_read(name, text, (size_t)size);
    return text;
}

/* ================================================================ lexer */

enum {
    TK_EOF = 256, TK_NUM, TK_STR, TK_ID,
    P_EQ, P_NE, P_LE, P_GE, P_ANDAND, P_OROR, P_SHL, P_SHR, P_INC, P_DEC, P_ARROW,
    P_ADDEQ, P_SUBEQ, P_MULEQ, P_DIVEQ, P_MODEQ, P_ANDEQ, P_OREQ, P_XOREQ, P_SHLEQ, P_SHREQ, P_ELLIPSIS,
    K_INT, K_CHAR, K_SHORT, K_LONG, K_VOID, K_UNSIGNED, K_SIGNED, K_STRUCT, K_UNION, K_ENUM, K_TYPEDEF,
    K_STATIC, K_EXTERN, K_CONST, K_VOLATILE, K_REGISTER, K_INLINE,
    K_IF, K_ELSE, K_WHILE, K_FOR, K_DO, K_RETURN, K_BREAK, K_CONTINUE, K_SWITCH, K_CASE, K_DEFAULT, K_SIZEOF
};

static const struct {
    const char *word;
    int kind;
} keywords[] = {
    {"int", K_INT}, {"char", K_CHAR}, {"short", K_SHORT}, {"long", K_LONG}, {"void", K_VOID},
    {"unsigned", K_UNSIGNED}, {"signed", K_SIGNED}, {"struct", K_STRUCT}, {"union", K_UNION},
    {"enum", K_ENUM}, {"typedef", K_TYPEDEF}, {"static", K_STATIC}, {"extern", K_EXTERN},
    {"const", K_CONST}, {"volatile", K_VOLATILE}, {"register", K_REGISTER}, {"inline", K_INLINE},
    {"if", K_IF}, {"else", K_ELSE}, {"while", K_WHILE}, {"for", K_FOR}, {"do", K_DO},
    {"return", K_RETURN}, {"break", K_BREAK}, {"continue", K_CONTINUE}, {"switch", K_SWITCH},
    {"case", K_CASE}, {"default", K_DEFAULT}, {"sizeof", K_SIZEOF},
};

static const struct {
    const char *text;
    int kind;
} puncts[] = {
    {"<<=", P_SHLEQ}, {">>=", P_SHREQ}, {"...", P_ELLIPSIS},
    {"==", P_EQ}, {"!=", P_NE}, {"<=", P_LE}, {">=", P_GE}, {"&&", P_ANDAND}, {"||", P_OROR},
    {"<<", P_SHL}, {">>", P_SHR}, {"++", P_INC}, {"--", P_DEC}, {"->", P_ARROW},
    {"+=", P_ADDEQ}, {"-=", P_SUBEQ}, {"*=", P_MULEQ}, {"/=", P_DIVEQ}, {"%=", P_MODEQ},
    {"&=", P_ANDEQ}, {"|=", P_OREQ}, {"^=", P_XOREQ},
};

typedef struct Macro {
    char *name;
    char *text;
    int busy; /* being expanded right now (stops recursion) */
    struct Macro *next;
} Macro;

/* Where characters are coming from: a file, or the text of a macro. */
typedef struct {
    const char *p;
    const char *file;
    int line;
    int at_line_start;
    Macro *macro;
} Input;

static Input inputs[24];
static int input_count;
static Macro *macros;

static struct {
    int kind;
    i64 num;
    int is_wide, is_unsigned; /* for numbers: needs 64 bits / has a u suffix */
    char *name;
    char *str;
    int str_len;
} tok;

static void push_input(const char *text, const char *file, int line, Macro *macro) {
    if (input_count == 24) error("#include or macro nesting is too deep");
    Input *in = &inputs[input_count++];
    in->p = text;
    in->file = file;
    in->line = line;
    in->at_line_start = 1;
    in->macro = macro;
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static int hex_value(char c) {
    if (is_digit(c)) return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* Decode one (possibly escaped) character of a string or char literal. */
static int read_char(Input *in) {
    int c = (unsigned char)*in->p++;
    if (c != '\\') return c;
    c = *in->p++;
    switch (c) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'b': return '\b';
    case 'e': return 27;
    case '0': return 0;
    case 'x': {
        int v = 0;
        while (hex_value(*in->p) >= 0) v = v * 16 + hex_value(*in->p++);
        return v & 0xFF;
    }
    default: return c;
    }
}

static const char *builtin_headers[] = {"rdx.h", "stdio.h", "stdlib.h", "string.h", "stddef.h", "stdint.h"};

static void directive(Input *in) {
    in->p++; /* the '#' */
    while (*in->p == ' ' || *in->p == '\t') in->p++;
    const char *start = in->p;
    while (is_alpha(*in->p)) in->p++;
    int len = (int)(in->p - start);
    while (*in->p == ' ' || *in->p == '\t') in->p++;
    cur_file = in->file;
    cur_line = in->line;

    if (len == 7 && memcmp(start, "include", 7) == 0) {
        char close = *in->p == '<' ? '>' : '"';
        if (*in->p != '"' && *in->p != '<') error("#include needs a file name in quotes");
        const char *name_start = ++in->p;
        while (*in->p && *in->p != close && *in->p != '\n') in->p++;
        char *name = intern(name_start, (int)(in->p - name_start));
        while (*in->p && *in->p != '\n') in->p++;
        /* The runtime is always compiled in, so its headers are no-ops. */
        for (size_t i = 0; i < sizeof(builtin_headers) / sizeof(builtin_headers[0]); i++) {
            if (strcmp(name, builtin_headers[i]) == 0) return;
        }
        push_input(load_file(name), name, 1, NULL);
    } else if (len == 6 && memcmp(start, "define", 6) == 0) {
        const char *name_start = in->p;
        while (is_alpha(*in->p) || is_digit(*in->p)) in->p++;
        if (in->p == name_start) error("#define needs a name");
        Macro *m = alloc(sizeof(Macro));
        m->name = intern(name_start, (int)(in->p - name_start));
        if (*in->p == '(') error("macros with parameters are not supported (write a function instead)");
        const char *text = in->p;
        while (*in->p && *in->p != '\n' && !(in->p[0] == '/' && (in->p[1] == '/' || in->p[1] == '*'))) in->p++;
        m->text = alloc((size_t)(in->p - text) + 1);
        memcpy(m->text, text, (size_t)(in->p - text));
        while (*in->p && *in->p != '\n') in->p++;
        m->next = macros;
        macros = m;
    } else if (len == 6 && memcmp(start, "pragma", 6) == 0) {
        while (*in->p && *in->p != '\n') in->p++;
    } else {
        error("unsupported preprocessor directive (only #include, #define and #pragma work)");
    }
}

static void next(void) {
    for (;;) {
        if (input_count == 0) {
            tok.kind = TK_EOF;
            return;
        }
        Input *in = &inputs[input_count - 1];
        char c = *in->p;
        if (!c) {
            if (in->macro) in->macro->busy = 0;
            input_count--;
            continue;
        }
        if (c == '\n') {
            in->line++;
            in->p++;
            in->at_line_start = 1;
            continue;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            in->p++;
            continue;
        }
        if (c == '/' && in->p[1] == '/') {
            while (*in->p && *in->p != '\n') in->p++;
            continue;
        }
        if (c == '/' && in->p[1] == '*') {
            in->p += 2;
            while (*in->p && !(in->p[0] == '*' && in->p[1] == '/')) {
                if (*in->p == '\n') in->line++;
                in->p++;
            }
            if (*in->p) in->p += 2;
            continue;
        }
        if (c == '#' && in->at_line_start && !in->macro) {
            directive(in);
            continue;
        }
        in->at_line_start = 0;
        cur_file = in->file;
        cur_line = in->line;

        if (is_digit(c)) {
            u64 n = 0;
            if (c == '0' && (in->p[1] == 'x' || in->p[1] == 'X')) {
                in->p += 2;
                while (hex_value(*in->p) >= 0) n = n * 16 + (u64)hex_value(*in->p++);
            } else if (c == '0') {
                while (*in->p >= '0' && *in->p <= '7') n = n * 8 + (u64)(*in->p++ - '0');
            } else {
                while (is_digit(*in->p)) n = n * 10 + (u64)(*in->p++ - '0');
            }
            tok.is_unsigned = 0;
            int longs = 0;
            for (;; in->p++) {
                if (*in->p == 'u' || *in->p == 'U') tok.is_unsigned = 1;
                else if (*in->p == 'l' || *in->p == 'L') longs++;
                else break;
            }
            tok.is_wide = longs >= 2 || n > 0x7FFFFFFFull;
            if (n > 0x7FFFFFFFFFFFFFFFull) tok.is_unsigned = 1;
            tok.kind = TK_NUM;
            tok.num = (i64)n;
            return;
        }
        if (is_alpha(c)) {
            const char *start = in->p;
            while (is_alpha(*in->p) || is_digit(*in->p)) in->p++;
            tok.name = intern(start, (int)(in->p - start));
            Macro *m = macros;
            while (m && m->name != tok.name) m = m->next;
            if (m && !m->busy) {
                m->busy = 1;
                push_input(m->text, in->file, in->line, m);
                continue;
            }
            tok.kind = TK_ID;
            for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
                if (strcmp(tok.name, keywords[i].word) == 0) tok.kind = keywords[i].kind;
            }
            return;
        }
        if (c == '\'') {
            in->p++;
            tok.num = read_char(in);
            if (*in->p != '\'') error("character literal is missing its closing quote");
            in->p++;
            tok.kind = TK_NUM;
            tok.is_wide = tok.is_unsigned = 0;
            return;
        }
        if (c == '"') {
            in->p++;
            Buf b = {0};
            while (*in->p != '"') {
                if (!*in->p || *in->p == '\n') error("string is missing its closing quote");
                char ch = (char)read_char(in);
                buf_put(&b, &ch, 1);
            }
            in->p++;
            buf_put(&b, "", 1);
            tok.kind = TK_STR;
            tok.str = (char *)b.p;
            tok.str_len = b.len - 1;
            return;
        }
        for (size_t i = 0; i < sizeof(puncts) / sizeof(puncts[0]); i++) {
            size_t n = strlen(puncts[i].text);
            if (memcmp(in->p, puncts[i].text, n) == 0) {
                in->p += n;
                tok.kind = puncts[i].kind;
                return;
            }
        }
        tok.kind = (unsigned char)c;
        in->p++;
        return;
    }
}

static int accept(int kind) {
    if (tok.kind != kind) return 0;
    next();
    return 1;
}

static void expect(int kind, const char *what) {
    if (tok.kind == kind) {
        next();
    } else if (tok.kind == TK_ID) {
        error("expected %s, but found '%s'", what, tok.name);
    } else if (tok.kind < 256) {
        error("expected %s, but found '%c'", what, tok.kind);
    } else if (tok.kind == TK_EOF) {
        error("expected %s, but the file ended", what);
    } else {
        error("expected %s here", what);
    }
}

/* ================================================================ types */

enum { TY_VOID, TY_CHAR, TY_SHORT, TY_INT, TY_LLONG, TY_PTR, TY_ARRAY, TY_STRUCT };

typedef struct Type Type;
typedef struct Member Member;

struct Type {
    int kind, size, align;
    int is_unsigned;
    int len;      /* arrays: element count, or -1 if not yet known */
    int complete; /* structs: has its member list been seen? */
    Type *base;   /* pointers and arrays */
    Member *members;
};

struct Member {
    char *name;
    Type *type;
    int offset;
    Member *next;
};

static Type *ty_void, *ty_char, *ty_uchar, *ty_short, *ty_ushort, *ty_int, *ty_uint, *ty_llong, *ty_ullong;

static Type *new_type(int kind, int size, int is_unsigned) {
    Type *t = alloc(sizeof(Type));
    t->kind = kind;
    t->size = size;
    t->align = size ? size : 1;
    t->is_unsigned = is_unsigned;
    return t;
}

static Type *pointer_to(Type *base) {
    Type *t = new_type(TY_PTR, 8, 1);
    t->base = base;
    return t;
}

static Type *array_of(Type *base, int len) {
    Type *t = new_type(TY_ARRAY, len < 0 ? 0 : base->size * len, 0);
    t->align = base->align;
    t->base = base;
    t->len = len;
    return t;
}

static int is_integer(Type *t) { return t->kind >= TY_CHAR && t->kind <= TY_LLONG; }
static int is_scalar(Type *t) { return is_integer(t) || t->kind == TY_PTR; }

/* Arrays turn into pointers to their first element when used as values. */
static Type *decay(Type *t) { return t->kind == TY_ARRAY ? pointer_to(t->base) : t; }

/* ================================================================ symbols */

enum { SY_LOCAL, SY_GLOBAL, SY_FUNC, SY_TYPEDEF, SY_CONST };
enum { SEC_DATA, SEC_STR, SEC_BSS };

typedef struct Sym {
    char *name;
    int kind;
    Type *type;      /* variable type, typedef target, or function return type */
    i64 value;       /* local: rbp offset; global: offset in its section; const: value */
    int section;     /* globals */
    int defined;     /* functions: has a body */
    int code_offset; /* functions */
    int param_count, variadic;
    IntList calls;   /* functions: call sites waiting for the address */
    struct Sym *next;
} Sym;

typedef struct Tag {
    char *name;
    Type *type;
    struct Tag *next;
} Tag;

static Sym *locals, *globals;
static Tag *tags;
static int in_function;

static Sym *find(char *name) {
    for (Sym *s = locals; s; s = s->next) {
        if (s->name == name) return s;
    }
    for (Sym *s = globals; s; s = s->next) {
        if (s->name == name) return s;
    }
    return NULL;
}

static Sym *find_global(char *name) {
    for (Sym *s = globals; s; s = s->next) {
        if (s->name == name) return s;
    }
    return NULL;
}

static Sym *add_sym(char *name, int kind, Type *type, int local) {
    Sym *s = alloc(sizeof(Sym));
    s->name = name;
    s->kind = kind;
    s->type = type;
    if (local) {
        s->next = locals;
        locals = s;
    } else {
        s->next = globals;
        globals = s;
    }
    return s;
}

/* ================================================================ output buffers */

static Buf code; /* machine code (.text) */
static Buf data; /* initialised globals */
static Buf strs; /* string literals */
static int bss_size;

/* A place that must be filled with an absolute address once the final
   layout of the executable is known. */
typedef struct {
    int pos;
    int in_data; /* the slot is in `data` rather than `code` */
    int section;
    i64 addend;
} Fixup;

static Fixup *fixups;
static int fixup_count, fixup_cap;

static void add_fixup(int pos, int in_data, int section, i64 addend) {
    if (fixup_count == fixup_cap) {
        fixup_cap = fixup_cap ? fixup_cap * 2 : 256;
        fixups = realloc(fixups, sizeof(Fixup) * (size_t)fixup_cap);
    }
    Fixup f = {pos, in_data, section, addend};
    fixups[fixup_count++] = f;
}

static void emit(const char *bytes, int n) { buf_put(&code, bytes, n); }
#define EMIT(bytes) emit(bytes, (int)sizeof(bytes) - 1)

static void emit32(int v) {
    unsigned char b[4];
    store32(b, (unsigned)v);
    buf_put(&code, b, 4);
}

static void emit64(i64 v) {
    unsigned char b[8];
    store64(b, (u64)v);
    buf_put(&code, b, 8);
}

/* movabs rax, <address of section + addend> */
static void emit_address(int section, i64 addend) {
    EMIT("\x48\xB8");
    add_fixup(code.len, 0, section, addend);
    emit64(0);
}

/* Emit a jump with an unknown target; returns where to patch it.
   cc: 0 = always, 0x84 = if zero, 0x85 = if not zero. */
static int jump_forward(int cc) {
    if (cc) {
        char op[2] = {0x0F, (char)cc};
        emit(op, 2);
    } else {
        EMIT("\xE9");
    }
    emit32(0);
    return code.len - 4;
}

static void land_here(int patch) { store32(code.p + patch, (unsigned)(code.len - (patch + 4))); }

static void jump_back(int cc, int target) {
    if (cc) {
        char op[2] = {0x0F, (char)cc};
        emit(op, 2);
    } else {
        EMIT("\xE9");
    }
    emit32(target - (code.len + 4));
}

static void test_rax(void) { EMIT("\x48\x85\xC0"); }

/* Append the string literal(s) at the current token; returns the offset. */
static int last_string_len;

static int add_string(void) {
    int offset = strs.len;
    last_string_len = 0;
    while (tok.kind == TK_STR) { /* "adjacent" "strings" are joined */
        buf_put(&strs, tok.str, tok.str_len);
        last_string_len += tok.str_len;
        next();
    }
    buf_put(&strs, "", 1);
    return offset;
}

/* ================================================================ expression trees */

enum {
    ND_NUM, ND_STR, ND_VAR, ND_ADDR, ND_DEREF, ND_MEMBER, ND_ASSIGN, ND_ASSIGNOP, ND_POSTINC,
    ND_ADD, ND_SUB, ND_MUL, ND_DIV, ND_MOD, ND_AND, ND_OR, ND_XOR, ND_SHL, ND_SHR,
    ND_EQ, ND_NE, ND_LT, ND_LE,
    ND_LOGAND, ND_LOGOR, ND_NOT, ND_BITNOT, ND_NEG, ND_COND, ND_CALL, ND_SYSCALL, ND_CAST, ND_COMMA
};

typedef struct Node Node;
struct Node {
    int kind;
    Type *type;
    Node *lhs, *rhs, *third;
    Node *next; /* next call argument */
    i64 val;
    Sym *sym;
    Member *member;
    int is_unsigned; /* how to divide, shift right and compare */
    int arg_count;
};

static Node *node(int kind, Type *type, Node *lhs, Node *rhs) {
    Node *n = alloc(sizeof(Node));
    n->kind = kind;
    n->type = type;
    n->lhs = lhs;
    n->rhs = rhs;
    return n;
}

static Node *number(i64 value, Type *type) {
    Node *n = node(ND_NUM, type, NULL, NULL);
    n->val = value;
    return n;
}

static int is_lvalue(Node *n) { return n->kind == ND_VAR || n->kind == ND_DEREF || n->kind == ND_MEMBER; }

/* char and short are widened to int before arithmetic. */
static Type *promote(Type *t) {
    if (!is_integer(t)) error("this operator needs a number");
    return t->size < 4 ? ty_int : t;
}

/* The type two operands are brought to before an arithmetic operation. */
static Type *common_type(Type *a, Type *b) {
    a = decay(a);
    b = decay(b);
    if (a->kind == TY_PTR) return a;
    if (b->kind == TY_PTR) return b;
    if (!is_integer(a) || !is_integer(b)) error("these operands cannot be combined");
    if (a->size == 8 || b->size == 8) {
        return (a->size == 8 && a->is_unsigned) || (b->size == 8 && b->is_unsigned) ? ty_ullong : ty_llong;
    }
    return (a->size == 4 && a->is_unsigned) || (b->size == 4 && b->is_unsigned) ? ty_uint : ty_int;
}

static Node *cast(Node *e, Type *to) {
    if (to->kind == TY_STRUCT || to->kind == TY_ARRAY) error("cannot cast to a struct or array");
    return node(ND_CAST, to, e, NULL);
}

static Node *arith(int kind, Node *l, Node *r) {
    Type *t = common_type(l->type, r->type);
    if (t->kind == TY_PTR) error("this operator cannot be used on pointers");
    Node *n = node(kind, t, l, r);
    n->is_unsigned = t->is_unsigned;
    return n;
}

static Node *new_add(Node *l, Node *r) {
    Type *lt = decay(l->type), *rt = decay(r->type);
    if (lt->kind == TY_PTR && rt->kind == TY_PTR) error("two pointers cannot be added");
    if (rt->kind == TY_PTR) { /* number + pointer */
        Node *swap = l;
        l = r;
        r = swap;
        lt = rt;
    }
    if (lt->kind == TY_PTR) {
        if (lt->base->size == 0) error("arithmetic on a pointer to an incomplete type");
        if (!is_integer(r->type)) error("only a number can be added to a pointer");
        r = node(ND_MUL, ty_llong, r, number(lt->base->size, ty_llong));
        return node(ND_ADD, lt, l, r);
    }
    return arith(ND_ADD, l, r);
}

static Node *new_sub(Node *l, Node *r) {
    Type *lt = decay(l->type), *rt = decay(r->type);
    if (lt->kind == TY_PTR && rt->kind == TY_PTR) { /* distance between two pointers */
        Node *diff = node(ND_SUB, ty_llong, l, r);
        return node(ND_DIV, ty_llong, diff, number(lt->base->size ? lt->base->size : 1, ty_llong));
    }
    if (lt->kind == TY_PTR) {
        if (lt->base->size == 0) error("arithmetic on a pointer to an incomplete type");
        if (!is_integer(r->type)) error("only a number can be subtracted from a pointer");
        r = node(ND_MUL, ty_llong, r, number(lt->base->size, ty_llong));
        return node(ND_SUB, lt, l, r);
    }
    if (rt->kind == TY_PTR) error("a pointer cannot be subtracted from a number");
    return arith(ND_SUB, l, r);
}

static Node *binary(int kind, Node *l, Node *r) {
    switch (kind) {
    case ND_ADD: return new_add(l, r);
    case ND_SUB: return new_sub(l, r);
    case ND_SHL:
    case ND_SHR: {
        Type *t = promote(l->type);
        promote(r->type);
        Node *n = node(kind, t, l, r);
        n->is_unsigned = t->is_unsigned;
        return n;
    }
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE: {
        Type *t = common_type(l->type, r->type);
        Node *n = node(kind, ty_int, l, r);
        n->is_unsigned = t->is_unsigned;
        return n;
    }
    default: return arith(kind, l, r);
    }
}

static Node *deref(Node *e) {
    Type *t = decay(e->type);
    if (t->kind != TY_PTR) error("only a pointer or array can be dereferenced or indexed");
    if (t->base->kind == TY_VOID) error("a void pointer cannot be dereferenced (cast it first)");
    return node(ND_DEREF, t->base, e, NULL);
}

static Node *member_of(Node *e, char *name) {
    if (e->type->kind != TY_STRUCT) error("'%s' is requested from something that is not a struct", name);
    for (Member *m = e->type->members; m; m = m->next) {
        if (m->name == name) {
            Node *n = node(ND_MEMBER, m->type, e, NULL);
            n->member = m;
            return n;
        }
    }
    error("the struct has no member called '%s'", name);
}

static Node *assign_op(int kind, Node *target, Node *value) {
    if (!is_lvalue(target) || !is_scalar(target->type)) error("this cannot be modified");
    Node *op = binary(kind, target, value);
    if (op->lhs != target) error("invalid operands for a compound assignment");
    Node *n = node(ND_ASSIGNOP, target->type, target, NULL);
    n->third = op; /* carries the operator, the scaled right side and the operation type */
    return n;
}

/* Evaluate an expression that must be known at compile time. */
static i64 const_eval(Node *n) {
    switch (n->kind) {
    case ND_NUM: return n->val;
    case ND_NEG: return -const_eval(n->lhs);
    case ND_NOT: return !const_eval(n->lhs);
    case ND_BITNOT: return ~const_eval(n->lhs);
    case ND_COND: return const_eval(n->lhs) ? const_eval(n->rhs) : const_eval(n->third);
    case ND_LOGAND: return const_eval(n->lhs) && const_eval(n->rhs);
    case ND_LOGOR: return const_eval(n->lhs) || const_eval(n->rhs);
    case ND_CAST: {
        i64 v = const_eval(n->lhs);
        switch (n->type->size) {
        case 1: return n->type->is_unsigned ? (i64)(unsigned char)v : (i64)(signed char)v;
        case 2: return n->type->is_unsigned ? (i64)(unsigned short)v : (i64)(short)v;
        case 4: return n->type->is_unsigned ? (i64)(unsigned)v : (i64)(int)v;
        default: return v;
        }
    }
    default: break;
    }
    if (n->kind >= ND_ADD && n->kind <= ND_LE) {
        i64 a = const_eval(n->lhs), b = const_eval(n->rhs);
        switch (n->kind) {
        case ND_ADD: return a + b;
        case ND_SUB: return a - b;
        case ND_MUL: return a * b;
        case ND_DIV:
        case ND_MOD:
            if (b == 0) error("division by zero in a constant");
            return n->kind == ND_DIV ? a / b : a % b;
        case ND_AND: return a & b;
        case ND_OR: return a | b;
        case ND_XOR: return a ^ b;
        case ND_SHL: return a << b;
        case ND_SHR: return a >> b;
        case ND_EQ: return a == b;
        case ND_NE: return a != b;
        case ND_LT: return a < b;
        case ND_LE: return a <= b;
        }
    }
    error("this value must be a constant known at compile time");
}

/* ================================================================ declarations (types) */

static Node *assign(void);
static Node *expr(void);
static Type *declarator(Type *base, char **name);
static Type *declspec(int *is_typedef, int *is_static, int *is_extern);

static int is_type_start(void) {
    switch (tok.kind) {
    case K_INT: case K_CHAR: case K_SHORT: case K_LONG: case K_VOID: case K_UNSIGNED: case K_SIGNED:
    case K_STRUCT: case K_UNION: case K_ENUM: case K_TYPEDEF: case K_STATIC: case K_EXTERN:
    case K_CONST: case K_VOLATILE: case K_REGISTER: case K_INLINE:
        return 1;
    case TK_ID: {
        Sym *s = find(tok.name);
        return s && s->kind == SY_TYPEDEF;
    }
    default:
        return 0;
    }
}

static Type *struct_decl(int is_union) {
    Type *t = NULL;
    if (tok.kind == TK_ID) {
        Tag *tag = tags;
        while (tag && tag->name != tok.name) tag = tag->next;
        if (!tag) {
            tag = alloc(sizeof(Tag));
            tag->name = tok.name;
            tag->type = new_type(TY_STRUCT, 0, 0);
            tag->next = tags;
            tags = tag;
        }
        t = tag->type;
        next();
    } else {
        t = new_type(TY_STRUCT, 0, 0);
    }
    if (!accept('{')) return t;
    if (t->complete) error("this struct is defined twice");

    int offset = 0, max_align = 1, max_size = 0;
    Member head = {0}, *tail = &head;
    while (!accept('}')) {
        if (!is_type_start()) error("expected a member declaration inside the struct");
        Type *base = declspec(NULL, NULL, NULL);
        for (;;) {
            char *name = NULL;
            Type *mt = declarator(base, &name);
            if (!name) error("a struct member needs a name");
            if (mt->size == 0) error("member '%s' has an incomplete type", name);
            Member *m = alloc(sizeof(Member));
            m->name = name;
            m->type = mt;
            if (is_union) {
                if (mt->size > max_size) max_size = mt->size;
            } else {
                offset = align_up(offset, mt->align);
                m->offset = offset;
                offset += mt->size;
            }
            if (mt->align > max_align) max_align = mt->align;
            tail = tail->next = m;
            if (!accept(',')) break;
        }
        expect(';', "';' after the struct member");
    }
    t->members = head.next;
    t->align = max_align;
    t->size = align_up(is_union ? max_size : offset, max_align);
    if (t->size == 0) t->size = max_align;
    t->complete = 1;
    return t;
}

static void enum_decl(void) {
    if (tok.kind == TK_ID) next(); /* the tag is not needed: enums are ints */
    if (!accept('{')) return;
    i64 value = 0;
    while (!accept('}')) {
        char *name = tok.name;
        expect(TK_ID, "a name for the enum value");
        if (accept('=')) value = const_eval(assign());
        Sym *s = add_sym(name, SY_CONST, ty_int, in_function);
        s->value = value++;
        if (!accept(',')) {
            expect('}', "'}' to end the enum");
            break;
        }
    }
}

/* Parse the leading part of a declaration: `static unsigned long`, etc.
   Returns NULL if no type is present. */
static Type *declspec(int *is_typedef, int *is_static, int *is_extern) {
    int is_unsigned = 0, longs = 0, seen = 0;
    int base = 0; /* K_VOID, K_CHAR, K_SHORT, or 0 for int */
    Type *named = NULL;
    for (;;) {
        int k = tok.kind;
        if (k == K_CONST || k == K_VOLATILE || k == K_REGISTER || k == K_INLINE) {
            next();
            continue;
        }
        if (k == K_TYPEDEF || k == K_STATIC || k == K_EXTERN) {
            int *flag = k == K_TYPEDEF ? is_typedef : k == K_STATIC ? is_static : is_extern;
            if (!flag) error("a storage class is not allowed here");
            *flag = 1;
            next();
            continue;
        }
        if (k == K_UNSIGNED || k == K_SIGNED) {
            is_unsigned = k == K_UNSIGNED;
        } else if (k == K_LONG) {
            longs++;
        } else if (k == K_INT) {
            /* nothing to record */
        } else if (k == K_VOID || k == K_CHAR || k == K_SHORT) {
            base = k;
        } else if (k == K_STRUCT || k == K_UNION) {
            next();
            named = struct_decl(k == K_UNION);
            seen = 1;
            continue;
        } else if (k == K_ENUM) {
            next();
            enum_decl();
            seen = 1;
            continue;
        } else if (k == TK_ID && !seen) {
            Sym *s = find(tok.name);
            if (!s || s->kind != SY_TYPEDEF) break;
            named = s->type;
        } else {
            break;
        }
        seen = 1;
        next();
    }
    if (!seen) return NULL;
    if (named) return named;
    if (base == K_VOID) return ty_void;
    if (base == K_CHAR) return is_unsigned ? ty_uchar : ty_char;
    if (base == K_SHORT) return is_unsigned ? ty_ushort : ty_short;
    if (longs >= 2) return is_unsigned ? ty_ullong : ty_llong;
    return is_unsigned ? ty_uint : ty_int; /* int and long are both 32-bit */
}

static Type *array_suffix(Type *base) {
    if (!accept('[')) return base;
    int len = -1;
    if (tok.kind != ']') {
        len = (int)const_eval(assign());
        if (len < 0) error("an array cannot have a negative size");
    }
    expect(']', "']'");
    base = array_suffix(base);
    if (base->size == 0) error("array of an incomplete type");
    return array_of(base, len);
}

/* Parse `*name[10]` after the base type. *name is left NULL if absent. */
static Type *declarator(Type *base, char **name) {
    while (accept('*')) {
        base = pointer_to(base);
        while (tok.kind == K_CONST || tok.kind == K_VOLATILE) next();
    }
    if (tok.kind == '(') error("function pointers and parenthesised declarators are not supported");
    *name = NULL;
    if (tok.kind == TK_ID) {
        *name = tok.name;
        next();
    }
    return array_suffix(base);
}

/* A type with no name, as in a cast or sizeof. */
static Type *type_name(void) {
    Type *base = declspec(NULL, NULL, NULL);
    char *name;
    Type *t = declarator(base, &name);
    if (name) error("unexpected name '%s' in a type", name);
    return t;
}

/* ================================================================ expression parser */

static char *name_syscall;
static Node *unary(void);

static Node *call_args(int *count) {
    Node *first = NULL, **link = &first;
    *count = 0;
    while (tok.kind != ')') {
        *link = assign();
        link = &(*link)->next;
        (*count)++;
        if (!accept(',')) break;
    }
    expect(')', "')' after the arguments");
    return first;
}

static Node *primary(void) {
    if (tok.kind == TK_NUM) {
        Type *t = tok.is_wide ? (tok.is_unsigned ? ty_ullong : ty_llong) : (tok.is_unsigned ? ty_uint : ty_int);
        Node *n = number(tok.num, t);
        next();
        return n;
    }
    if (tok.kind == TK_STR) {
        Node *n = node(ND_STR, NULL, NULL, NULL);
        n->val = add_string();
        n->type = array_of(ty_char, last_string_len + 1);
        return n;
    }
    if (tok.kind == TK_ID) {
        char *name = tok.name;
        next();
        if (name == name_syscall) {
            /* Built in: __syscall(number, a, b, c) -> int 0x80 */
            expect('(', "'(' after __syscall");
            Node *n = node(ND_SYSCALL, ty_llong, NULL, NULL);
            n->lhs = call_args(&n->arg_count);
            if (n->arg_count != 4) error("__syscall takes exactly 4 arguments");
            return n;
        }
        Sym *s = find(name);
        if (!s) error("'%s' is not declared", name);
        if (s->kind == SY_CONST) return number(s->value, ty_int);
        if (s->kind == SY_TYPEDEF) error("'%s' is a type, not a value", name);
        if (s->kind == SY_FUNC) {
            if (tok.kind != '(') error("'%s' is a function; function pointers are not supported", name);
            next();
            Node *n = node(ND_CALL, s->type, NULL, NULL);
            n->sym = s;
            n->lhs = call_args(&n->arg_count);
            if (n->arg_count < s->param_count || (!s->variadic && n->arg_count > s->param_count)) {
                error("%s() takes %d argument(s) but is given %d", name, s->param_count, n->arg_count);
            }
            return n;
        }
        Node *n = node(ND_VAR, s->type, NULL, NULL);
        n->sym = s;
        return n;
    }
    if (tok.kind == TK_EOF) error("the file ended in the middle of an expression");
    if (tok.kind < 256) error("expected a value, but found '%c'", tok.kind);
    error("expected a value here");
}

static Node *postfix_tail(Node *n) {
    for (;;) {
        if (accept('[')) {
            Node *index = expr();
            expect(']', "']'");
            n = deref(new_add(n, index));
        } else if (accept('.')) {
            char *name = tok.name;
            expect(TK_ID, "a member name after '.'");
            n = member_of(n, name);
        } else if (accept(P_ARROW)) {
            char *name = tok.name;
            expect(TK_ID, "a member name after '->'");
            n = member_of(deref(n), name);
        } else if (tok.kind == P_INC || tok.kind == P_DEC) {
            if (!is_lvalue(n) || !is_scalar(n->type)) error("this cannot be incremented or decremented");
            int step = n->type->kind == TY_PTR ? n->type->base->size : 1;
            Node *inc = node(ND_POSTINC, n->type, n, NULL);
            inc->val = tok.kind == P_INC ? step : -step;
            next();
            n = inc;
        } else {
            return n;
        }
    }
}

static Node *unary(void) {
    switch (tok.kind) {
    case '+':
        next();
        return unary();
    case '-': {
        next();
        Node *e = unary();
        return node(ND_NEG, promote(e->type), e, NULL);
    }
    case '~': {
        next();
        Node *e = unary();
        return node(ND_BITNOT, promote(e->type), e, NULL);
    }
    case '!':
        next();
        return node(ND_NOT, ty_int, unary(), NULL);
    case '*':
        next();
        return deref(unary());
    case '&': {
        next();
        Node *e = unary();
        if (!is_lvalue(e)) error("'&' needs a variable (something with an address)");
        return node(ND_ADDR, pointer_to(e->type), e, NULL);
    }
    case P_INC:
    case P_DEC: {
        int kind = tok.kind == P_INC ? ND_ADD : ND_SUB;
        next();
        return assign_op(kind, unary(), number(1, ty_int));
    }
    case K_SIZEOF: {
        next();
        Type *t;
        if (accept('(')) {
            t = is_type_start() ? type_name() : expr()->type;
            expect(')', "')' after sizeof");
        } else {
            t = unary()->type;
        }
        return number(t->size, ty_ullong);
    }
    case '(': {
        next();
        if (is_type_start()) { /* a cast */
            Type *t = type_name();
            expect(')', "')' after the type in a cast");
            return cast(unary(), t);
        }
        Node *e = expr();
        expect(')', "')'");
        return postfix_tail(e);
    }
    default:
        return postfix_tail(primary());
    }
}

/* Binary operators by precedence level, loosest first. */
static const struct {
    int token, kind, swap;
} binary_ops[][5] = {
    {{'|', ND_OR, 0}},
    {{'^', ND_XOR, 0}},
    {{'&', ND_AND, 0}},
    {{P_EQ, ND_EQ, 0}, {P_NE, ND_NE, 0}},
    {{'<', ND_LT, 0}, {'>', ND_LT, 1}, {P_LE, ND_LE, 0}, {P_GE, ND_LE, 1}},
    {{P_SHL, ND_SHL, 0}, {P_SHR, ND_SHR, 0}},
    {{'+', ND_ADD, 0}, {'-', ND_SUB, 0}},
    {{'*', ND_MUL, 0}, {'/', ND_DIV, 0}, {'%', ND_MOD, 0}},
};
#define BINARY_LEVELS 8

static Node *binary_level(int level) {
    if (level == BINARY_LEVELS) return unary();
    Node *n = binary_level(level + 1);
    for (;;) {
        int found = -1;
        for (int i = 0; i < 5 && binary_ops[level][i].token; i++) {
            if (binary_ops[level][i].token == tok.kind) found = i;
        }
        if (found < 0) return n;
        next();
        Node *r = binary_level(level + 1);
        n = binary_ops[level][found].swap ? binary(binary_ops[level][found].kind, r, n)
                                          : binary(binary_ops[level][found].kind, n, r);
    }
}

static Node *logical_and(void) {
    Node *n = binary_level(0);
    while (accept(P_ANDAND)) n = node(ND_LOGAND, ty_int, n, binary_level(0));
    return n;
}

static Node *logical_or(void) {
    Node *n = logical_and();
    while (accept(P_OROR)) n = node(ND_LOGOR, ty_int, n, logical_and());
    return n;
}

static Node *conditional(void) {
    Node *c = logical_or();
    if (!accept('?')) return c;
    Node *a = expr();
    expect(':', "':' in the ?: expression");
    Node *b = conditional();
    Type *t = is_integer(a->type) && is_integer(b->type) ? common_type(a->type, b->type) : decay(a->type);
    Node *n = node(ND_COND, t, c, a);
    n->third = b;
    return n;
}

static Node *assign(void) {
    Node *target = conditional();
    int kind;
    switch (tok.kind) {
    case '=': {
        next();
        if (!is_lvalue(target) || target->type->kind == TY_ARRAY) error("this cannot be assigned to");
        return node(ND_ASSIGN, target->type, target, assign());
    }
    case P_ADDEQ: kind = ND_ADD; break;
    case P_SUBEQ: kind = ND_SUB; break;
    case P_MULEQ: kind = ND_MUL; break;
    case P_DIVEQ: kind = ND_DIV; break;
    case P_MODEQ: kind = ND_MOD; break;
    case P_ANDEQ: kind = ND_AND; break;
    case P_OREQ: kind = ND_OR; break;
    case P_XOREQ: kind = ND_XOR; break;
    case P_SHLEQ: kind = ND_SHL; break;
    case P_SHREQ: kind = ND_SHR; break;
    default: return target;
    }
    next();
    return assign_op(kind, target, assign());
}

static Node *expr(void) {
    Node *n = assign();
    while (accept(',')) {
        Node *r = assign();
        n = node(ND_COMMA, r->type, n, r);
    }
    return n;
}

/* ================================================================ code generation */

static void gen(Node *n);

/* rax holds an address: replace it with the value stored there. */
static void gen_load(Type *t) {
    switch (t->size) {
    case 1: t->is_unsigned ? EMIT("\x48\x0F\xB6\x00") : EMIT("\x48\x0F\xBE\x00"); break;
    case 2: t->is_unsigned ? EMIT("\x48\x0F\xB7\x00") : EMIT("\x48\x0F\xBF\x00"); break;
    case 4: t->is_unsigned ? EMIT("\x8B\x00") : EMIT("\x48\x63\x00"); break;
    default: EMIT("\x48\x8B\x00");
    }
}

/* Store rax to the address in rcx. */
static void gen_store(Type *t) {
    switch (t->size) {
    case 1: EMIT("\x88\x01"); break;
    case 2: EMIT("\x66\x89\x01"); break;
    case 4: EMIT("\x89\x01"); break;
    default: EMIT("\x48\x89\x01");
    }
}

/* Narrow rax to the given type, keeping it sign- or zero-extended. */
static void gen_cast(Type *t) {
    switch (t->size) {
    case 1: t->is_unsigned ? EMIT("\x48\x0F\xB6\xC0") : EMIT("\x48\x0F\xBE\xC0"); break;
    case 2: t->is_unsigned ? EMIT("\x48\x0F\xB7\xC0") : EMIT("\x48\x0F\xBF\xC0"); break;
    case 4: t->is_unsigned ? EMIT("\x89\xC0") : EMIT("\x48\x63\xC0"); break;
    default: break;
    }
}

/* Leave the address of an lvalue in rax. */
static void gen_address(Node *n) {
    switch (n->kind) {
    case ND_VAR:
        if (n->sym->kind == SY_LOCAL) {
            EMIT("\x48\x8D\x85"); /* lea rax, [rbp + offset] */
            emit32((int)n->sym->value);
        } else {
            emit_address(n->sym->section, n->sym->value);
        }
        return;
    case ND_STR:
        emit_address(SEC_STR, n->val);
        return;
    case ND_DEREF:
        gen(n->lhs);
        return;
    case ND_MEMBER:
        gen_address(n->lhs);
        if (n->member->offset) {
            EMIT("\x48\x05"); /* add rax, offset */
            emit32(n->member->offset);
        }
        return;
    default:
        error("this expression has no address");
    }
}

/* rax = rax <op> rcx */
static void gen_operator(Node *op) {
    switch (op->kind) {
    case ND_ADD: EMIT("\x48\x01\xC8"); break;
    case ND_SUB: EMIT("\x48\x29\xC8"); break;
    case ND_MUL: EMIT("\x48\x0F\xAF\xC1"); break;
    case ND_DIV:
    case ND_MOD:
        if (op->is_unsigned) {
            EMIT("\x31\xD2\x48\xF7\xF1"); /* xor edx, edx; div rcx */
        } else {
            EMIT("\x48\x99\x48\xF7\xF9"); /* cqo; idiv rcx */
        }
        if (op->kind == ND_MOD) EMIT("\x48\x89\xD0"); /* mov rax, rdx */
        break;
    case ND_AND: EMIT("\x48\x21\xC8"); break;
    case ND_OR: EMIT("\x48\x09\xC8"); break;
    case ND_XOR: EMIT("\x48\x31\xC8"); break;
    case ND_SHL: EMIT("\x48\xD3\xE0"); break;
    case ND_SHR: op->is_unsigned ? EMIT("\x48\xD3\xE8") : EMIT("\x48\xD3\xF8"); break;
    case ND_EQ:
    case ND_NE:
    case ND_LT:
    case ND_LE: {
        EMIT("\x48\x39\xC8"); /* cmp rax, rcx */
        char set[3] = {0x0F, 0, (char)0xC0};
        set[1] = (char)(op->kind == ND_EQ ? 0x94
                        : op->kind == ND_NE ? 0x95
                        : op->kind == ND_LT ? (op->is_unsigned ? 0x92 : 0x9C)
                                            : (op->is_unsigned ? 0x96 : 0x9E));
        emit(set, 3);
        EMIT("\x48\x0F\xB6\xC0"); /* movzx rax, al */
        return;
    }
    }
    if (op->type->size == 4) gen_cast(op->type); /* wrap to 32 bits like real int arithmetic */
}

/* Evaluate call arguments into stack slots: argument i at [rsp + 8*i]. */
static void gen_args(Node *args, int count) {
    if (!count) return;
    EMIT("\x48\x81\xEC"); /* sub rsp, 8*count */
    emit32(8 * count);
    int i = 0;
    for (Node *a = args; a; a = a->next, i++) {
        gen(a);
        EMIT("\x48\x89\x84\x24"); /* mov [rsp + 8*i], rax */
        emit32(8 * i);
    }
}

static void gen(Node *n) {
    switch (n->kind) {
    case ND_NUM:
        if (n->val >= -0x80000000ll && n->val <= 0x7FFFFFFFll) {
            EMIT("\x48\xC7\xC0"); /* mov rax, imm32 */
            emit32((int)n->val);
        } else {
            EMIT("\x48\xB8"); /* movabs rax, imm64 */
            emit64(n->val);
        }
        return;
    case ND_STR:
        gen_address(n);
        return;
    case ND_VAR:
    case ND_DEREF:
    case ND_MEMBER:
        gen_address(n);
        /* Arrays and structs are used through their address. */
        if (is_scalar(n->type)) gen_load(n->type);
        return;
    case ND_ADDR:
        gen_address(n->lhs);
        return;
    case ND_ASSIGN:
        gen_address(n->lhs);
        EMIT("\x50"); /* push rax */
        gen(n->rhs);
        if (n->type->kind == TY_STRUCT) {
            EMIT("\x48\x89\xC6\x5F\x48\xC7\xC1"); /* mov rsi, rax; pop rdi; mov rcx, size */
            emit32(n->type->size);
            EMIT("\xF3\xA4"); /* rep movsb */
            return;
        }
        EMIT("\x59"); /* pop rcx */
        gen_store(n->type);
        gen_cast(n->type);
        return;
    case ND_ASSIGNOP: {
        Node *op = n->third;
        gen_address(n->lhs);
        EMIT("\x50"); /* push the address */
        gen_load(n->type);
        EMIT("\x50"); /* push the old value */
        gen(op->rhs);
        EMIT("\x48\x89\xC1\x58"); /* mov rcx, rax; pop rax */
        gen_operator(op);
        EMIT("\x59"); /* pop rcx (the address) */
        gen_store(n->type);
        gen_cast(n->type);
        return;
    }
    case ND_POSTINC:
        gen_address(n->lhs);
        EMIT("\x48\x89\xC1"); /* mov rcx, rax */
        gen_load(n->type);
        EMIT("\x50\x48\x05"); /* push rax (old value); add rax, step */
        emit32((int)n->val);
        gen_store(n->type);
        EMIT("\x58"); /* pop rax */
        return;
    case ND_LOGAND: {
        gen(n->lhs);
        test_rax();
        int f1 = jump_forward(0x84);
        gen(n->rhs);
        test_rax();
        int f2 = jump_forward(0x84);
        EMIT("\x48\xC7\xC0\x01\x00\x00\x00"); /* mov rax, 1 */
        int end = jump_forward(0);
        land_here(f1);
        land_here(f2);
        EMIT("\x31\xC0"); /* xor eax, eax */
        land_here(end);
        return;
    }
    case ND_LOGOR: {
        gen(n->lhs);
        test_rax();
        int t1 = jump_forward(0x85);
        gen(n->rhs);
        test_rax();
        int t2 = jump_forward(0x85);
        EMIT("\x31\xC0");
        int end = jump_forward(0);
        land_here(t1);
        land_here(t2);
        EMIT("\x48\xC7\xC0\x01\x00\x00\x00");
        land_here(end);
        return;
    }
    case ND_NOT:
        gen(n->lhs);
        EMIT("\x48\x85\xC0\x0F\x94\xC0\x48\x0F\xB6\xC0"); /* test rax,rax; sete al; movzx rax, al */
        return;
    case ND_BITNOT:
        gen(n->lhs);
        EMIT("\x48\xF7\xD0");
        if (n->type->size == 4) gen_cast(n->type);
        return;
    case ND_NEG:
        gen(n->lhs);
        EMIT("\x48\xF7\xD8");
        if (n->type->size == 4) gen_cast(n->type);
        return;
    case ND_COND: {
        gen(n->lhs);
        test_rax();
        int otherwise = jump_forward(0x84);
        gen(n->rhs);
        int end = jump_forward(0);
        land_here(otherwise);
        gen(n->third);
        land_here(end);
        return;
    }
    case ND_CALL:
        gen_args(n->lhs, n->arg_count);
        EMIT("\xE8"); /* call */
        list_add(&n->sym->calls, code.len);
        emit32(0);
        if (n->arg_count) {
            EMIT("\x48\x81\xC4"); /* add rsp, 8*count */
            emit32(8 * n->arg_count);
        }
        return;
    case ND_SYSCALL:
        gen_args(n->lhs, 4);
        /* mov rax,[rsp]; mov rdi,[rsp+8]; mov rsi,[rsp+16]; mov rdx,[rsp+24]; int 0x80; add rsp, 32 */
        EMIT("\x48\x8B\x04\x24\x48\x8B\x7C\x24\x08\x48\x8B\x74\x24\x10\x48\x8B\x54\x24\x18\xCD\x80");
        EMIT("\x48\x81\xC4\x20\x00\x00\x00");
        return;
    case ND_CAST:
        gen(n->lhs);
        gen_cast(n->type);
        return;
    case ND_COMMA:
        gen(n->lhs);
        gen(n->rhs);
        return;
    default: /* the two-operand arithmetic and comparison operators */
        gen(n->lhs);
        EMIT("\x50"); /* push rax */
        gen(n->rhs);
        EMIT("\x48\x89\xC1\x58"); /* mov rcx, rax; pop rax */
        gen_operator(n);
        return;
    }
}

/* ================================================================ statements */

/* What `break` and `continue` refer to at the moment. */
typedef struct Context {
    struct Context *outer;
    int is_switch;
    IntList breaks;      /* jumps to patch to the end */
    IntList continues;   /* jumps to patch to the loop's next iteration */
    int continue_target; /* or a known place to jump back to, if >= 0 */
    IntList case_values, case_places;
    int default_place;
} Context;

static Context *context;
static Type *return_type;
static IntList returns;
static int stack_used, stack_max;

static int alloc_local(int size) {
    stack_used = align_up(stack_used + (size ? size : 1), 8);
    if (stack_used > stack_max) stack_max = stack_used;
    return -stack_used;
}

static Node *var_node(Sym *s) {
    Node *n = node(ND_VAR, s->type, NULL, NULL);
    n->sym = s;
    return n;
}

static void define_global(Sym *s, Type *t);

/* Fill `size` bytes at [rbp + offset] with zeros. */
static void gen_zero(int offset, int size) {
    EMIT("\x48\x8D\xBD"); /* lea rdi, [rbp + offset] */
    emit32(offset);
    EMIT("\x31\xC0\x48\xC7\xC1"); /* xor eax, eax; mov rcx, size */
    emit32(size);
    EMIT("\xF3\xAA"); /* rep stosb */
}

static void local_array_init(Type *t, char *name) {
    if (t->base->kind == TY_CHAR && tok.kind == TK_STR) {
        Buf text = {0};
        while (tok.kind == TK_STR) {
            buf_put(&text, tok.str, tok.str_len);
            next();
        }
        buf_put(&text, "", 1);
        if (t->len < 0) *t = *array_of(t->base, text.len);
        if (text.len - 1 > t->len) error("the string is too long for the array");
        Sym *s = add_sym(name, SY_LOCAL, t, 1);
        s->value = alloc_local(t->size);
        gen_zero((int)s->value, t->size);
        for (int i = 0; i < text.len - 1; i++) {
            EMIT("\xC6\x85"); /* mov byte [rbp + offset], value */
            emit32((int)s->value + i);
            emit((char *)&text.p[i], 1);
        }
        return;
    }
    if (!is_scalar(t->base)) error("initializer lists for nested local arrays are not supported (use a global)");
    expect('{', "'{' to start the array initializer");
    Node *first = NULL, **link = &first;
    int count = 0;
    while (tok.kind != '}') {
        *link = assign();
        link = &(*link)->next;
        count++;
        if (!accept(',')) break;
    }
    expect('}', "'}' to end the array initializer");
    if (t->len < 0) *t = *array_of(t->base, count);
    if (count > t->len) error("too many values for the array");
    Sym *s = add_sym(name, SY_LOCAL, t, 1);
    s->value = alloc_local(t->size);
    if (count < t->len) gen_zero((int)s->value, t->size);
    int i = 0;
    for (Node *e = first; e; i++) {
        Node *following = e->next;
        e->next = NULL;
        Node *element = deref(new_add(var_node(s), number(i, ty_int)));
        gen(node(ND_ASSIGN, element->type, element, e));
        e = following;
    }
}

static void local_decl(void) {
    int is_typedef = 0, is_static = 0, is_extern = 0;
    Type *base = declspec(&is_typedef, &is_static, &is_extern);
    if (accept(';')) return;
    for (;;) {
        char *name;
        Type *t = declarator(base, &name);
        if (!name) error("expected a variable name");
        if (is_typedef) {
            add_sym(name, SY_TYPEDEF, t, 1);
        } else if (is_static) {
            /* A static local is a global only this scope can see. */
            define_global(add_sym(name, SY_GLOBAL, t, 1), t);
        } else if (t->kind == TY_VOID) {
            error("variable '%s' cannot have type void", name);
        } else if (accept('=')) {
            if (t->kind == TY_ARRAY) {
                local_array_init(t, name);
            } else if (t->kind == TY_STRUCT && tok.kind == '{') {
                error("initializer lists for local structs are not supported (assign the members)");
            } else {
                Sym *s = add_sym(name, SY_LOCAL, t, 1);
                s->value = alloc_local(t->size);
                gen(node(ND_ASSIGN, t, var_node(s), assign()));
            }
        } else {
            if (t->size == 0) error("variable '%s' has an incomplete type or unknown size", name);
            Sym *s = add_sym(name, SY_LOCAL, t, 1);
            s->value = alloc_local(t->size);
        }
        if (!accept(',')) break;
    }
    expect(';', "';' after the declaration");
}

static void statement(void);

static void block(void) {
    Sym *saved = locals;
    while (!accept('}')) {
        if (tok.kind == TK_EOF) error("a '{' is never closed");
        if (is_type_start()) {
            local_decl();
        } else {
            statement();
        }
    }
    locals = saved;
}

static void patch_all(IntList *list) {
    for (int i = 0; i < list->n; i++) land_here(list->v[i]);
}

/* Parse `(condition)`, emit it, and return a jump taken when it is false. */
static int condition_jump(void) {
    expect('(', "'('");
    gen(expr());
    expect(')', "')'");
    test_rax();
    return jump_forward(0x84);
}

static void statement(void) {
    switch (tok.kind) {
    case '{':
        next();
        block();
        return;
    case ';':
        next();
        return;
    case K_IF: {
        next();
        int otherwise = condition_jump();
        statement();
        if (accept(K_ELSE)) {
            int end = jump_forward(0);
            land_here(otherwise);
            statement();
            land_here(end);
        } else {
            land_here(otherwise);
        }
        return;
    }
    case K_WHILE: {
        next();
        Context ctx = {0};
        ctx.outer = context;
        ctx.continue_target = code.len;
        context = &ctx;
        int done = condition_jump();
        statement();
        jump_back(0, ctx.continue_target);
        land_here(done);
        patch_all(&ctx.breaks);
        context = ctx.outer;
        return;
    }
    case K_DO: {
        next();
        Context ctx = {0};
        ctx.outer = context;
        ctx.continue_target = -1;
        context = &ctx;
        int top = code.len;
        statement();
        patch_all(&ctx.continues);
        expect(K_WHILE, "'while' after the do block");
        expect('(', "'('");
        gen(expr());
        expect(')', "')'");
        expect(';', "';'");
        test_rax();
        jump_back(0x85, top);
        patch_all(&ctx.breaks);
        context = ctx.outer;
        return;
    }
    case K_FOR: {
        next();
        Sym *saved = locals;
        Context ctx = {0};
        ctx.outer = context;
        ctx.continue_target = -1;
        expect('(', "'(' after for");
        if (is_type_start()) {
            local_decl();
        } else {
            if (tok.kind != ';') gen(expr());
            expect(';', "';' in the for loop");
        }
        int top = code.len, done = -1;
        if (tok.kind != ';') {
            gen(expr());
            test_rax();
            done = jump_forward(0x84);
        }
        expect(';', "';' in the for loop");
        Node *step = tok.kind != ')' ? expr() : NULL;
        expect(')', "')' after the for loop header");
        context = &ctx;
        statement();
        patch_all(&ctx.continues);
        if (step) gen(step);
        jump_back(0, top);
        if (done >= 0) land_here(done);
        patch_all(&ctx.breaks);
        context = ctx.outer;
        locals = saved;
        return;
    }
    case K_SWITCH: {
        next();
        expect('(', "'(' after switch");
        gen(expr());
        expect(')', "')'");
        /* Keep the value in a hidden local; the comparisons come after the body. */
        int slot = alloc_local(8);
        EMIT("\x48\x89\x85"); /* mov [rbp + slot], rax */
        emit32(slot);
        int to_dispatch = jump_forward(0);
        Context ctx = {0};
        ctx.outer = context;
        ctx.is_switch = 1;
        ctx.default_place = -1;
        context = &ctx;
        statement();
        context = ctx.outer;
        int end = jump_forward(0);
        land_here(to_dispatch);
        EMIT("\x48\x8B\x85"); /* mov rax, [rbp + slot] */
        emit32(slot);
        for (int i = 0; i < ctx.case_values.n; i++) {
            EMIT("\x48\x3D"); /* cmp rax, imm32 */
            emit32(ctx.case_values.v[i]);
            jump_back(0x84, ctx.case_places.v[i]);
        }
        if (ctx.default_place >= 0) jump_back(0, ctx.default_place);
        land_here(end);
        patch_all(&ctx.breaks);
        return;
    }
    case K_CASE:
    case K_DEFAULT: {
        Context *sw = context;
        while (sw && !sw->is_switch) sw = sw->outer;
        if (!sw) error("'case' and 'default' only make sense inside a switch");
        if (accept(K_DEFAULT)) {
            sw->default_place = code.len;
        } else {
            next();
            i64 value = const_eval(conditional());
            if (value < -0x80000000ll || value > 0x7FFFFFFFll) error("case value does not fit in an int");
            list_add(&sw->case_values, (int)value);
            list_add(&sw->case_places, code.len);
        }
        expect(':', "':' after the case label");
        return;
    }
    case K_BREAK:
        next();
        if (!context) error("'break' outside a loop or switch");
        list_add(&context->breaks, jump_forward(0));
        expect(';', "';' after break");
        return;
    case K_CONTINUE: {
        next();
        Context *loop = context;
        while (loop && loop->is_switch) loop = loop->outer;
        if (!loop) error("'continue' outside a loop");
        if (loop->continue_target >= 0) {
            jump_back(0, loop->continue_target);
        } else {
            list_add(&loop->continues, jump_forward(0));
        }
        expect(';', "';' after continue");
        return;
    }
    case K_RETURN:
        next();
        if (tok.kind != ';') {
            Node *value = expr();
            if (return_type->kind == TY_VOID) {
                gen(value);
            } else {
                gen(cast(value, return_type));
            }
        }
        list_add(&returns, jump_forward(0));
        expect(';', "';' after return");
        return;
    default:
        gen(expr());
        expect(';', "';' at the end of the statement");
        return;
    }
}

/* ================================================================ globals and functions */

/* Write the initializer at the current token into `data` at `offset`. */
static void global_init(Type *t, int offset) {
    if (t->kind == TY_ARRAY) {
        int element = t->base->size;
        if (t->base->kind == TY_CHAR && tok.kind == TK_STR) {
            int i = 0;
            while (tok.kind == TK_STR) {
                if (t->len >= 0 && i + tok.str_len > t->len) error("the string is too long for the array");
                buf_reserve(&data, offset + i + tok.str_len + 1);
                memcpy(data.p + offset + i, tok.str, (size_t)tok.str_len);
                i += tok.str_len;
                next();
            }
            if (t->len < 0) *t = *array_of(t->base, i + 1);
            return;
        }
        expect('{', "'{' to start the array initializer");
        int count = 0;
        while (tok.kind != '}') {
            if (t->len >= 0 && count >= t->len) error("too many values for the array");
            buf_reserve(&data, offset + (count + 1) * element);
            global_init(t->base, offset + count * element);
            count++;
            if (!accept(',')) break;
        }
        expect('}', "'}' to end the array initializer");
        if (t->len < 0) *t = *array_of(t->base, count);
        return;
    }
    if (t->kind == TY_STRUCT) {
        expect('{', "'{' to start the struct initializer");
        for (Member *m = t->members; m && tok.kind != '}'; m = m->next) {
            global_init(m->type, offset + m->offset);
            if (!accept(',')) break;
        }
        expect('}', "'}' to end the struct initializer");
        return;
    }
    buf_reserve(&data, offset + t->size);
    if (tok.kind == TK_STR && t->kind == TY_PTR) {
        add_fixup(offset, 1, SEC_STR, add_string());
        return;
    }
    Node *e = assign();
    /* The address of another global is a constant too. */
    Node *target = e->kind == ND_ADDR ? e->lhs : e;
    if (target->kind == ND_VAR && target->sym->kind == SY_GLOBAL && (e->kind == ND_ADDR || e->type->kind == TY_ARRAY)) {
        add_fixup(offset, 1, target->sym->section, target->sym->value);
        return;
    }
    i64 value = const_eval(e);
    unsigned char bytes[8];
    store64(bytes, (u64)value);
    memcpy(data.p + offset, bytes, (size_t)t->size);
}

/* Give a global variable its storage, reading an initializer if present. */
static void define_global(Sym *s, Type *t) {
    if (t->kind == TY_VOID) error("variable '%s' cannot have type void", s->name);
    if (accept('=')) {
        int offset = align_up(data.len, t->align < 8 ? 8 : t->align);
        buf_reserve(&data, offset + t->size);
        data.len = offset; /* nothing else is appended to `data` while we fill it */
        global_init(t, offset);
        s->section = SEC_DATA;
        s->value = offset;
        buf_reserve(&data, offset + t->size);
        data.len = offset + t->size;
    } else {
        if (t->size == 0) error("variable '%s' has an incomplete type or unknown size", s->name);
        bss_size = align_up(bss_size, t->align < 8 ? 8 : t->align);
        s->section = SEC_BSS;
        s->value = bss_size;
        bss_size += t->size;
    }
}

static void function(Type *ret, char *name) {
    if (ret->kind == TY_STRUCT || ret->kind == TY_ARRAY) error("functions cannot return structs or arrays");
    next(); /* the '(' */
    char *param_names[32];
    Type *param_types[32];
    int count = 0, variadic = 0;
    while (tok.kind != ')') {
        if (accept(P_ELLIPSIS)) {
            variadic = 1;
            break;
        }
        Type *base = declspec(NULL, NULL, NULL);
        if (!base) error("expected a parameter type");
        if (base->kind == TY_VOID && tok.kind == ')') break; /* f(void) */
        if (count == 32) error("too many parameters (the limit is 32)");
        Type *t = decay(declarator(base, &param_names[count]));
        if (t->kind == TY_STRUCT) error("structs cannot be passed by value (pass a pointer)");
        param_types[count++] = t;
        if (!accept(',')) break;
    }
    expect(')', "')' after the parameters");

    Sym *f = find_global(name);
    if (f && f->kind != SY_FUNC) error("'%s' is already declared as something else", name);
    if (!f) {
        f = add_sym(name, SY_FUNC, ret, 0);
        f->code_offset = -1;
    }
    f->type = ret;
    f->param_count = count;
    f->variadic = variadic;
    if (tok.kind != '{') return; /* just a prototype */

    if (f->defined) error("function '%s' is defined twice", name);
    f->defined = 1;
    f->code_offset = code.len;
    next();

    in_function = 1;
    locals = NULL;
    return_type = ret;
    returns.n = 0;
    stack_used = stack_max = 0;
    for (int i = 0; i < count; i++) {
        if (!param_names[i]) error("parameter %d of %s() needs a name", i + 1, name);
        Sym *p = add_sym(param_names[i], SY_LOCAL, param_types[i], 1);
        p->value = 16 + 8 * i; /* above the saved rbp and the return address */
    }

    EMIT("\x55\x48\x89\xE5\x48\x81\xEC"); /* push rbp; mov rbp, rsp; sub rsp, <frame size> */
    int frame_size = code.len;
    emit32(0);
    block();
    EMIT("\x31\xC0"); /* falling off the end returns 0 */
    patch_all(&returns);
    EMIT("\xC9\xC3"); /* leave; ret */
    store32(code.p + frame_size, (unsigned)align_up(stack_max, 16));

    locals = NULL;
    in_function = 0;
}

static void global_decl(void) {
    int is_typedef = 0, is_static = 0, is_extern = 0;
    Type *base = declspec(&is_typedef, &is_static, &is_extern);
    if (!base) {
        if (tok.kind == TK_ID) error("'%s' is not a known type", tok.name);
        error("expected a declaration");
    }
    if (accept(';')) return; /* e.g. just `struct point { ... };` */
    for (;;) {
        char *name;
        Type *t = declarator(base, &name);
        if (!name) error("expected a name in the declaration");
        if (is_typedef) {
            add_sym(name, SY_TYPEDEF, t, 0);
        } else if (tok.kind == '(') {
            function(t, name);
            if (find_global(name)->defined && tok.kind != ',' && tok.kind != ';') return; /* had a body */
        } else {
            Sym *existing = find_global(name);
            if (existing && existing->kind != SY_GLOBAL) error("'%s' is already declared as something else", name);
            if (existing && tok.kind == '=') error("variable '%s' is defined twice", name);
            if (!existing) define_global(add_sym(name, SY_GLOBAL, t, 0), t);
        }
        if (!accept(',')) break;
    }
    expect(';', "';' after the declaration");
}

/* ================================================================ writing the executable */

static void write_exe(const char *out_name, int entry_stub_call) {
    /* Resolve calls now that every function's address is known. */
    for (Sym *s = globals; s; s = s->next) {
        if (s->kind != SY_FUNC || s->calls.n == 0) continue;
        if (!s->defined) {
            cur_file = out_name;
            cur_line = 0;
            if (strcmp(s->name, "main") == 0) error("the program has no main() function");
            error("function '%s' is called but never defined", s->name);
        }
        for (int i = 0; i < s->calls.n; i++) {
            int at = s->calls.v[i];
            store32(code.p + at, (unsigned)(s->code_offset - (at + 4)));
        }
    }
    (void)entry_stub_call;

    /* Layout: [headers] [.text] [.data = globals, strings, then zeroed bss] */
    int text_size = code.len;
    int section_base[3];
    section_base[SEC_DATA] = 0;
    section_base[SEC_STR] = align_up(data.len, 16);
    int init_size = section_base[SEC_STR] + strs.len;
    section_base[SEC_BSS] = align_up(init_size, 16);
    int data_virtual = section_base[SEC_BSS] + bss_size;
    if (data_virtual == 0) data_virtual = 16;

    int text_rva = 0x1000;
    int data_rva = text_rva + align_up(text_size, 0x1000);
    int image_size = data_rva + align_up(data_virtual, 0x1000);
    if (image_size > 60 * 1024 * 1024) error("the program is too large (globals are limited to about 60 MiB)");

    for (int i = 0; i < fixup_count; i++) {
        Fixup *f = &fixups[i];
        u64 address = IMAGE_BASE + (u64)data_rva + (u64)section_base[f->section] + (u64)f->addend;
        store64((f->in_data ? data.p : code.p) + f->pos, address);
    }

    int text_raw = align_up(text_size, 0x200);
    int data_raw = align_up(init_size, 0x200);
    int file_size = 0x200 + text_raw + data_raw;
    unsigned char *out = alloc((size_t)file_size);

    /* DOS header: just the magic and the offset of the PE header. */
    out[0] = 'M';
    out[1] = 'Z';
    store32(out + 0x3C, 0x40);
    unsigned char *pe = out + 0x40;
    memcpy(pe, "PE\0\0", 4);
    /* COFF header */
    pe[4] = 0x64; /* machine: x86-64 */
    pe[5] = 0x86;
    pe[6] = 2;     /* sections */
    pe[20] = 0xF0; /* size of the optional header */
    pe[22] = 0x23; /* executable, no relocations, large addresses */
    /* Optional header (PE32+) */
    unsigned char *opt = pe + 24;
    opt[0] = 0x0B;
    opt[1] = 0x02;
    store32(opt + 4, (unsigned)text_raw);
    store32(opt + 8, (unsigned)data_raw);
    store32(opt + 16, (unsigned)text_rva); /* entry point: the stub at the start of .text */
    store32(opt + 20, (unsigned)text_rva);
    store64(opt + 24, IMAGE_BASE);
    store32(opt + 32, 0x1000); /* section alignment */
    store32(opt + 36, 0x200);  /* file alignment */
    opt[40] = 6;               /* OS version */
    opt[48] = 6;               /* subsystem version */
    store32(opt + 56, (unsigned)image_size);
    store32(opt + 60, 0x200); /* size of headers */
    opt[68] = 1;              /* subsystem: native */
    store64(opt + 72, 0x100000);
    store64(opt + 80, 0x1000);
    store64(opt + 88, 0x100000);
    store64(opt + 96, 0x1000);
    store32(opt + 108, 16); /* data directories, all empty */
    /* Section table */
    unsigned char *sec = opt + 0xF0;
    memcpy(sec, ".text", 5);
    store32(sec + 8, (unsigned)text_size);
    store32(sec + 12, (unsigned)text_rva);
    store32(sec + 16, (unsigned)text_raw);
    store32(sec + 20, 0x200);
    store32(sec + 36, 0x60000020); /* code, execute, read */
    sec += 40;
    memcpy(sec, ".data", 5);
    store32(sec + 8, (unsigned)data_virtual);
    store32(sec + 12, (unsigned)data_rva);
    store32(sec + 16, (unsigned)data_raw);
    store32(sec + 20, (unsigned)(0x200 + text_raw));
    store32(sec + 36, 0xC0000040); /* initialised data, read, write */

    memcpy(out + 0x200, code.p, (size_t)text_size);
    unsigned char *d = out + 0x200 + text_raw;
    if (data.len) memcpy(d, data.p, (size_t)data.len);
    if (strs.len) memcpy(d + section_base[SEC_STR], strs.p, (size_t)strs.len);

    if (file_write(out_name, out, (size_t)file_size) != 0) {
        cur_file = out_name;
        cur_line = 0;
        error("could not write the output file (is it larger than 1 MiB?)");
    }
    printf("%s: %d bytes (%d code, %d data)\n", out_name, file_size, text_size, init_size + bss_size);
}

/* ================================================================ main */

int main(int argc, char **argv) {
    const char *source = NULL, *output = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            output = argv[++i];
        } else {
            source = argv[i];
        }
    }
    if (!source) {
        printf("usage: c-comp <file.c> [-o output.exe]\n");
        return 1;
    }

    char out_name[64];
    if (output) {
        if (strlen(output) > 60) error("the output name is too long");
        strcpy(out_name, output);
    } else {
        size_t n = strlen(source);
        if (n > 56) error("the file name is too long");
        strcpy(out_name, source);
        if (n > 2 && strcmp(source + n - 2, ".c") == 0) n -= 2;
        strcpy(out_name + n, ".exe");
    }

    ty_void = new_type(TY_VOID, 0, 0);
    ty_char = new_type(TY_CHAR, 1, 0);
    ty_uchar = new_type(TY_CHAR, 1, 1);
    ty_short = new_type(TY_SHORT, 2, 0);
    ty_ushort = new_type(TY_SHORT, 2, 1);
    ty_int = new_type(TY_INT, 4, 0);
    ty_uint = new_type(TY_INT, 4, 1);
    ty_llong = new_type(TY_LLONG, 8, 0);
    ty_ullong = new_type(TY_LLONG, 8, 1);
    name_syscall = intern("__syscall", 9);

    /* Entry stub. The kernel starts the program here with the command line
       in rcx: push it as the argument and call __start (in clib.c). */
    EMIT("\x51\xE8");
    int stub_call = code.len;
    emit32(0);
    EMIT("\x0F\x0B"); /* ud2: __start never returns */

    /* The user's file is read after the runtime library. */
    cur_file = source;
    push_input(load_file(source), source, 1, NULL);
    push_input(load_file("clib.c"), "clib.c", 1, NULL);
    next();
    while (tok.kind != TK_EOF) global_decl();

    Sym *start = find_global(intern("__start", 7));
    if (!start || !start->defined) error("clib.c does not define __start");
    list_add(&start->calls, stub_call);

    write_exe(out_name, stub_call);
    return 0;
}
