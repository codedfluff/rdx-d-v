/* rx.exe - interpreter for RX, the RDX scripting language.

   Usage:  rx file.rx [arguments]     run a script
           rx                         interactive prompt

   The language in one screen:

       # comment
       let name = "world"                 # variables: integers, strings, lists, nil
       let nums = [1, 2, 3]
       fn greet(who) { return "hello " + who }
       if len(nums) > 2 { print(greet(name)) } else { print("short") }
       while nums[0] < 10 { nums[0] += 1 }
       for i in 0..5 { write(i, " ") }    # 0 1 2 3 4
       for n in nums { print(n) }

   Built-in functions are listed in the `builtins` table below. */
#include "rdx.h"

/* ================================================================ values */

enum { V_NIL, V_INT, V_STR, V_LIST, V_FN, V_BUILTIN };

typedef struct Node Node;
typedef struct List List;

typedef struct {
    int len;
    char s[1]; /* len bytes, then a NUL */
} Str;

typedef struct {
    int type;
    union {
        i64 i;
        Str *s;
        List *l;
        Node *fn;
    } u;
} Value;

struct List {
    int len, cap;
    Value *items;
};

static Value nil(void) {
    Value v = {V_NIL, {0}};
    return v;
}

static Value num(i64 i) {
    Value v = {V_INT, {0}};
    v.u.i = i;
    return v;
}

static Str *new_str(const char *text, int len) {
    Str *s = malloc(sizeof(Str) + (size_t)len);
    s->len = len;
    if (text) memcpy(s->s, text, (size_t)len);
    s->s[len] = 0;
    return s;
}

static Value str_value(Str *s) {
    Value v = {V_STR, {0}};
    v.u.s = s;
    return v;
}

static Value text(const char *s) { return str_value(new_str(s, (int)strlen(s))); }

static Value new_list(void) {
    Value v = {V_LIST, {0}};
    v.u.l = malloc(sizeof(List));
    v.u.l->len = v.u.l->cap = 0;
    v.u.l->items = NULL;
    return v;
}

static void list_push(List *l, Value v) {
    if (l->len == l->cap) {
        l->cap = l->cap ? l->cap * 2 : 8;
        l->items = realloc(l->items, sizeof(Value) * (size_t)l->cap);
    }
    l->items[l->len++] = v;
}

static int truthy(Value v) { return !(v.type == V_NIL || (v.type == V_INT && v.u.i == 0)); }

/* ================================================================ errors */

static int failed;
static const char *script_name = "rx";

static Value fail(int line, const char *fmt, ...) {
    if (!failed) {
        failed = 1;
        screen_color(0xFF5C57);
        printf("%s:%d: ", script_name, line);
        va_list ap;
        va_start(ap, fmt);
        vprintf(fmt, ap);
        va_end(ap);
        putchar('\n');
        screen_color(COLOR_DEFAULT);
    }
    return nil();
}

/* ================================================================ lexer */

enum {
    TK_EOF = 256, TK_NUM, TK_STR, TK_IDENT,
    TK_EQ, TK_NE, TK_LE, TK_GE, TK_AND, TK_OR, TK_DOTDOT, TK_PLUSEQ, TK_MINUSEQ,
    TK_LET, TK_FN, TK_IF, TK_ELSE, TK_WHILE, TK_FOR, TK_IN, TK_RETURN, TK_BREAK, TK_CONTINUE,
    TK_TRUE, TK_FALSE, TK_NIL
};

static const struct {
    const char *word;
    int type;
} keywords[] = {
    {"let", TK_LET}, {"fn", TK_FN}, {"if", TK_IF}, {"else", TK_ELSE}, {"while", TK_WHILE},
    {"for", TK_FOR}, {"in", TK_IN}, {"return", TK_RETURN}, {"break", TK_BREAK},
    {"continue", TK_CONTINUE}, {"true", TK_TRUE}, {"false", TK_FALSE}, {"nil", TK_NIL},
};

typedef struct {
    int type;
    i64 num;
    char *name; /* interned identifier */
    Str *str;
    int line;
    int newline_before;
} Token;

static const char *src;
static int pos, line;
static Token tok;

/* Identifiers are interned so they can be compared by pointer. */
static char **names;
static int name_count, name_cap;

static char *intern(const char *s, int len) {
    for (int i = 0; i < name_count; i++) {
        if ((int)strlen(names[i]) == len && memcmp(names[i], s, (size_t)len) == 0) return names[i];
    }
    if (name_count == name_cap) {
        name_cap = name_cap ? name_cap * 2 : 64;
        names = realloc(names, sizeof(char *) * (size_t)name_cap);
    }
    char *copy = malloc((size_t)len + 1);
    memcpy(copy, s, (size_t)len);
    copy[len] = 0;
    return names[name_count++] = copy;
}

static int is_alpha(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
static int is_digit(char c) { return c >= '0' && c <= '9'; }

static void next(void) {
    tok.newline_before = 0;
    for (;;) {
        char c = src[pos];
        if (c == '\n') {
            line++;
            pos++;
            tok.newline_before = 1;
        } else if (c == ';') {
            pos++;
            tok.newline_before = 1;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            pos++;
        } else if (c == '#' || (c == '/' && src[pos + 1] == '/')) {
            while (src[pos] && src[pos] != '\n') pos++;
        } else {
            break;
        }
    }
    tok.line = line;
    char c = src[pos];
    if (!c) {
        tok.type = TK_EOF;
        return;
    }
    if (is_digit(c)) {
        i64 n = 0;
        if (c == '0' && (src[pos + 1] == 'x' || src[pos + 1] == 'X')) {
            pos += 2;
            for (;; pos++) {
                char h = src[pos];
                if (is_digit(h)) n = n * 16 + (h - '0');
                else if (h >= 'a' && h <= 'f') n = n * 16 + (h - 'a' + 10);
                else if (h >= 'A' && h <= 'F') n = n * 16 + (h - 'A' + 10);
                else break;
            }
        } else {
            while (is_digit(src[pos])) n = n * 10 + (src[pos++] - '0');
        }
        tok.type = TK_NUM;
        tok.num = n;
        return;
    }
    if (is_alpha(c)) {
        int start = pos;
        while (is_alpha(src[pos]) || is_digit(src[pos])) pos++;
        tok.name = intern(src + start, pos - start);
        tok.type = TK_IDENT;
        for (size_t i = 0; i < sizeof(keywords) / sizeof(keywords[0]); i++) {
            if (strcmp(tok.name, keywords[i].word) == 0) tok.type = keywords[i].type;
        }
        return;
    }
    if (c == '"') {
        pos++;
        int cap = 32, len = 0;
        char *buf = malloc((size_t)cap);
        while (src[pos] && src[pos] != '"') {
            char ch = src[pos++];
            if (ch == '\n') line++;
            if (ch == '\\' && src[pos]) {
                char e = src[pos++];
                ch = e == 'n' ? '\n' : e == 't' ? '\t' : e == '0' ? '\0' : e;
            }
            if (len == cap) buf = realloc(buf, (size_t)(cap *= 2));
            buf[len++] = ch;
        }
        if (!src[pos]) {
            fail(tok.line, "string is missing its closing quote");
        } else {
            pos++;
        }
        tok.type = TK_STR;
        tok.str = new_str(buf, len);
        free(buf);
        return;
    }
    char d = src[pos + 1];
    int two = 0;
    if (c == '=' && d == '=') two = TK_EQ;
    else if (c == '!' && d == '=') two = TK_NE;
    else if (c == '<' && d == '=') two = TK_LE;
    else if (c == '>' && d == '=') two = TK_GE;
    else if (c == '&' && d == '&') two = TK_AND;
    else if (c == '|' && d == '|') two = TK_OR;
    else if (c == '.' && d == '.') two = TK_DOTDOT;
    else if (c == '+' && d == '=') two = TK_PLUSEQ;
    else if (c == '-' && d == '=') two = TK_MINUSEQ;
    if (two) {
        tok.type = two;
        pos += 2;
    } else {
        tok.type = (unsigned char)c;
        pos++;
    }
}

/* ================================================================ parser */

enum {
    N_LIT, N_VAR, N_LIST, N_INDEX, N_CALL, N_UNARY, N_BINARY, N_AND, N_OR,
    N_LET, N_ASSIGN, N_EXPR, N_IF, N_WHILE, N_FOR, N_FN, N_RETURN, N_BREAK, N_CONTINUE
};

struct Node {
    int kind, line, op, count;
    Node *a, *b, *c;
    Node *next; /* next statement, argument, or list element */
    char *name;
    Value val;
};

static Node *node(int kind, int at_line) {
    Node *n = malloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->kind = kind;
    n->line = at_line;
    return n;
}

/* Report a syntax error and jump the lexer to the end of the input, so
   every parsing loop winds down on its own. */
static void syntax_error(const char *what) {
    if (tok.type == TK_EOF) {
        fail(tok.line, "%s, but the code ended", what);
    } else if (tok.type == TK_IDENT) {
        fail(tok.line, "%s, but found '%s'", what, tok.name);
    } else if (tok.type < 256) {
        fail(tok.line, "%s, but found '%c'", what, tok.type);
    } else {
        fail(tok.line, "%s", what);
    }
    while (src[pos]) pos++;
    tok.type = TK_EOF;
}

static void expect(int type, const char *what) {
    if (tok.type == type) {
        next();
    } else {
        syntax_error(what);
    }
}

static Node *expression(void);
static Node *block(void);

/* Comma-separated expressions up to `closer`, as a linked list. */
static Node *expression_list(int closer, int *count) {
    Node *first = NULL, **link = &first;
    *count = 0;
    while (tok.type != closer && tok.type != TK_EOF) {
        *link = expression();
        link = &(*link)->next;
        (*count)++;
        if (tok.type != ',') break;
        next();
    }
    expect(closer, closer == ')' ? "expected ')'" : "expected ']'");
    return first;
}

static Node *primary(void) {
    Node *n = node(N_LIT, tok.line);
    switch (tok.type) {
    case TK_NUM:
        n->val = num(tok.num);
        next();
        break;
    case TK_STR:
        n->val = str_value(tok.str);
        next();
        break;
    case TK_TRUE:
        n->val = num(1);
        next();
        break;
    case TK_FALSE:
        n->val = num(0);
        next();
        break;
    case TK_NIL:
        next();
        break;
    case TK_IDENT:
        n->kind = N_VAR;
        n->name = tok.name;
        next();
        break;
    case '(':
        next();
        n = expression();
        expect(')', "expected ')'");
        break;
    case '[':
        next();
        n->kind = N_LIST;
        n->a = expression_list(']', &n->count);
        break;
    default:
        syntax_error("expected a value");
    }
    return n;
}

static Node *postfix(void) {
    Node *n = primary();
    /* A '(' or '[' on a new line starts a new statement, not a call/index. */
    while ((tok.type == '(' || tok.type == '[') && !tok.newline_before) {
        Node *outer = node(tok.type == '(' ? N_CALL : N_INDEX, tok.line);
        outer->a = n;
        if (tok.type == '(') {
            next();
            outer->b = expression_list(')', &outer->count);
        } else {
            next();
            outer->b = expression();
            expect(']', "expected ']'");
        }
        n = outer;
    }
    return n;
}

static Node *unary(void) {
    if (tok.type == '-' || tok.type == '!') {
        Node *n = node(N_UNARY, tok.line);
        n->op = tok.type;
        next();
        n->a = unary();
        return n;
    }
    return postfix();
}

static Node *binary_node(int kind, int op, int at_line, Node *a, Node *b) {
    Node *n = node(kind, at_line);
    n->op = op;
    n->a = a;
    n->b = b;
    return n;
}

static Node *multiplicative(void) {
    Node *n = unary();
    while (tok.type == '*' || tok.type == '/' || tok.type == '%') {
        int op = tok.type, at = tok.line;
        next();
        n = binary_node(N_BINARY, op, at, n, unary());
    }
    return n;
}

static Node *additive(void) {
    Node *n = multiplicative();
    while (tok.type == '+' || tok.type == '-') {
        int op = tok.type, at = tok.line;
        next();
        n = binary_node(N_BINARY, op, at, n, multiplicative());
    }
    return n;
}

static Node *comparison(void) {
    Node *n = additive();
    while (tok.type == '<' || tok.type == '>' || tok.type == TK_LE || tok.type == TK_GE) {
        int op = tok.type, at = tok.line;
        next();
        n = binary_node(N_BINARY, op, at, n, additive());
    }
    return n;
}

static Node *equality(void) {
    Node *n = comparison();
    while (tok.type == TK_EQ || tok.type == TK_NE) {
        int op = tok.type, at = tok.line;
        next();
        n = binary_node(N_BINARY, op, at, n, comparison());
    }
    return n;
}

static Node *logical_and(void) {
    Node *n = equality();
    while (tok.type == TK_AND) {
        int at = tok.line;
        next();
        n = binary_node(N_AND, 0, at, n, equality());
    }
    return n;
}

static Node *expression(void) {
    Node *n = logical_and();
    while (tok.type == TK_OR) {
        int at = tok.line;
        next();
        n = binary_node(N_OR, 0, at, n, logical_and());
    }
    return n;
}

static Node *statement(void) {
    int at = tok.line;
    Node *n;
    switch (tok.type) {
    case TK_LET:
        next();
        n = node(N_LET, at);
        n->name = tok.name;
        expect(TK_IDENT, "expected a variable name after 'let'");
        expect('=', "expected '=' after the variable name");
        n->a = expression();
        return n;
    case TK_FN: {
        next();
        n = node(N_FN, at);
        n->name = tok.name;
        expect(TK_IDENT, "expected a function name after 'fn'");
        expect('(', "expected '(' after the function name");
        Node **link = &n->a;
        while (tok.type == TK_IDENT) {
            Node *param = node(N_VAR, tok.line);
            param->name = tok.name;
            *link = param;
            link = &param->next;
            n->count++;
            next();
            if (tok.type != ',') break;
            next();
        }
        expect(')', "expected ')' after the parameters");
        n->b = block();
        return n;
    }
    case TK_IF:
        next();
        n = node(N_IF, at);
        n->a = expression();
        n->b = block();
        if (tok.type == TK_ELSE) {
            next();
            n->c = tok.type == TK_IF ? statement() : block();
        }
        return n;
    case TK_WHILE:
        next();
        n = node(N_WHILE, at);
        n->a = expression();
        n->b = block();
        return n;
    case TK_FOR:
        next();
        n = node(N_FOR, at);
        n->name = tok.name;
        expect(TK_IDENT, "expected a variable name after 'for'");
        expect(TK_IN, "expected 'in' after the loop variable");
        n->a = expression();
        if (tok.type == TK_DOTDOT) {
            next();
            n->b = expression();
        }
        n->c = block();
        return n;
    case TK_RETURN:
        next();
        n = node(N_RETURN, at);
        if (!tok.newline_before && tok.type != '}' && tok.type != TK_EOF) n->a = expression();
        return n;
    case TK_BREAK:
        next();
        return node(N_BREAK, at);
    case TK_CONTINUE:
        next();
        return node(N_CONTINUE, at);
    default:
        break;
    }

    Node *target = expression();
    if (tok.type == '=' || tok.type == TK_PLUSEQ || tok.type == TK_MINUSEQ) {
        int op = tok.type;
        if (target->kind != N_VAR && target->kind != N_INDEX) {
            syntax_error("only a variable or list element can be assigned to");
            return target;
        }
        next();
        Node *value = expression();
        if (op != '=') value = binary_node(N_BINARY, op == TK_PLUSEQ ? '+' : '-', at, target, value);
        return binary_node(N_ASSIGN, 0, at, target, value);
    }
    n = node(N_EXPR, at);
    n->a = target;
    return n;
}

/* `{ statements }` as a linked list (NULL when empty). */
static Node *block(void) {
    Node *first = NULL, **link = &first;
    expect('{', "expected '{'");
    while (tok.type != '}' && tok.type != TK_EOF) {
        *link = statement();
        link = &(*link)->next;
    }
    expect('}', "expected '}'");
    return first;
}

static Node *parse(const char *source) {
    src = source;
    pos = 0;
    line = 1;
    next();
    Node *first = NULL, **link = &first;
    while (tok.type != TK_EOF) {
        *link = statement();
        link = &(*link)->next;
    }
    return first;
}

/* ================================================================ text conversion */

typedef struct {
    char *p;
    int len, cap;
} Buf;

static void buf_add(Buf *b, const char *s, int n) {
    if (b->len + n + 1 > b->cap) {
        b->cap = (b->len + n + 1) * 2;
        b->p = realloc(b->p, (size_t)b->cap);
    }
    memcpy(b->p + b->len, s, (size_t)n);
    b->len += n;
}

static void buf_value(Buf *b, Value v, int quote, int depth) {
    switch (v.type) {
    case V_NIL:
        buf_add(b, "nil", 3);
        break;
    case V_INT: {
        char digits[24];
        int n = 0;
        u64 u = v.u.i < 0 ? 0 - (u64)v.u.i : (u64)v.u.i;
        do {
            digits[n++] = (char)('0' + u % 10);
            u /= 10;
        } while (u);
        if (v.u.i < 0) digits[n++] = '-';
        while (n) buf_add(b, &digits[--n], 1);
        break;
    }
    case V_STR:
        if (quote) buf_add(b, "\"", 1);
        buf_add(b, v.u.s->s, v.u.s->len);
        if (quote) buf_add(b, "\"", 1);
        break;
    case V_LIST:
        if (depth > 8) {
            buf_add(b, "[...]", 5);
            break;
        }
        buf_add(b, "[", 1);
        for (int i = 0; i < v.u.l->len; i++) {
            if (i) buf_add(b, ", ", 2);
            buf_value(b, v.u.l->items[i], 1, depth + 1);
        }
        buf_add(b, "]", 1);
        break;
    default:
        buf_add(b, "<function>", 10);
    }
}

static Str *to_str(Value v, int quote) {
    if (v.type == V_STR && !quote) return v.u.s;
    Buf b = {0};
    buf_value(&b, v, quote, 0);
    Str *s = new_str(b.p, b.len);
    free(b.p);
    return s;
}

static void print_value(Value v, int quote) {
    Str *s = to_str(v, quote);
    for (int i = 0; i < s->len; i++) putchar(s->s[i]);
    if (s != v.u.s || v.type != V_STR) free(s);
}

/* ================================================================ variables */

typedef struct {
    char *name;
    Value v;
} Var;

typedef struct {
    Var *vars;
    int count, cap;
} Scope;

static Scope globals;
static Scope *locals; /* the running function's variables; NULL at top level */
static int call_depth;

static Value *lookup(Scope *s, char *name) {
    for (int i = s->count - 1; i >= 0; i--) {
        if (s->vars[i].name == name) return &s->vars[i].v;
    }
    return NULL;
}

static void define(Scope *s, char *name, Value v) {
    Value *existing = lookup(s, name);
    if (existing) {
        *existing = v;
        return;
    }
    if (s->count == s->cap) {
        s->cap = s->cap ? s->cap * 2 : 8;
        s->vars = realloc(s->vars, sizeof(Var) * (size_t)s->cap);
    }
    s->vars[s->count].name = name;
    s->vars[s->count++].v = v;
}

static Value *find(char *name) {
    Value *v = locals ? lookup(locals, name) : NULL;
    return v ? v : lookup(&globals, name);
}

static Scope *current_scope(void) { return locals ? locals : &globals; }

/* ================================================================ built-in functions */

enum {
    B_PRINT, B_WRITE, B_INPUT, B_INT, B_STR, B_LEN, B_PUSH, B_POP, B_CHR, B_ORD, B_RAND, B_TIME,
    B_SLEEP, B_READ, B_SAVE, B_CLEAR, B_GOTO, B_COLOR, B_KEY, B_POLL, B_EXIT, B_SUBSTR, B_RANGE,
    B_TYPE, B_SIZE, B_ABS, B_MIN, B_MAX, B_COUNT
};

static const struct {
    const char *name;
    int min_args, max_args;
    const char *help;
} builtins[B_COUNT] = {
    [B_PRINT] = {"print", 0, 16, "print values separated by spaces, then a newline"},
    [B_WRITE] = {"write", 0, 16, "print values with nothing added"},
    [B_INPUT] = {"input", 0, 1, "input(prompt) - read a line of text"},
    [B_INT] = {"int", 1, 1, "convert a string to a number (nil if it isn't one)"},
    [B_STR] = {"str", 1, 1, "convert anything to a string"},
    [B_LEN] = {"len", 1, 1, "length of a string or list"},
    [B_PUSH] = {"push", 2, 2, "push(list, value) - append to a list"},
    [B_POP] = {"pop", 1, 1, "remove and return the last item of a list"},
    [B_CHR] = {"chr", 1, 1, "character with the given code"},
    [B_ORD] = {"ord", 1, 1, "code of a string's first character"},
    [B_RAND] = {"rand", 1, 1, "rand(n) - random number from 0 to n-1"},
    [B_TIME] = {"time", 0, 0, "milliseconds since boot"},
    [B_SLEEP] = {"sleep", 1, 1, "sleep(ms)"},
    [B_READ] = {"read", 1, 1, "read(file) - file contents, or nil"},
    [B_SAVE] = {"save", 2, 2, "save(file, text) - write a file"},
    [B_CLEAR] = {"clear", 0, 0, "clear the screen"},
    [B_GOTO] = {"goto", 2, 2, "goto(col, row) - move the cursor"},
    [B_COLOR] = {"color", 0, 1, "color(0xRRGGBB) - text colour; color() resets"},
    [B_KEY] = {"key", 0, 0, "wait for a key, return its code"},
    [B_POLL] = {"poll", 0, 0, "a waiting key's code, or nil"},
    [B_EXIT] = {"exit", 0, 1, "stop the program"},
    [B_SUBSTR] = {"substr", 2, 3, "substr(s, start, length)"},
    [B_RANGE] = {"range", 1, 2, "range(n) or range(a, b) - list of numbers"},
    [B_TYPE] = {"type", 1, 1, "\"nil\", \"int\", \"str\", \"list\" or \"fn\""},
    [B_SIZE] = {"size", 0, 0, "screen size as [columns, rows]"},
    [B_ABS] = {"abs", 1, 1, "absolute value"},
    [B_MIN] = {"min", 2, 2, "smaller of two numbers"},
    [B_MAX] = {"max", 2, 2, "larger of two numbers"},
};

static u64 rng_state;

static u64 random_u64(void) {
    if (!rng_state) rng_state = uptime_ms() * 0x9E3779B97F4A7C15ull + 0x1234567;
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

static Value call_builtin(int id, Value *args, int argc, int at) {
    const char *name = builtins[id].name;
    if (argc < builtins[id].min_args || argc > builtins[id].max_args) {
        return fail(at, "%s() was given %d argument(s): %s", name, argc, builtins[id].help);
    }
#define WANT(index, kind, what)                                                         \
    if (args[index].type != kind) return fail(at, "%s(): argument %d must be %s", name, index + 1, what)

    switch (id) {
    case B_PRINT:
    case B_WRITE:
        for (int i = 0; i < argc; i++) {
            if (i && id == B_PRINT) putchar(' ');
            print_value(args[i], 0);
        }
        if (id == B_PRINT) putchar('\n');
        return nil();
    case B_INPUT: {
        char buf[256];
        if (argc) print_value(args[0], 0);
        int n = readline(buf, sizeof(buf));
        return str_value(new_str(buf, n));
    }
    case B_INT: {
        if (args[0].type == V_INT) return args[0];
        if (args[0].type != V_STR) return nil();
        const char *p = args[0].u.s->s;
        while (*p == ' ') p++;
        int negative = *p == '-';
        if (negative) p++;
        if (!is_digit(*p)) return nil();
        i64 n = 0;
        while (is_digit(*p)) n = n * 10 + (*p++ - '0');
        while (*p == ' ') p++;
        if (*p) return nil();
        return num(negative ? -n : n);
    }
    case B_STR:
        return str_value(to_str(args[0], 0));
    case B_LEN:
        if (args[0].type == V_STR) return num(args[0].u.s->len);
        if (args[0].type == V_LIST) return num(args[0].u.l->len);
        return fail(at, "len(): argument must be a string or list");
    case B_PUSH:
        WANT(0, V_LIST, "a list");
        list_push(args[0].u.l, args[1]);
        return args[0];
    case B_POP:
        WANT(0, V_LIST, "a list");
        if (args[0].u.l->len == 0) return nil();
        return args[0].u.l->items[--args[0].u.l->len];
    case B_CHR: {
        WANT(0, V_INT, "a number");
        char c = (char)args[0].u.i;
        return str_value(new_str(&c, 1));
    }
    case B_ORD:
        WANT(0, V_STR, "a string");
        return args[0].u.s->len ? num((unsigned char)args[0].u.s->s[0]) : nil();
    case B_RAND:
        WANT(0, V_INT, "a number");
        if (args[0].u.i <= 0) return num(0);
        return num((i64)(random_u64() % (u64)args[0].u.i));
    case B_TIME:
        return num((i64)uptime_ms());
    case B_SLEEP:
        WANT(0, V_INT, "a number");
        if (args[0].u.i > 0) sleep_ms((u64)args[0].u.i);
        return nil();
    case B_READ: {
        WANT(0, V_STR, "a file name");
        i64 size = file_read(args[0].u.s->s, NULL, 0);
        if (size < 0) return nil();
        Str *s = new_str(NULL, (int)size);
        file_read(args[0].u.s->s, s->s, (size_t)size);
        return str_value(s);
    }
    case B_SAVE: {
        WANT(0, V_STR, "a file name");
        Str *s = to_str(args[1], 0);
        return num(file_write(args[0].u.s->s, s->s, (size_t)s->len) == 0);
    }
    case B_CLEAR:
        screen_clear();
        return nil();
    case B_GOTO:
        WANT(0, V_INT, "a number");
        WANT(1, V_INT, "a number");
        screen_goto((int)args[0].u.i, (int)args[1].u.i);
        return nil();
    case B_COLOR:
        if (argc) WANT(0, V_INT, "a number like 0xFF8800");
        screen_color(argc ? (unsigned)args[0].u.i : COLOR_DEFAULT);
        return nil();
    case B_KEY:
        return num(getchar());
    case B_POLL: {
        int c = pollchar();
        return c < 0 ? nil() : num(c);
    }
    case B_EXIT:
        screen_color(COLOR_DEFAULT);
        exit(argc && args[0].type == V_INT ? (int)args[0].u.i : 0);
    case B_SUBSTR: {
        WANT(0, V_STR, "a string");
        WANT(1, V_INT, "a number");
        Str *s = args[0].u.s;
        i64 start = args[1].u.i, count = s->len;
        if (argc == 3) {
            WANT(2, V_INT, "a number");
            count = args[2].u.i;
        }
        if (start < 0) start = 0;
        if (start > s->len) start = s->len;
        if (count < 0) count = 0;
        if (count > s->len - start) count = s->len - start;
        return str_value(new_str(s->s + start, (int)count));
    }
    case B_RANGE: {
        WANT(0, V_INT, "a number");
        i64 from = 0, to = args[0].u.i;
        if (argc == 2) {
            WANT(1, V_INT, "a number");
            from = to;
            to = args[1].u.i;
        }
        if (to - from > 10000000) return fail(at, "range(): too many items");
        Value list = new_list();
        for (i64 i = from; i < to; i++) list_push(list.u.l, num(i));
        return list;
    }
    case B_TYPE:
        switch (args[0].type) {
        case V_NIL: return text("nil");
        case V_INT: return text("int");
        case V_STR: return text("str");
        case V_LIST: return text("list");
        default: return text("fn");
        }
    case B_SIZE: {
        int cols, rows;
        screen_size(&cols, &rows);
        Value list = new_list();
        list_push(list.u.l, num(cols));
        list_push(list.u.l, num(rows));
        return list;
    }
    case B_ABS:
        WANT(0, V_INT, "a number");
        return num(args[0].u.i < 0 ? -args[0].u.i : args[0].u.i);
    case B_MIN:
    case B_MAX:
        WANT(0, V_INT, "a number");
        WANT(1, V_INT, "a number");
        if (id == B_MIN) return num(args[0].u.i < args[1].u.i ? args[0].u.i : args[1].u.i);
        return num(args[0].u.i > args[1].u.i ? args[0].u.i : args[1].u.i);
    }
#undef WANT
    return nil();
}

/* ================================================================ evaluation */

enum { S_OK, S_BREAK, S_CONTINUE, S_RETURN, S_ERROR };

static Value return_value;

static Value eval(Node *n);
static int exec_list(Node *first);

static const char *type_name(Value v) {
    switch (v.type) {
    case V_NIL: return "nil";
    case V_INT: return "a number";
    case V_STR: return "a string";
    case V_LIST: return "a list";
    default: return "a function";
    }
}

static int values_equal(Value a, Value b) {
    if (a.type != b.type) return 0;
    switch (a.type) {
    case V_NIL: return 1;
    case V_INT: return a.u.i == b.u.i;
    case V_STR: return a.u.s->len == b.u.s->len && memcmp(a.u.s->s, b.u.s->s, (size_t)a.u.s->len) == 0;
    case V_LIST: return a.u.l == b.u.l;
    default: return a.u.fn == b.u.fn;
    }
}

static Value binary(Node *n, Value a, Value b) {
    int op = n->op;
    if (op == TK_EQ) return num(values_equal(a, b));
    if (op == TK_NE) return num(!values_equal(a, b));

    if (a.type == V_INT && b.type == V_INT) {
        i64 x = a.u.i, y = b.u.i;
        switch (op) {
        case '+': return num((i64)((u64)x + (u64)y));
        case '-': return num((i64)((u64)x - (u64)y));
        case '*': return num((i64)((u64)x * (u64)y));
        case '/':
        case '%':
            if (y == 0) return fail(n->line, "division by zero");
            if (y == -1) return num(op == '/' ? (i64)(0 - (u64)x) : 0);
            return num(op == '/' ? x / y : x % y);
        case '<': return num(x < y);
        case '>': return num(x > y);
        case TK_LE: return num(x <= y);
        case TK_GE: return num(x >= y);
        }
    }
    if (op == '+' && (a.type == V_STR || b.type == V_STR)) {
        Str *x = to_str(a, 0), *y = to_str(b, 0);
        Str *joined = new_str(NULL, x->len + y->len);
        memcpy(joined->s, x->s, (size_t)x->len);
        memcpy(joined->s + x->len, y->s, (size_t)y->len);
        if (a.type != V_STR) free(x);
        if (b.type != V_STR) free(y);
        return str_value(joined);
    }
    if (op == '+' && a.type == V_LIST && b.type == V_LIST) {
        Value list = new_list();
        for (int i = 0; i < a.u.l->len; i++) list_push(list.u.l, a.u.l->items[i]);
        for (int i = 0; i < b.u.l->len; i++) list_push(list.u.l, b.u.l->items[i]);
        return list;
    }
    if (a.type == V_STR && b.type == V_STR && (op == '<' || op == '>' || op == TK_LE || op == TK_GE)) {
        int c = strcmp(a.u.s->s, b.u.s->s);
        return num(op == '<' ? c < 0 : op == '>' ? c > 0 : op == TK_LE ? c <= 0 : c >= 0);
    }
    const char *symbol = op == TK_LE ? "<=" : op == TK_GE ? ">=" : NULL;
    char single[2] = {(char)op, 0};
    return fail(n->line, "cannot use '%s' on %s and %s", symbol ? symbol : single, type_name(a), type_name(b));
}

/* Resolve list[index] to a slot, with negative indexes counting from the end. */
static Value *list_slot(Node *n, Value list, Value index) {
    if (index.type != V_INT) {
        fail(n->line, "a list index must be a number, not %s", type_name(index));
        return NULL;
    }
    i64 i = index.u.i < 0 ? index.u.i + list.u.l->len : index.u.i;
    if (i < 0 || i >= list.u.l->len) {
        fail(n->line, "index %lld is outside the list (length %d)", index.u.i, list.u.l->len);
        return NULL;
    }
    return &list.u.l->items[i];
}

static Value call(Value callee, Value *args, int argc, int at) {
    if (callee.type == V_BUILTIN) return call_builtin((int)callee.u.i, args, argc, at);
    if (callee.type != V_FN) return fail(at, "cannot call %s", type_name(callee));
    Node *fn = callee.u.fn;
    if (argc != fn->count) {
        return fail(at, "%s() takes %d argument(s) but was given %d", fn->name, fn->count, argc);
    }
    if (call_depth >= 400) return fail(at, "too much recursion in %s()", fn->name);

    Scope scope = {0};
    Node *param = fn->a;
    for (int i = 0; i < argc; i++, param = param->next) define(&scope, param->name, args[i]);
    Scope *saved = locals;
    locals = &scope;
    call_depth++;
    int status = exec_list(fn->b);
    call_depth--;
    locals = saved;
    free(scope.vars);

    Value result = status == S_RETURN ? return_value : nil();
    return_value = nil();
    return result;
}

static Value eval(Node *n) {
    switch (n->kind) {
    case N_LIT:
        return n->val;
    case N_VAR: {
        Value *v = find(n->name);
        return v ? *v : fail(n->line, "'%s' is not defined", n->name);
    }
    case N_LIST: {
        Value list = new_list();
        for (Node *item = n->a; item; item = item->next) {
            Value v = eval(item);
            if (failed) return nil();
            list_push(list.u.l, v);
        }
        return list;
    }
    case N_INDEX: {
        Value target = eval(n->a);
        if (failed) return nil();
        Value index = eval(n->b);
        if (failed) return nil();
        if (target.type == V_LIST) {
            Value *slot = list_slot(n, target, index);
            return slot ? *slot : nil();
        }
        if (target.type == V_STR && index.type == V_INT) {
            i64 i = index.u.i < 0 ? index.u.i + target.u.s->len : index.u.i;
            if (i < 0 || i >= target.u.s->len) {
                return fail(n->line, "index %lld is outside the string (length %d)", index.u.i, target.u.s->len);
            }
            return str_value(new_str(target.u.s->s + i, 1));
        }
        return fail(n->line, "cannot index %s", type_name(target));
    }
    case N_CALL: {
        Value callee = eval(n->a);
        if (failed) return nil();
        Value args[16];
        if (n->count > 16) return fail(n->line, "too many arguments (the limit is 16)");
        int argc = 0;
        for (Node *arg = n->b; arg; arg = arg->next) {
            args[argc++] = eval(arg);
            if (failed) return nil();
        }
        return call(callee, args, argc, n->line);
    }
    case N_UNARY: {
        Value v = eval(n->a);
        if (failed) return nil();
        if (n->op == '!') return num(!truthy(v));
        if (v.type != V_INT) return fail(n->line, "cannot negate %s", type_name(v));
        return num((i64)(0 - (u64)v.u.i));
    }
    case N_AND: {
        Value a = eval(n->a);
        if (failed || !truthy(a)) return num(0);
        return num(truthy(eval(n->b)));
    }
    case N_OR: {
        Value a = eval(n->a);
        if (failed) return nil();
        if (truthy(a)) return num(1);
        return num(truthy(eval(n->b)));
    }
    case N_BINARY: {
        Value a = eval(n->a);
        if (failed) return nil();
        Value b = eval(n->b);
        if (failed) return nil();
        return binary(n, a, b);
    }
    }
    return fail(n->line, "internal error: not an expression");
}

static int exec(Node *n) {
    switch (n->kind) {
    case N_LET: {
        Value v = eval(n->a);
        if (failed) return S_ERROR;
        define(current_scope(), n->name, v);
        return S_OK;
    }
    case N_ASSIGN: {
        if (n->a->kind == N_VAR) {
            Value v = eval(n->b);
            if (failed) return S_ERROR;
            /* Existing local, else existing global, else a new variable here. */
            Value *slot = find(n->a->name);
            if (slot) {
                *slot = v;
            } else {
                define(current_scope(), n->a->name, v);
            }
            return S_OK;
        }
        Value target = eval(n->a->a);
        if (failed) return S_ERROR;
        Value index = eval(n->a->b);
        if (failed) return S_ERROR;
        Value v = eval(n->b);
        if (failed) return S_ERROR;
        if (target.type != V_LIST) {
            fail(n->line, "only list elements can be assigned, not parts of %s", type_name(target));
            return S_ERROR;
        }
        Value *slot = list_slot(n, target, index);
        if (!slot) return S_ERROR;
        *slot = v;
        return S_OK;
    }
    case N_EXPR:
        eval(n->a);
        return failed ? S_ERROR : S_OK;
    case N_IF: {
        Value cond = eval(n->a);
        if (failed) return S_ERROR;
        return exec_list(truthy(cond) ? n->b : n->c);
    }
    case N_WHILE:
        for (;;) {
            Value cond = eval(n->a);
            if (failed) return S_ERROR;
            if (!truthy(cond)) return S_OK;
            int status = exec_list(n->b);
            if (status == S_BREAK) return S_OK;
            if (status == S_RETURN || status == S_ERROR) return status;
        }
    case N_FOR: {
        Value from = eval(n->a);
        if (failed) return S_ERROR;
        Value to = nil();
        if (n->b) {
            to = eval(n->b);
            if (failed) return S_ERROR;
            if (from.type != V_INT || to.type != V_INT) {
                fail(n->line, "both ends of a..b must be numbers");
                return S_ERROR;
            }
        } else if (from.type != V_LIST && from.type != V_STR) {
            fail(n->line, "cannot loop over %s", type_name(from));
            return S_ERROR;
        }
        for (i64 i = n->b ? from.u.i : 0;; i++) {
            Value item;
            if (n->b) {
                if (i >= to.u.i) break;
                item = num(i);
            } else if (from.type == V_LIST) {
                if (i >= from.u.l->len) break;
                item = from.u.l->items[i];
            } else {
                if (i >= from.u.s->len) break;
                item = str_value(new_str(from.u.s->s + i, 1));
            }
            define(current_scope(), n->name, item);
            int status = exec_list(n->c);
            if (status == S_BREAK) break;
            if (status == S_RETURN || status == S_ERROR) return status;
        }
        return S_OK;
    }
    case N_FN: {
        Value v = {V_FN, {0}};
        v.u.fn = n;
        define(&globals, n->name, v);
        return S_OK;
    }
    case N_RETURN:
        return_value = n->a ? eval(n->a) : nil();
        return failed ? S_ERROR : S_RETURN;
    case N_BREAK:
        return S_BREAK;
    case N_CONTINUE:
        return S_CONTINUE;
    }
    fail(n->line, "internal error: not a statement");
    return S_ERROR;
}

static int exec_list(Node *first) {
    for (Node *n = first; n; n = n->next) {
        int status = exec(n);
        if (failed) return S_ERROR;
        if (status != S_OK) return status;
    }
    return S_OK;
}

/* ================================================================ entry points */

static void setup(void) {
    for (int i = 0; i < B_COUNT; i++) {
        Value v = {V_BUILTIN, {0}};
        v.u.i = i;
        define(&globals, intern(builtins[i].name, (int)strlen(builtins[i].name)), v);
    }
}

static int repl(void) {
    printf("RX interactive prompt. Type help() for functions, exit() to leave.\n");
    char *buf = malloc(4096);
    for (;;) {
        int len = 0, open = 0;
        screen_color(0x9AEDFE);
        printf("rx> ");
        flush();
        screen_color(COLOR_DEFAULT);
        /* Keep reading lines until every '{' has been closed. */
        for (;;) {
            int n = readline(buf + len, 4000 - len);
            for (int i = len; i < len + n; i++) {
                if (buf[i] == '{') open++;
                if (buf[i] == '}') open--;
            }
            len += n;
            buf[len++] = '\n';
            if (open <= 0 || len > 3900) break;
            printf("... ");
        }
        buf[len] = 0;

        failed = 0;
        if (strcmp(buf, "help()\n") == 0) {
            for (int i = 0; i < B_COUNT; i++) printf("  %8s  %s\n", builtins[i].name, builtins[i].help);
            continue;
        }
        for (Node *n = parse(buf); n && !failed; n = n->next) {
            if (n->kind == N_EXPR) {
                /* A bare expression: show its value, like a calculator. */
                Value v = eval(n->a);
                if (!failed && v.type != V_NIL) {
                    print_value(v, 1);
                    putchar('\n');
                }
            } else if (exec(n) == S_RETURN) {
                break;
            }
        }
    }
}

int main(int argc, char **argv) {
    setup();
    if (argc < 2) return repl();

    /* `rx game` runs game.rx if there is no file called just `game`. */
    char name[64];
    size_t n = strlen(argv[1]);
    if (n > 56) n = 56;
    memcpy(name, argv[1], n);
    name[n] = 0;
    i64 size = file_read(name, NULL, 0);
    if (size < 0) {
        strcpy(name + n, ".rx");
        size = file_read(name, NULL, 0);
    }
    if (size < 0) {
        printf("rx: cannot find %s\n", argv[1]);
        return 1;
    }
    char *source = malloc((size_t)size + 1);
    file_read(name, source, (size_t)size);
    source[size] = 0;
    script_name = name;

    /* Script arguments are available as the list `args`. */
    Value args = new_list();
    for (int i = 2; i < argc; i++) list_push(args.u.l, text(argv[i]));
    define(&globals, intern("args", 4), args);

    Node *program = parse(source);
    if (!failed) exec_list(program);
    screen_color(COLOR_DEFAULT);
    return failed ? 1 : 0;
}
