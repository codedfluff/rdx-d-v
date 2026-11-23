/* web/js_bigint.c - BigInt arithmetic: whole numbers of any size. Part of
   the browser's JavaScript engine.

   A BigInt value (V_BIG) points at a string-kind heap block: byte 0 is the
   sign (1 negative), then the magnitude as 32-bit limbs, least significant
   first, with no zero limbs at the top (zero has none). Being normalised,
   two equal BigInts have equal bytes, so === can compare them as strings.
   The work is done on Big, a malloc'd copy; these functions never throw:
   they return 0 for "not a valid BigInt" and the caller raises the error. */

typedef struct {
    int neg, n, cap;
    unsigned *d;
} Big;

static void big_free(Big *b) {
    free(b->d);
    b->d = NULL;
    b->n = b->cap = 0;
}

static int big_reserve(Big *b, int n) {
    if (n <= b->cap) return 1;
    int cap = n < 4 ? 4 : n;
    unsigned *d = realloc(b->d, sizeof(unsigned) * (size_t)cap);
    if (!d) {
        js_oom_where(__LINE__);
        return 0;
    }
    b->d = d;
    b->cap = cap;
    return 1;
}

static void big_trim(Big *b) {
    while (b->n && b->d[b->n - 1] == 0) b->n--;
    if (!b->n) b->neg = 0;
}

static void big_set_u64(Big *b, unsigned long long v) {
    b->neg = 0;
    b->n = 0;
    if (!big_reserve(b, 2)) return;
    while (v) {
        b->d[b->n++] = (unsigned)v;
        v >>= 32;
    }
}

static void big_copy(Big *to, const Big *from) {
    if (!big_reserve(to, from->n)) return;
    to->n = from->n;
    to->neg = from->neg;
    if (from->n) memcpy(to->d, from->d, sizeof(unsigned) * (size_t)from->n);
}

static void big_load(JsStr *s, Big *b) {
    b->neg = s->len > 0 && s->s[0];
    b->n = s->len > 0 ? (s->len - 1) / 4 : 0;
    b->d = NULL;
    b->cap = 0;
    if (!big_reserve(b, b->n)) {
        b->n = 0;
        return;
    }
    if (b->n) memcpy(b->d, s->s + 1, sizeof(unsigned) * (size_t)b->n);
}

/* The value as a heap block (and frees the Big). */
static JsStr *big_store(Big *b) {
    big_trim(b);
    JsStr *s = str_new(NULL, 1 + 4 * b->n);
    if (s->len == 1 + 4 * b->n) {
        s->s[0] = (char)b->neg;
        if (b->n) memcpy(s->s + 1, b->d, sizeof(unsigned) * (size_t)b->n);
    }
    big_free(b);
    return s;
}

static Value v_big(JsStr *s) {
    Value v;
    v.t = V_BIG;
    v.u.n = 0;
    v.u.s = s;
    return v;
}

/* ---------------------------------------------------------------- magnitudes */

static int mag_cmp(const Big *a, const Big *b) {
    if (a->n != b->n) return a->n < b->n ? -1 : 1;
    for (int i = a->n - 1; i >= 0; i--)
        if (a->d[i] != b->d[i]) return a->d[i] < b->d[i] ? -1 : 1;
    return 0;
}

/* r = |a| + |b| (r may be a or b) */
static void mag_add(Big *r, const Big *a, const Big *b) {
    int n = a->n > b->n ? a->n : b->n;
    unsigned *ad = a->d, *bd = b->d;
    int an = a->n, bn = b->n;
    Big t = {0};
    if (!big_reserve(&t, n + 1)) return;
    unsigned long long carry = 0;
    for (int i = 0; i < n; i++) {
        carry += (unsigned long long)(i < an ? ad[i] : 0) + (i < bn ? bd[i] : 0);
        t.d[i] = (unsigned)carry;
        carry >>= 32;
    }
    t.d[n] = (unsigned)carry;
    t.n = n + 1;
    free(r->d);
    r->d = t.d;
    r->cap = t.cap;
    r->n = t.n;
}

/* r = |a| - |b|, needs |a| >= |b| */
static void mag_sub(Big *r, const Big *a, const Big *b) {
    int an = a->n, bn = b->n;
    Big t = {0};
    if (!big_reserve(&t, an)) return;
    long long borrow = 0;
    for (int i = 0; i < an; i++) {
        long long v = (long long)a->d[i] - (i < bn ? b->d[i] : 0) - borrow;
        borrow = v < 0;
        t.d[i] = (unsigned)(v + (borrow ? (1LL << 32) : 0));
    }
    t.n = an;
    free(r->d);
    r->d = t.d;
    r->cap = t.cap;
    r->n = t.n;
}

static void mag_mul(Big *r, const Big *a, const Big *b) {
    Big t = {0};
    int n = a->n + b->n;
    if (!big_reserve(&t, n ? n : 1)) return;
    memset(t.d, 0, sizeof(unsigned) * (size_t)(n ? n : 1));
    for (int i = 0; i < a->n; i++) {
        unsigned long long carry = 0, x = a->d[i];
        if (!x) continue;
        for (int j = 0; j < b->n; j++) {
            carry += x * b->d[j] + t.d[i + j];
            t.d[i + j] = (unsigned)carry;
            carry >>= 32;
        }
        t.d[i + b->n] = (unsigned)carry;
    }
    t.n = n;
    free(r->d);
    r->d = t.d;
    r->cap = t.cap;
    r->n = t.n;
}

/* |a| divided by a small number: the quotient in place, the remainder back */
static unsigned mag_div_small(Big *a, unsigned by) {
    unsigned long long rem = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        unsigned long long cur = (rem << 32) | a->d[i];
        a->d[i] = (unsigned)(cur / by);
        rem = cur % by;
    }
    big_trim(a);
    return (unsigned)rem;
}

static int top_zero_bits(unsigned x) {
    int n = 0;
    if (!x) return 32;
    while (!(x & 0x80000000u)) {
        x <<= 1;
        n++;
    }
    return n;
}

/* q = |a| / |b|, m = |a| % |b| (Knuth's algorithm D); b is not zero.
   Either output may be NULL. */
static void mag_divmod(const Big *a, const Big *b, Big *q, Big *m) {
    if (mag_cmp(a, b) < 0) {
        if (m) {
            big_copy(m, a);
            m->neg = 0;
        }
        if (q) q->n = 0;
        return;
    }
    if (b->n == 1) {
        Big t = {0};
        big_copy(&t, a);
        t.neg = 0;
        unsigned r = mag_div_small(&t, b->d[0]);
        if (m) big_set_u64(m, r);
        if (q) {
            free(q->d);
            *q = t;
            q->neg = 0;
        } else {
            big_free(&t);
        }
        return;
    }
    int s = top_zero_bits(b->d[b->n - 1]);
    int n = b->n, mlen = a->n - b->n;
    unsigned *v = malloc(sizeof(unsigned) * (size_t)n), *u = malloc(sizeof(unsigned) * (size_t)(a->n + 1));
    unsigned *qd = malloc(sizeof(unsigned) * (size_t)(mlen + 1));
    if (!v || !u || !qd) {
        js_oom_where(__LINE__);
        free(v);
        free(u);
        free(qd);
        return;
    }
    /* normalise: shift both so the divisor's top bit is set */
    for (int i = n - 1; i > 0; i--) v[i] = (b->d[i] << s) | (s ? b->d[i - 1] >> (32 - s) : 0);
    v[0] = b->d[0] << s;
    u[a->n] = s ? a->d[a->n - 1] >> (32 - s) : 0;
    for (int i = a->n - 1; i > 0; i--) u[i] = (a->d[i] << s) | (s ? a->d[i - 1] >> (32 - s) : 0);
    u[0] = a->d[0] << s;
    for (int j = mlen; j >= 0; j--) {
        unsigned long long num = ((unsigned long long)u[j + n] << 32) | u[j + n - 1];
        unsigned long long qhat = num / v[n - 1], rhat = num % v[n - 1];
        while (qhat >= (1ULL << 32) || qhat * v[n - 2] > ((rhat << 32) | u[j + n - 2])) {
            qhat--;
            rhat += v[n - 1];
            if (rhat >= (1ULL << 32)) break;
        }
        /* u[j..j+n] -= qhat * v */
        long long borrow = 0;
        unsigned long long carry = 0;
        for (int i = 0; i < n; i++) {
            unsigned long long p = qhat * v[i] + carry;
            carry = p >> 32;
            long long t = (long long)u[i + j] - (long long)(p & 0xFFFFFFFFu) - borrow;
            borrow = t < 0;
            u[i + j] = (unsigned)(t + (borrow ? (1LL << 32) : 0));
        }
        long long t = (long long)u[j + n] - (long long)carry - borrow;
        borrow = t < 0;
        u[j + n] = (unsigned)(t + (borrow ? (1LL << 32) : 0));
        if (borrow) { /* qhat was one too many: add v back */
            qhat--;
            unsigned long long c = 0;
            for (int i = 0; i < n; i++) {
                c += (unsigned long long)u[i + j] + v[i];
                u[i + j] = (unsigned)c;
                c >>= 32;
            }
            u[j + n] += (unsigned)c;
        }
        qd[j] = (unsigned)qhat;
    }
    if (q) {
        free(q->d);
        q->d = qd;
        q->cap = mlen + 1;
        q->n = mlen + 1;
        q->neg = 0;
        big_trim(q);
    } else {
        free(qd);
    }
    if (m) {
        if (big_reserve(m, n)) {
            for (int i = 0; i < n; i++) m->d[i] = (u[i] >> s) | (s && i + 1 <= n ? (unsigned)((unsigned long long)u[i + 1] << (32 - s)) : 0);
            m->n = n;
            m->neg = 0;
            big_trim(m);
        }
    }
    free(u);
    free(v);
}

/* ---------------------------------------------------------------- signed */

static void big_addsub(Big *r, const Big *a, const Big *b, int subtract) {
    int bneg = subtract ? !b->neg : b->neg;
    if (a->neg == bneg) {
        int neg = a->neg;
        mag_add(r, a, b);
        r->neg = neg;
    } else if (mag_cmp(a, b) >= 0) {
        int neg = a->neg;
        mag_sub(r, a, b);
        r->neg = neg;
    } else {
        mag_sub(r, b, a);
        r->neg = bneg;
    }
    big_trim(r);
}

static void big_shift_left(Big *a, unsigned long long bits) {
    if (!a->n || !bits) return;
    int limbs = (int)(bits / 32), s = (int)(bits % 32);
    if (!big_reserve(a, a->n + limbs + 1)) return;
    a->d[a->n + limbs] = 0;
    for (int i = a->n - 1; i >= 0; i--) {
        unsigned x = a->d[i];
        a->d[i + limbs + 1] |= s ? x >> (32 - s) : 0;
        a->d[i + limbs] = x << s;
    }
    for (int i = 0; i < limbs; i++) a->d[i] = 0;
    a->n += limbs + 1;
    big_trim(a);
}

/* the magnitude shifted right (rounding toward zero) */
static void mag_shift_right(Big *a, unsigned long long bits) {
    if (bits >= (unsigned long long)a->n * 32) {
        a->n = 0;
        a->neg = 0;
        return;
    }
    int limbs = (int)(bits / 32), s = (int)(bits % 32);
    for (int i = 0; i + limbs < a->n; i++) {
        unsigned lo = a->d[i + limbs] >> s;
        unsigned hi = s && i + limbs + 1 < a->n ? a->d[i + limbs + 1] << (32 - s) : 0;
        a->d[i] = lo | hi;
    }
    a->n -= limbs;
    big_trim(a);
}

/* a >> bits, rounding toward minus infinity like JavaScript */
static void big_shift_right(Big *a, unsigned long long bits) {
    if (!a->neg) {
        mag_shift_right(a, bits);
        return;
    }
    /* -((|a| - 1) >> bits) - 1 */
    Big one = {0};
    big_set_u64(&one, 1);
    a->neg = 0;
    mag_sub(a, a, &one);
    big_trim(a);
    mag_shift_right(a, bits);
    mag_add(a, a, &one);
    a->neg = 1;
    big_trim(a);
    big_free(&one);
}

/* two's complement in `width` limbs */
static unsigned *twos(const Big *a, int width) {
    unsigned *t = malloc(sizeof(unsigned) * (size_t)width);
    if (!t) return NULL;
    memset(t, 0, sizeof(unsigned) * (size_t)width);
    for (int i = 0; i < a->n && i < width; i++) t[i] = a->d[i];
    if (a->neg) {
        unsigned long long c = 1;
        for (int i = 0; i < width; i++) {
            c += (unsigned)~t[i];
            t[i] = (unsigned)c;
            c >>= 32;
        }
    }
    return t;
}

static void from_twos(Big *r, unsigned *t, int width) {
    int neg = width && (t[width - 1] & 0x80000000u);
    if (neg) {
        unsigned long long c = 1;
        for (int i = 0; i < width; i++) {
            c += (unsigned)~t[i];
            t[i] = (unsigned)c;
            c >>= 32;
        }
    }
    free(r->d);
    r->d = t;
    r->cap = width;
    r->n = width;
    r->neg = neg;
    big_trim(r);
}

/* op: '&', '|', '^' */
static void big_bitwise(Big *r, const Big *a, const Big *b, int op) {
    int width = (a->n > b->n ? a->n : b->n) + 1;
    unsigned *x = twos(a, width), *y = twos(b, width);
    if (!x || !y) {
        free(x);
        free(y);
        return;
    }
    for (int i = 0; i < width; i++) x[i] = op == '&' ? x[i] & y[i] : op == '|' ? x[i] | y[i] : x[i] ^ y[i];
    free(y);
    from_twos(r, x, width);
}

/* BigInt.asUintN / asIntN: the low `bits` bits, as unsigned or signed */
static void big_as_n(Big *a, unsigned long long bits, int as_signed) {
    if (bits == 0) {
        a->n = 0;
        a->neg = 0;
        return;
    }
    if (bits > (1u << 24)) bits = 1u << 24;
    int need = (int)((bits + 31) / 32);
    int width = (a->n > need ? a->n : need) + 1;
    unsigned *t = twos(a, width);
    if (!t) return;
    int full = (int)(bits / 32), rest = (int)(bits % 32);
    for (int i = 0; i < width; i++) {
        if (i > full || (i == full && !rest)) t[i] = 0;
        else if (i == full) t[i] &= (1u << rest) - 1;
    }
    int top = (t[(bits - 1) / 32] >> ((bits - 1) % 32)) & 1;
    free(a->d);
    a->d = t;
    a->cap = width;
    a->n = width;
    a->neg = 0;
    big_trim(a);
    if (as_signed && top) { /* minus 2^bits */
        Big p = {0};
        big_set_u64(&p, 1);
        big_shift_left(&p, bits);
        mag_sub(a, &p, a);
        a->neg = 1;
        big_trim(a);
        big_free(&p);
    }
}

/* ---------------------------------------------------------------- conversions */

static double big_to_double(const Big *a) {
    double r = 0;
    for (int i = a->n - 1; i >= 0; i--) r = r * 4294967296.0 + a->d[i];
    return a->neg ? -r : r;
}

/* from a whole number (0 if it has a fraction or is not finite) */
static int big_from_double(Big *b, double d) {
    if (!is_finite(d) || d != js_trunc(d)) return 0;
    b->neg = d < 0;
    if (d < 0) d = -d;
    b->n = 0;
    while (d >= 1) { /* (exact: d is whole, and dividing by 2^32 is exact) */
        double limb = js_fmod(d, 4294967296.0);
        if (!big_reserve(b, b->n + 1)) return 0;
        b->d[b->n++] = (unsigned)limb;
        d = (d - limb) / 4294967296.0;
    }
    big_trim(b);
    return 1;
}

static int digit_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

/* StringToBigInt: spaces around, an optional sign (decimal only), or a
   0x / 0o / 0b prefix; "" is 0. radix 0 means "decide". 0: not valid. */
static int big_from_text(Big *b, const char *s, int len, int radix) {
    int i = 0, end = len;
    while (i < end && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v')) i++;
    while (end > i && (s[end - 1] == ' ' || s[end - 1] == '\t' || s[end - 1] == '\n' || s[end - 1] == '\r' || s[end - 1] == '\f' || s[end - 1] == '\v')) end--;
    int neg = 0;
    big_set_u64(b, 0);
    if (i == end) return 1;
    if (!radix) {
        radix = 10;
        if (end - i > 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) radix = 16, i += 2;
        else if (end - i > 2 && s[i] == '0' && (s[i + 1] == 'o' || s[i + 1] == 'O')) radix = 8, i += 2;
        else if (end - i > 2 && s[i] == '0' && (s[i + 1] == 'b' || s[i + 1] == 'B')) radix = 2, i += 2;
        else if (s[i] == '+' || s[i] == '-') neg = s[i++] == '-';
    } else if (s[i] == '+' || s[i] == '-') {
        neg = s[i++] == '-';
    }
    if (i == end) return 0;
    for (; i < end; i++) {
        int v = digit_value(s[i]);
        if (v >= radix) return 0;
        /* b = b * radix + v */
        unsigned long long carry = (unsigned)v;
        for (int k = 0; k < b->n; k++) {
            carry += (unsigned long long)b->d[k] * (unsigned)radix;
            b->d[k] = (unsigned)carry;
            carry >>= 32;
        }
        if (carry) {
            if (!big_reserve(b, b->n + 1)) return 0;
            b->d[b->n++] = (unsigned)carry;
        }
    }
    b->neg = neg;
    big_trim(b);
    return 1;
}

static JsStr *big_text(JsStr *v, int radix) {
    Big a;
    big_load(v, &a);
    if (!a.n) {
        big_free(&a);
        return str_c("0");
    }
    /* digits come out backwards, a chunk at a time */
    unsigned chunk = radix, per = 1;
    while ((unsigned long long)chunk * (unsigned)radix <= 0xFFFFFFFFull) {
        chunk *= (unsigned)radix;
        per++;
    }
    Buf b = {0};
    while (a.n) {
        unsigned r = mag_div_small(&a, chunk);
        for (unsigned k = 0; k < per && (a.n || r); k++) {
            buf_ch(&b, "0123456789abcdefghijklmnopqrstuvwxyz"[r % (unsigned)radix]);
            r /= (unsigned)radix;
        }
    }
    if (v->s[0]) buf_ch(&b, '-');
    for (int i = 0, j = b.len - 1; i < j; i++, j--) {
        char c = b.p[i];
        b.p[i] = b.p[j];
        b.p[j] = c;
    }
    big_free(&a);
    return buf_done(&b);
}

static int big_is_zero(JsStr *v) { return v->len <= 1; }

/* -1, 0, 1 */
static int big_compare(JsStr *x, JsStr *y) {
    Big a, b;
    big_load(x, &a);
    big_load(y, &b);
    int r;
    if (a.neg != b.neg) r = a.neg ? -1 : 1;
    else r = a.neg ? -mag_cmp(&a, &b) : mag_cmp(&a, &b);
    big_free(&a);
    big_free(&b);
    return r;
}

/* a BigInt against a Number: -1, 0, 1, or 2 for NaN */
static int big_compare_number(JsStr *x, double d) {
    if (is_nan(d)) return 2;
    if (d == js_inf()) return -1;
    if (d == -js_inf()) return 1;
    Big a;
    big_load(x, &a);
    double t = js_trunc(d);
    Big b = {0};
    big_from_double(&b, t);
    int r;
    if (a.neg != b.neg) r = a.neg ? -1 : 1;
    else r = a.neg ? -mag_cmp(&a, &b) : mag_cmp(&a, &b);
    if (r == 0 && d != t) r = d > t ? -1 : 1;
    big_free(&a);
    big_free(&b);
    return r;
}

static JsStr *big_small(long long v) {
    Big b = {0};
    big_set_u64(&b, (unsigned long long)(v < 0 ? -v : v));
    b.neg = v < 0;
    return big_store(&b);
}

/* x op y. Returns 0 with *error set for RangeErrors (division by zero,
   negative exponent, too big). op: + - * / % ** & | ^ << >> */
static JsStr *big_arith(int op, JsStr *x, JsStr *y, const char **error) {
    Big a, b, r = {0};
    big_load(x, &a);
    big_load(y, &b);
    JsStr *out = NULL;
    switch (op) {
    case '+': big_addsub(&r, &a, &b, 0); break;
    case '-': big_addsub(&r, &a, &b, 1); break;
    case '*':
        mag_mul(&r, &a, &b);
        r.neg = a.neg != b.neg;
        break;
    case '/':
    case '%':
        if (!b.n) {
            *error = "Division by zero";
            goto done;
        }
        if (op == '/') {
            mag_divmod(&a, &b, &r, NULL);
            r.neg = a.neg != b.neg;
        } else {
            mag_divmod(&a, &b, NULL, &r);
            r.neg = a.neg;
        }
        break;
    case P2('*', '*'): {
        if (b.neg) {
            *error = "Exponent must be non-negative";
            goto done;
        }
        big_set_u64(&r, 1);
        if (!b.n) break;
        if (b.n > 1 && a.n && !(a.n == 1 && a.d[0] == 1)) {
            *error = "Maximum BigInt size exceeded";
            goto done;
        }
        unsigned e = b.d[0];
        if (a.n && (unsigned long long)e * (unsigned long long)(a.n * 32) > (1ull << 30)) {
            *error = "Maximum BigInt size exceeded";
            goto done;
        }
        Big base = {0};
        big_copy(&base, &a);
        int neg = a.neg && (e & 1);
        base.neg = 0;
        while (e) {
            if (e & 1) mag_mul(&r, &r, &base);
            e >>= 1;
            if (e) mag_mul(&base, &base, &base);
            big_trim(&r);
            big_trim(&base);
        }
        big_free(&base);
        r.neg = neg;
        break;
    }
    case '&': case '|': case '^': big_bitwise(&r, &a, &b, op); break;
    case P2('<', '<'):
    case P2('>', '>'): {
        int left = op == P2('<', '<');
        if (b.neg) left = !left;
        if (b.n > 1 || (b.n == 1 && b.d[0] > (1u << 30))) {
            if (left && a.n) {
                *error = "Maximum BigInt size exceeded";
                goto done;
            }
            big_set_u64(&r, 0);
            if (!left && a.neg) { /* shifting a negative far right gives -1 */
                big_set_u64(&r, 1);
                r.neg = 1;
            }
            break;
        }
        unsigned long long bits = b.n ? b.d[0] : 0;
        big_copy(&r, &a);
        if (left) big_shift_left(&r, bits);
        else big_shift_right(&r, bits);
        break;
    }
    default: *error = "unsupported BigInt operation"; goto done;
    }
    out = big_store(&r);
done:
    big_free(&a);
    big_free(&b);
    big_free(&r);
    return out;
}

static JsStr *big_negate(JsStr *x) {
    Big a;
    big_load(x, &a);
    if (a.n) a.neg = !a.neg;
    return big_store(&a);
}

/* ~x = -x - 1 */
static JsStr *big_not(JsStr *x) {
    JsStr *one = big_small(1);
    const char *e = NULL;
    return big_arith('-', big_negate(x), one, &e);
}

/* A BigInt literal's value (the lexer's text, without the n), interned:
   literals live as long as the syntax tree. */
static JsStr *bigint_literal(const char *text, int len) {
    Big b = {0};
    if (!big_from_text(&b, text, len, 0)) big_set_u64(&b, 0);
    big_trim(&b);
    char small[64], *bytes = 1 + 4 * b.n <= (int)sizeof(small) ? small : malloc((size_t)(1 + 4 * b.n));
    if (!bytes) {
        big_free(&b);
        return intern("", 1);
    }
    bytes[0] = (char)b.neg;
    if (b.n) memcpy(bytes + 1, b.d, sizeof(unsigned) * (size_t)b.n);
    JsStr *r = intern(bytes, 1 + 4 * b.n);
    if (bytes != small) free(bytes);
    big_free(&b);
    return r;
}
