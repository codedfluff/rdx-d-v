/* web/cookies.c - the cookie jar: what sites ask the browser to remember
   (Set-Cookie) and send back (Cookie), with the rules browsers follow for
   which site sees which cookie (RFC 6265 and SameSite). Cookies with an
   expiry date are kept in a file between runs. Part of the browser. */

#define MAX_COOKIES 600
#define MAX_COOKIE_BYTES 4096           /* name and value together */
#define COOKIE_FILE "browser.cookies"

enum { SAME_LAX, SAME_STRICT, SAME_NONE };

/* Who is asking, for SameSite: */
enum {
    CTX_SAME_SITE,      /* the page's own site: every cookie */
    CTX_CROSS_TOP,      /* following a link to another site: not Strict ones */
    CTX_CROSS_SUB       /* a part of a page from another site, or a cross-site POST: only SameSite=None */
};

typedef struct {
    char *name, *value;  /* (value points into the same allocation as name) */
    char domain[128];
    char path[128];
    u64 expires;         /* seconds since 1970; 0: only until the browser closes */
    unsigned char host_only, secure, http_only, same_site;
    u64 serial;          /* creation order */
} Cookie;

static Cookie cookies[MAX_COOKIES];
static int cookie_count;
static u64 cookie_serial;
static int cookies_loaded, cookies_dirty;

/* Copy `s` to `out` (room for `room` bytes, not NUL-terminated); returns how many were copied. */
static int snprintf_lite(char *out, size_t room, const char *s) {
    size_t n = strlen(s);
    if (n > room) n = room;
    memcpy(out, s, n);
    return (int)n;
}

static char lower_c(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }
static int digit_c(char c) { return c >= '0' && c <= '9'; }
static int alnum_c(char c) { return digit_c(c) || (lower_c(c) >= 'a' && lower_c(c) <= 'z'); }

static int same_text_nocase(const char *a, const char *b) {
    size_t n = strlen(a);
    return n == strlen(b) && strncasecmp(a, b, n) == 0;
}

/* The host (lowercase) and path (without the query) of an http(s) address.
   Pages that are files on this machine get a host of their own, which no
   network server can have. */
static int url_host_path(const char *url, int *https, char *host, int hmax, char *path, int pmax) {
    *https = strncasecmp(url, "https://", 8) == 0;
    if (!*https && strncasecmp(url, "http://", 7) != 0) {
        strcpy(host, "(this machine)");
        strcpy(path, "/");
        return 1;
    }
    const char *p = url + (*https ? 8 : 7);
    const char *at = NULL; /* user:password@host */
    for (const char *q = p; *q && *q != '/' && *q != '?' && *q != '#'; q++) {
        if (*q == '@') at = q;
    }
    if (at) p = at + 1;
    int n = 0;
    while (*p && *p != '/' && *p != ':' && *p != '?' && *p != '#') {
        if (n < hmax - 1) host[n++] = lower_c(*p);
        p++;
    }
    while (n > 0 && host[n - 1] == '.') n--;
    host[n] = 0;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    n = 0;
    if (*p != '/') path[n++] = '/';
    while (*p && *p != '?' && *p != '#' && n < pmax - 1) path[n++] = *p++;
    path[n] = 0;
    return host[0] != 0;
}

static int is_ip_address(const char *host) {
    for (const char *p = host; *p; p++) {
        if (!digit_c(*p) && *p != '.') return host[0] == '[';
    }
    return 1;
}

/* Is `d` a public suffix ("com", "co.uk", "github.io"): a name under which
   unrelated people have sites, so no site may set cookies for all of it.
   (An approximation of the public suffix list.) */
static int public_suffix(const char *d) {
    const char *dot = strchr(d, '.');
    if (!dot) return 1;
    static const char *shared[] = {"github.io", "gitlab.io", "blogspot.com", "herokuapp.com", "netlify.app", "vercel.app",
                                   "pages.dev", "workers.dev", "web.app", "firebaseapp.com", "appspot.com",
                                   "azurewebsites.net", "cloudfront.net", "fly.dev", "onrender.com", "glitch.me",
                                   "neocities.org", "wordpress.com", "tumblr.com", "s3.amazonaws.com", NULL};
    for (int i = 0; shared[i]; i++) {
        if (same_text_nocase(d, shared[i])) return 1;
    }
    if (strchr(dot + 1, '.')) return 0;
    /* two labels: co.uk, com.au, ne.jp, org.br ... */
    size_t first = (size_t)(dot - d), last = strlen(dot + 1);
    return last == 2 && first <= 3;
}

/* The site a host belongs to: its registrable domain (www.forum.example.co.uk
   -> example.co.uk). Two addresses on the same site are "same-site". */
static const char *site_of(const char *host) {
    if (is_ip_address(host)) return host;
    for (const char *p = host + strlen(host) - 1; p > host; p--) {
        if (*p == '.' && !public_suffix(p + 1)) return p + 1;
    }
    return host;
}

static int domain_match(const char *host, const char *domain) {
    size_t h = strlen(host), d = strlen(domain);
    if (h == d) return strcmp(host, domain) == 0;
    return h > d && host[h - d - 1] == '.' && strcmp(host + h - d, domain) == 0 && !is_ip_address(host);
}

static int path_match(const char *path, const char *cookie_path) {
    size_t n = strlen(cookie_path);
    if (strncmp(path, cookie_path, n) != 0) return 0;
    return path[n] == 0 || cookie_path[n - 1] == '/' || path[n] == '/';
}

static void default_path(const char *path, char *out, int max) {
    const char *last = strrchr(path, '/');
    if (!last || last == path) {
        strcpy(out, "/");
        return;
    }
    int n = (int)(last - path);
    if (n > max - 1) n = max - 1;
    memcpy(out, path, (size_t)n);
    out[n] = 0;
}

/* ---- dates: "Wed, 21 Oct 2026 07:28:00 GMT" and the older forms ---- */

static i64 cookie_days(i64 y, int m, int d) {
    y -= m <= 2;
    i64 era = (y >= 0 ? y : y - 399) / 400;
    i64 yoe = y - era * 400;
    i64 doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    i64 doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* RFC 6265 section 5.1.1. Returns 0 if it is not a date. */
static int parse_cookie_date(const char *s, i64 *out) {
    int hh = -1, mi = 0, ss = 0, day = -1, month = -1, year = -1;
    static const char months[] = "janfebmaraprmayjunjulaugsepoctnovdec";
    while (*s) {
        while (*s && !alnum_c(*s) && *s != ':') s++;
        const char *t = s;
        while (*s && (alnum_c(*s) || *s == ':')) s++;
        int len = (int)(s - t);
        if (!len) continue;
        if (hh < 0 && digit_c(t[0]) && memchr(t, ':', (size_t)len)) {
            int v[3] = {0, 0, 0}, k = 0;
            for (const char *q = t; q < s && k < 3; q++) {
                if (*q == ':') k++;
                else if (digit_c(*q)) v[k] = v[k] * 10 + (*q - '0');
            }
            hh = v[0], mi = v[1], ss = v[2];
            continue;
        }
        if (digit_c(t[0])) {
            int digits = 0, v = 0;
            while (digits < len && digit_c(t[digits])) v = v * 10 + (t[digits++] - '0');
            if (day < 0 && digits <= 2) day = v;
            else if (year < 0 && digits >= 2 && digits <= 4) year = v;
            continue;
        }
        if (month < 0 && len >= 3) {
            for (int m = 0; m < 12; m++) {
                if (lower_c(t[0]) == months[m * 3] && lower_c(t[1]) == months[m * 3 + 1] && lower_c(t[2]) == months[m * 3 + 2]) month = m + 1;
            }
        }
    }
    if (year >= 70 && year <= 99) year += 1900;
    else if (year >= 0 && year <= 69) year += 2000;
    if (day < 1 || day > 31 || month < 1 || year < 1601 || hh < 0 || hh > 23 || mi > 59 || ss > 59) return 0;
    *out = cookie_days(year, month, day) * 86400 + hh * 3600 + mi * 60 + ss;
    return 1;
}

/* ---- the jar ---- */

static void cookie_remove(int i) {
    if (cookies[i].expires) cookies_dirty = 1;
    free(cookies[i].name);
    cookies[i] = cookies[--cookie_count];
}

static void cookies_expire(void) {
    u64 now = time_now();
    for (int i = cookie_count - 1; i >= 0; i--) {
        if (cookies[i].expires && cookies[i].expires <= now) cookie_remove(i);
    }
}

static void cookie_store(Cookie *c, const char *name, const char *value) {
    size_t nl = strlen(name), vl = strlen(value);
    char *block = malloc(nl + vl + 2);
    if (!block) return;
    memcpy(block, name, nl + 1);
    memcpy(block + nl + 1, value, vl + 1);
    c->name = block;
    c->value = block + nl + 1;
    /* the same cookie (name, domain, path) is replaced, keeping its age */
    for (int i = 0; i < cookie_count; i++) {
        Cookie *o = &cookies[i];
        if (!strcmp(o->name, name) && !strcmp(o->domain, c->domain) && !strcmp(o->path, c->path)) {
            c->serial = o->serial;
            if (o->expires || c->expires) cookies_dirty = 1;
            free(o->name);
            *o = *c;
            return;
        }
    }
    if (cookie_count == MAX_COOKIES) { /* full: the oldest goes */
        int oldest = 0;
        for (int i = 1; i < cookie_count; i++) {
            if (cookies[i].serial < cookies[oldest].serial) oldest = i;
        }
        cookie_remove(oldest);
    }
    c->serial = ++cookie_serial;
    if (c->expires) cookies_dirty = 1;
    cookies[cookie_count++] = *c;
}

static void trim_copy(const char *from, const char *to, char *out, int max) {
    while (from < to && (*from == ' ' || *from == '\t')) from++;
    while (to > from && (to[-1] == ' ' || to[-1] == '\t')) to--;
    int n = (int)(to - from);
    if (n > max - 1) n = max - 1;
    if (n < 0) n = 0;
    memcpy(out, from, (size_t)n);
    out[n] = 0;
}

static void cookies_load(void);

/* A Set-Cookie header (or document.cookie = ...) from the page at `url`. */
static void cookie_set(const char *url, const char *line, int from_script) {
    cookies_load();
    int https;
    char host[256], path[1024];
    if (!url_host_path(url, &https, host, sizeof(host), path, sizeof(path))) return;
    const char *semi = strchr(line, ';');
    const char *pair_end = semi ? semi : line + strlen(line);
    const char *eq = memchr(line, '=', (size_t)(pair_end - line));
    static char name[MAX_COOKIE_BYTES], value[MAX_COOKIE_BYTES];
    if (eq) {
        trim_copy(line, eq, name, sizeof(name));
        trim_copy(eq + 1, pair_end, value, sizeof(value));
    } else {
        name[0] = 0;
        trim_copy(line, pair_end, value, sizeof(value));
    }
    if (!name[0] && !value[0]) return;
    if (strlen(name) + strlen(value) >= MAX_COOKIE_BYTES - 1) return;
    for (const char *p = name; *p; p++) {
        if ((unsigned char)*p < 32 || *p == '\t') return;
    }
    for (const char *p = value; *p; p++) {
        if ((unsigned char)*p < 32 || *p == '\t') return;
    }
    Cookie c;
    memset(&c, 0, sizeof(c));
    char domain[256] = "", cpath[256] = "";
    i64 expires = 0;
    int have_expiry = 0, have_max_age = 0;
    i64 max_age = 0;
    for (const char *a = semi; a && *a;) {
        a++;
        const char *end = strchr(a, ';');
        if (!end) end = a + strlen(a);
        const char *aeq = memchr(a, '=', (size_t)(end - a));
        char key[32], val[256];
        trim_copy(a, aeq ? aeq : end, key, sizeof(key));
        if (aeq) trim_copy(aeq + 1, end, val, sizeof(val));
        else val[0] = 0;
        if (same_text_nocase(key, "expires")) {
            i64 t;
            if (parse_cookie_date(val, &t)) {
                expires = t;
                have_expiry = 1;
            }
        } else if (same_text_nocase(key, "max-age")) {
            const char *v = val;
            int neg = *v == '-';
            if (neg) v++;
            if (digit_c(*v)) {
                i64 n = 0;
                while (digit_c(*v) && n < 100000000000ll) n = n * 10 + (*v++ - '0');
                max_age = neg ? -n : n;
                have_max_age = 1;
            }
        } else if (same_text_nocase(key, "domain")) {
            const char *v = val[0] == '.' ? val + 1 : val;
            int k = 0;
            for (; v[k] && k < 255; k++) domain[k] = lower_c(v[k]);
            domain[k] = 0;
        } else if (same_text_nocase(key, "path")) {
            if (val[0] == '/') strcpy(cpath, val);
        } else if (same_text_nocase(key, "secure")) {
            c.secure = 1;
        } else if (same_text_nocase(key, "httponly")) {
            c.http_only = 1;
        } else if (same_text_nocase(key, "samesite")) {
            c.same_site = same_text_nocase(val, "strict") ? SAME_STRICT : same_text_nocase(val, "none") ? SAME_NONE : SAME_LAX;
        }
    }
    if (from_script && c.http_only) return;
    if (c.secure && !https && strcmp(host, "localhost")) return; /* only a secure page may set a Secure cookie */
    if (c.same_site == SAME_NONE && !c.secure) c.same_site = SAME_LAX;
    /* which hosts will see it */
    if (domain[0]) {
        if (public_suffix(domain) && strcmp(domain, host) != 0) return;
        if (!domain_match(host, domain)) return;
        c.host_only = public_suffix(domain) || is_ip_address(host);
        strncpy(c.domain, domain, sizeof(c.domain) - 1);
    } else {
        c.host_only = 1;
        strncpy(c.domain, host, sizeof(c.domain) - 1);
    }
    if (cpath[0]) strncpy(c.path, cpath, sizeof(c.path) - 1);
    else default_path(path, c.path, sizeof(c.path));
    /* the __Secure- and __Host- prefixes promise how the cookie was set */
    if (!strncmp(name, "__Secure-", 9) && !c.secure) return;
    if (!strncmp(name, "__Host-", 7) && (!c.secure || domain[0] || strcmp(c.path, "/"))) return;
    /* a page that is not secure may not replace a Secure cookie */
    if (!https) {
        for (int i = 0; i < cookie_count; i++) {
            if (cookies[i].secure && !strcmp(cookies[i].name, name) &&
                (domain_match(cookies[i].domain, c.domain) || domain_match(c.domain, cookies[i].domain)) && path_match(c.path, cookies[i].path))
                return;
        }
    }
    u64 now = time_now();
    int remove = 0;
    if (have_max_age) {
        if (max_age <= 0) remove = 1;
        else c.expires = now + (u64)max_age;
    } else if (have_expiry) {
        if (expires <= (i64)now) remove = 1;
        else c.expires = (u64)expires;
    }
    for (int i = 0; i < cookie_count; i++) {
        Cookie *o = &cookies[i];
        if (!strcmp(o->name, name) && !strcmp(o->domain, c.domain) && !strcmp(o->path, c.path)) {
            if (o->http_only && from_script) return; /* scripts may not touch HttpOnly cookies */
            if (remove) {
                cookie_remove(i);
                return;
            }
            break;
        }
    }
    if (!remove) cookie_store(&c, name, value);
}

/* The Cookie header for a request to `url` ("" if none), or what
   document.cookie shows (for_script: no HttpOnly ones). */
static void cookie_header(const char *url, int context, int for_script, char *out, int max) {
    cookies_load();
    out[0] = 0;
    int https;
    char host[256], path[1024];
    if (!url_host_path(url, &https, host, sizeof(host), path, sizeof(path))) return;
    cookies_expire();
    static int pick[MAX_COOKIES];
    int n = 0;
    for (int i = 0; i < cookie_count; i++) {
        Cookie *c = &cookies[i];
        if (c->host_only ? strcmp(host, c->domain) != 0 : !domain_match(host, c->domain)) continue;
        if (!path_match(path, c->path)) continue;
        if (c->secure && !https && strcmp(host, "localhost")) continue;
        if (for_script && c->http_only) continue;
        if (context == CTX_CROSS_SUB && c->same_site != SAME_NONE) continue;
        if (context == CTX_CROSS_TOP && c->same_site == SAME_STRICT) continue;
        pick[n++] = i;
    }
    /* longer paths first, then older first */
    for (int i = 1; i < n; i++) {
        int v = pick[i], j = i;
        while (j > 0) {
            Cookie *a = &cookies[pick[j - 1]], *b = &cookies[v];
            size_t la = strlen(a->path), lb = strlen(b->path);
            if (la > lb || (la == lb && a->serial <= b->serial)) break;
            pick[j] = pick[j - 1];
            j--;
        }
        pick[j] = v;
    }
    int len = 0;
    for (int i = 0; i < n; i++) {
        Cookie *c = &cookies[pick[i]];
        int need = (int)(strlen(c->name) + strlen(c->value)) + 3;
        if (len + need >= max) break;
        if (len) {
            memcpy(out + len, "; ", 2);
            len += 2;
        }
        if (c->name[0]) {
            strcpy(out + len, c->name);
            len += (int)strlen(c->name);
            out[len++] = '=';
        }
        strcpy(out + len, c->value);
        len += (int)strlen(c->value);
    }
    out[len] = 0;
}

/* Whether a request to `url`, made by the page at `from` (or a link
   followed from it, `navigation`), is same-site. */
static int cookie_context(const char *url, const char *from, int navigation, int unsafe_method) {
    int h1, h2;
    char a[256], b[256], pa[8], pb[8];
    if (!url_host_path(url, &h1, a, sizeof(a), pa, sizeof(pa))) return CTX_SAME_SITE;
    if (!from || !from[0] || (strncasecmp(from, "http://", 7) && strncasecmp(from, "https://", 8))) return navigation ? CTX_SAME_SITE : CTX_CROSS_SUB;
    if (!url_host_path(from, &h2, b, sizeof(b), pb, sizeof(pb))) return CTX_SAME_SITE;
    if (!strcmp(site_of(a), site_of(b))) return CTX_SAME_SITE;
    return navigation && !unsafe_method ? CTX_CROSS_TOP : CTX_CROSS_SUB;
}

/* ---- kept between runs: the cookies with an expiry date ---- */

static void cookies_load(void) {
    if (cookies_loaded) return;
    cookies_loaded = 1;
    size_t len;
    char *data = read_all(COOKIE_FILE, &len);
    if (!data) return;
    u64 now = time_now();
    for (char *line = data; line && *line && cookie_count < MAX_COOKIES;) {
        char *end = strchr(line, '\n');
        if (end) *end = 0;
        /* domain host_only path secure http_only same_site expires serial name value, tab-separated */
        char *f[10];
        int k = 0;
        for (char *p = line; k < 10;) {
            f[k++] = p;
            char *tab = strchr(p, '\t');
            if (!tab) break;
            *tab = 0;
            p = tab + 1;
        }
        if (k == 10) {
            Cookie c;
            memset(&c, 0, sizeof(c));
            strncpy(c.domain, f[0], sizeof(c.domain) - 1);
            c.host_only = f[1][0] == '1';
            strncpy(c.path, f[2], sizeof(c.path) - 1);
            c.secure = f[3][0] == '1';
            c.http_only = f[4][0] == '1';
            c.same_site = (unsigned char)(f[5][0] - '0') <= 2 ? (unsigned char)(f[5][0] - '0') : SAME_LAX;
            u64 e = 0, s = 0;
            for (char *p = f[6]; digit_c(*p); p++) e = e * 10 + (u64)(*p - '0');
            for (char *p = f[7]; digit_c(*p); p++) s = s * 10 + (u64)(*p - '0');
            c.expires = e;
            if (e > now && c.domain[0] && c.path[0] == '/' && strlen(f[8]) + strlen(f[9]) < MAX_COOKIE_BYTES) {
                size_t nl = strlen(f[8]), vl = strlen(f[9]);
                char *block = malloc(nl + vl + 2);
                if (block) {
                    memcpy(block, f[8], nl + 1);
                    memcpy(block + nl + 1, f[9], vl + 1);
                    c.name = block;
                    c.value = block + nl + 1;
                    c.serial = s;
                    if (s > cookie_serial) cookie_serial = s;
                    cookies[cookie_count++] = c;
                }
            }
        }
        line = end ? end + 1 : NULL;
    }
    free(data);
    cookies_dirty = 0;
}

/* Write the jar out if a lasting cookie changed (each save rewrites the
   disk's snapshot, so this is done once per page, not per cookie). */
static void cookies_save(void) {
    if (!cookies_dirty) return;
    cookies_dirty = 0;
    cookies_expire();
    cookies_dirty = 0;
    size_t cap = 4096, len = 0;
    for (int i = 0; i < cookie_count; i++) cap += strlen(cookies[i].name) + strlen(cookies[i].value) + 320;
    char *out = malloc(cap);
    if (!out) return;
    for (int i = 0; i < cookie_count; i++) {
        Cookie *c = &cookies[i];
        if (!c->expires) continue; /* (session cookies end with the browser) */
        char nums[64];
        int k = 0;
        char e[24], s[24];
        int ek = 0, sk = 0;
        u64 v = c->expires;
        do e[ek++] = (char)('0' + v % 10);
        while (v /= 10);
        v = c->serial;
        do s[sk++] = (char)('0' + v % 10);
        while (v /= 10);
        nums[k++] = '\t';
        nums[k++] = c->host_only ? '1' : '0';
        nums[k] = 0;
        len += (size_t)snprintf_lite(out + len, cap - len, c->domain);
        len += (size_t)snprintf_lite(out + len, cap - len, nums);
        out[len++] = '\t';
        len += (size_t)snprintf_lite(out + len, cap - len, c->path);
        out[len++] = '\t';
        out[len++] = c->secure ? '1' : '0';
        out[len++] = '\t';
        out[len++] = c->http_only ? '1' : '0';
        out[len++] = '\t';
        out[len++] = (char)('0' + c->same_site);
        out[len++] = '\t';
        while (ek) out[len++] = e[--ek];
        out[len++] = '\t';
        while (sk) out[len++] = s[--sk];
        out[len++] = '\t';
        len += (size_t)snprintf_lite(out + len, cap - len, c->name);
        out[len++] = '\t';
        len += (size_t)snprintf_lite(out + len, cap - len, c->value);
        out[len++] = '\n';
    }
    file_write(COOKIE_FILE, out, len);
    free(out);
}
