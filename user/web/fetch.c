/* web/fetch.c - getting pages, stylesheets and pictures: from files, or
   over HTTP and HTTPS, following redirects and unpacking compressed
   replies. Part of the browser. */

#define MAX_PAGE (8 * 1024 * 1024)

#pragma clang section bss=".rlmb$m" data=".rlmd$m"
static char current_url[1024];
#pragma clang section bss="" data=""

/* Sites the user chose to open although their identity could not be
   verified (for this run of the browser only). */
static char insecure_hosts[8][256];
static int insecure_count;

static int host_is_insecure(const char *host) {
    for (int i = 0; i < insecure_count; i++) {
        if (!strcasecmp_word(insecure_hosts[i], host)) return 1;
    }
    return 0;
}

static void allow_insecure(const char *url) {
    const char *p = strstr(url, "://");
    if (!p || insecure_count == 8) return;
    p += 3;
    int n = 0;
    while (p[n] && p[n] != '/' && p[n] != ':' && p[n] != '?' && n < 255) n++;
    memcpy(insecure_hosts[insecure_count], p, (size_t)n);
    insecure_hosts[insecure_count++][n] = 0;
}
#pragma clang section bss=".rlmb$m" data=".rlmd$m"
static char base_url[1024]; /* what relative addresses in the page are relative to */
#pragma clang section bss="" data=""

static void resolve_url_part(const char *base, const char *href, char *out, size_t max);

/* Make `href` (as found in a page at `base`) into a full address, keeping
   its #fragment (the place in the page it points to). */
static void resolve_url(const char *base, const char *href, char *out, size_t max) {
    const char *hash = strchr(href, '#');
    if (!hash || !strncasecmp(href, "javascript:", 11) || !strncasecmp(href, "data:", 5)) {
        resolve_url_part(base, href, out, max);
        return;
    }
    static char head[2048];
    size_t hl = (size_t)(hash - href) < sizeof(head) - 1 ? (size_t)(hash - href) : sizeof(head) - 1;
    memcpy(head, href, hl);
    head[hl] = 0;
    resolve_url_part(base, head, out, max);
    strncat_safe(out, hash, max - strlen(out) - 1);
}

/* The same, without any #fragment. */
static void resolve_url_part(const char *base, const char *href, char *out, size_t max) {
    while (is_space(*href)) href++;
    static char result[2048], joined[2048], dir[1024], tail[1024];
    int web = strncasecmp(base, "http://", 7) == 0 || strncasecmp(base, "https://", 8) == 0;
    if (strncasecmp(href, "http://", 7) == 0 || strncasecmp(href, "https://", 8) == 0) {
        strcpy(result, href);
    } else if (href[0] == '/' && href[1] == '/') {
        const char *colon = strchr(base, ':');
        size_t n = web && colon ? (size_t)(colon - base) + 1 : 5;
        memcpy(result, web ? base : "http:", n);
        strcpy(result + n, href);
    } else if (strchr(href, ':') && (!strchr(href, '/') || strchr(href, ':') < strchr(href, '/')) &&
               !(href[1] == ':' && is_alpha(href[0]))) {
        strcpy(result, href); /* mailto:, javascript:, data: */
    } else {
        const char *path = base;
        if (web) {
            path = strstr(base, "://") + 3;
            while (*path && *path != '/' && *path != '?' && *path != '#') path++;
        }
        size_t origin = (size_t)(path - base);
        memcpy(result, base, origin);
        result[origin] = 0;
        if (href[0] == '/') {
            dir[0] = 0;
        } else {
            size_t n = strlen(path), cut = 0;
            for (size_t i = 0; i < n && path[i] != '?' && path[i] != '#'; i++) {
                if (path[i] == '/') cut = i + 1;
            }
            if (cut == 0 && web) strcpy(dir, "/");
            else {
                memcpy(dir, path, cut);
                dir[cut] = 0;
            }
        }
        if (href[0] == '?') { /* a new query on the same path */
            strcpy(joined, path);
            char *q = strchr(joined, '?');
            if (q) *q = 0;
            char *h = strchr(joined, '#');
            if (h) *h = 0;
            strcpy(joined + strlen(joined), href);
        } else if (href[0] == '#' || href[0] == 0) {
            strcpy(joined, path);
        } else {
            strcpy(joined, dir);
            strcpy(joined + strlen(joined), href);
        }
        tail[0] = 0;
        for (char *p = joined; *p; p++) {
            if (*p == '?' || *p == '#') {
                strcpy(tail, p);
                *p = 0;
                break;
            }
        }
        char *parts[128];
        int count = 0;
        int absolute = joined[0] == '/';
        size_t jl = strlen(joined);
        int trailing = jl > 0 && joined[jl - 1] == '/';
        for (char *seg = joined; seg && *seg;) {
            while (*seg == '/') seg++;
            if (!*seg) break;
            char *slash = strchr(seg, '/');
            if (slash) *slash = 0;
            if (!strcmp(seg, "..")) {
                if (count) count--;
            } else if (strcmp(seg, ".") && count < 128) {
                parts[count++] = seg;
            }
            seg = slash ? slash + 1 : NULL;
        }
        size_t at = strlen(result);
        if (absolute || web) result[at++] = '/';
        for (int i = 0; i < count; i++) {
            size_t n = strlen(parts[i]);
            memcpy(result + at, parts[i], n);
            at += n;
            if (i < count - 1 || trailing) result[at++] = '/';
        }
        result[at] = 0;
        strcpy(result + at, tail);
    }
    char *hash = strchr(result, '#');
    if (hash) *hash = 0;
    size_t n = strlen(result);
    if (n >= max) n = max - 1;
    memcpy(out, result, n);
    out[n] = 0;
}

static int header_value(const char *headers, const char *name, char *out, int max) {
    size_t n = strlen(name);
    for (const char *line = headers; line && *line;) {
        if (strncasecmp(line, name, n) == 0 && line[n] == ':') {
            const char *v = line + n + 1;
            while (*v == ' ') v++;
            int k = 0;
            while (*v && *v != '\r' && *v != '\n' && k < max - 1) out[k++] = *v++;
            out[k] = 0;
            return 1;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return 0;
}

static size_t unchunk(char *body, size_t len) {
    size_t in = 0, out = 0;
    while (in < len) {
        size_t size = 0;
        while (in < len && body[in] != '\r' && body[in] != '\n' && body[in] != ';') {
            int d = hex_digit(body[in++]);
            if (d >= 0) size = size * 16 + (size_t)d;
        }
        while (in < len && body[in] != '\n') in++;
        in++;
        if (size == 0 || in >= len) break;
        if (size > len - in) size = len - in;
        memmove(body + out, body + in, size);
        out += size;
        in += size + 2;
    }
    return out;
}

/* ---- connections, kept open between requests to the same server ----
   Opening a secure connection costs several round trips and a lot of
   arithmetic; a page's dozens of scripts, stylesheets and pictures mostly
   come from a few servers, so connections are reused (HTTP/1.1 keep-alive). */

typedef struct {
    int fd;
    tls_session *tls;
    char host[256];
    int port, https, no_verify;
    u64 idle_since;
    char security[256]; /* how it travels (for the status bar) */
} Conn;

/* fetch_request may run on several threads at once (fetching a page's
   scripts in parallel): this lock covers the cookie jar, the connection
   pool and the address helpers' buffers. It is let go while waiting on
   the network. */
static mutex_t fetch_lock;

static int stat_conn_new, stat_conn_reused;
static u64 stat_connect_ms, stat_wait_ms, stat_unpack_ms;

#define POOL 6
#define POOL_IDLE_MS 30000
static Conn conn_pool[POOL];
static int conn_pool_used[POOL];

static int conn_write(Conn *c, const void *data, size_t len) {
    return c->tls ? tls_write(c->tls, data, len) : (fd_write(c->fd, data, len) == (i64)len ? 0 : -1);
}

static i64 conn_read(Conn *c, void *buf, size_t max) {
    if (c->tls) return tls_read(c->tls, buf, max, 15000);
    for (;;) {
        pollfd w = {c->fd, POLLIN, 0};
        int ready = poll(&w, 1, 15000);
        if (ready == EINTR) continue;
        if (ready <= 0) return -1;
        return read(c->fd, buf, max);
    }
}

static void conn_close(Conn *c) {
    if (c->tls) tls_close(c->tls);
    close(c->fd);
    c->tls = NULL;
    c->fd = -1;
}

/* An idle connection to host:port from the pool, if there is one (fetch_lock held). */
static int conn_take(Conn *c, const char *host, int port, int https, int fresh) {
    int no_verify = https && host_is_insecure(host);
    u64 now = uptime_ms();
    for (int i = 0; i < POOL; i++) {
        if (!conn_pool_used[i]) continue;
        if (now - conn_pool[i].idle_since > POOL_IDLE_MS) { /* the server has likely given up on it */
            conn_close(&conn_pool[i]);
            conn_pool_used[i] = 0;
            continue;
        }
        if (!fresh && conn_pool[i].port == port && conn_pool[i].https == https && conn_pool[i].no_verify == no_verify && same_text_nocase(conn_pool[i].host, host)) {
            *c = conn_pool[i];
            conn_pool_used[i] = 0;
            stat_conn_reused++;
            return 1;
        }
    }
    return 0;
}

/* A new connection to host:port (fetch_lock not held: this waits on the network). */
static int conn_connect(Conn *c, const char *host, int port, int https, int no_verify, char *error) {
    memset(c, 0, sizeof(*c));
    stat_conn_new++;
    u64 connect_t0 = uptime_ms();
    unsigned char ip[4];
    if (net_resolve(host, ip) != 0) {
        strcpy(error, "Could not find the address of ");
        strncat_safe(error, host, 100);
        strncat_safe(error, " (is the network up? see: net)", 40);
        return -1;
    }
    c->fd = tcp_connect(ip, port);
    if (c->fd < 0) {
        strcpy(error, "Could not connect to ");
        strncat_safe(error, host, 100);
        return -1;
    }
    if (https) {
        char why[160];
        c->tls = tls_connect_options(c->fd, host, no_verify ? TLS_NO_VERIFY : 0, why);
        if (!c->tls) {
            close(c->fd);
            strcpy(error, "The secure (https) connection to ");
            strncat_safe(error, host, 80);
            strncat_safe(error, " failed: ", 10);
            strncat_safe(error, why, 160);
            return -1;
        }
        tls_describe(c->tls, c->security, sizeof(c->security));
    } else {
        strcpy(c->security, "not encrypted (http)");
    }
    stat_connect_ms += uptime_ms() - connect_t0;
    strncpy(c->host, host, sizeof(c->host) - 1);
    c->port = port;
    c->https = https;
    c->no_verify = no_verify;
    return 0;
}

static void request_buffers_free(char *request) { free(request); }

/* Done with a connection that may be used again: keep it. */
static void conn_keep(Conn *c) {
    int slot = -1;
    for (int i = 0; i < POOL && slot < 0; i++) {
        if (!conn_pool_used[i]) slot = i;
    }
    if (slot < 0) { /* full: the one idle longest goes */
        slot = 0;
        for (int i = 1; i < POOL; i++) {
            if (conn_pool[i].idle_since < conn_pool[slot].idle_since) slot = i;
        }
        conn_close(&conn_pool[slot]);
    }
    c->idle_since = uptime_ms();
    conn_pool[slot] = *c;
    conn_pool_used[slot] = 1;
}

/* Has a chunked body (from `body`, `len` bytes so far) ended? *at is where
   the next chunk starts, kept between calls so nothing is read twice. */
static int chunked_complete(const char *b, size_t len, size_t *at) {
    size_t p = *at;
    for (;;) {
        size_t e = p;
        while (e + 1 < len && !(b[e] == '\r' && b[e + 1] == '\n')) e++;
        if (e + 1 >= len) return 0;
        size_t size = 0;
        for (size_t i = p; i < e; i++) {
            int d = hex_digit(b[i]);
            if (d < 0) break; /* (a chunk extension follows) */
            size = size * 16 + (size_t)d;
            if (size > (size_t)MAX_PAGE * 4) return 1; /* nonsense: stop here */
        }
        size_t data = e + 2;
        if (size == 0) { /* the last chunk: then trailer lines, up to an empty one */
            size_t q = data;
            for (;;) {
                if (q + 1 >= len) return 0;
                if (b[q] == '\r' && b[q + 1] == '\n') return 1;
                while (q + 1 < len && !(b[q] == '\r' && b[q + 1] == '\n')) q++;
                if (q + 1 >= len) return 0;
                q += 2;
            }
        }
        if (data + size + 2 > len) return 0;
        p = data + size + 2;
        *at = p;
    }
}

/* Read one reply. Returns 0 with the reply (malloc'd, NUL-terminated) and
   where its body starts; *keep says whether the connection can be used
   again. -1: the connection was closed before anything came (a kept
   connection the server had given up on), -2: a bad or incomplete reply,
   -3: out of memory. */
static int read_reply(Conn *c, int head_only, char **out, size_t *out_len, size_t *out_header_end, int *keep) {
    size_t len = 0, cap = 65536, header_end = 0, chunk_at = 0;
    char *reply = malloc(cap + 1);
    i64 expected = -1;
    int chunked = 0, until_close = 0, done = 0, closed = 0;
    *keep = 0;
    if (!reply) return -3;
    while (!done) {
        if (len + 16384 > cap) {
            if (cap >= MAX_PAGE) break; /* too big: what came is shown */
            cap *= 2;
            char *bigger = realloc(reply, cap + 1);
            if (!bigger) break;
            reply = bigger;
        }
        i64 got = conn_read(c, reply + len, 16384);
        if (got <= 0) {
            closed = 1;
            break;
        }
        len += (size_t)got;
        reply[len] = 0;
        while (!header_end) {
            char *blank = strstr(reply, "\r\n\r\n");
            if (!blank) break;
            size_t end = (size_t)(blank - reply) + 4;
            int status = atoi(strchr(reply, ' ') ? strchr(reply, ' ') + 1 : "0");
            if (status >= 100 && status < 200) { /* "100 Continue" and the like: the real reply follows */
                memmove(reply, reply + end, len - end + 1);
                len -= end;
                continue;
            }
            header_end = end;
            char value[64];
            int http11 = !strncmp(reply, "HTTP/1.1", 8);
            *keep = http11;
            /* (headers are searched only within the header part) */
            char saved = reply[header_end - 2];
            reply[header_end - 2] = 0;
            if (header_value(reply, "Connection", value, sizeof(value))) {
                if (strstr(value, "close") || strstr(value, "Close")) *keep = 0;
                else if (strstr(value, "keep-alive") || strstr(value, "Keep-Alive")) *keep = 1;
            }
            if (head_only || status == 204 || status == 304) expected = 0;
            else if (header_value(reply, "Transfer-Encoding", value, sizeof(value)) && strstr(value, "chunked")) chunked = 1;
            else if (header_value(reply, "Content-Length", value, sizeof(value))) expected = atoi(value);
            else until_close = 1;
            reply[header_end - 2] = saved;
            if (until_close) *keep = 0;
        }
        if (header_end) {
            if (expected >= 0) done = len >= header_end + (size_t)expected;
            else if (chunked) done = chunked_complete(reply + header_end, len - header_end, &chunk_at);
        }
    }
    if (!header_end) {
        free(reply);
        return len == 0 && closed ? -1 : -2;
    }
    if (!done) *keep = 0; /* (cut short, or read until the server closed it) */
    if (expected >= 0 && len > header_end + (size_t)expected) len = header_end + (size_t)expected;
    reply[len] = 0;
    *out = reply;
    *out_len = len;
    *out_header_end = header_end;
    return 0;
}

typedef struct {
    char *body;           /* malloc'd, NUL-terminated */
    size_t len;
    char type[96];        /* Content-Type */
    char security[256];   /* how it travelled */
    char refresh[512];    /* a Refresh header: "5; url=..." */
    int status;
} Fetched;

/* What to ask for. */
typedef struct {
    const char *method;       /* NULL: GET, or POST when there is a body */
    const char *body;
    size_t body_len;
    const char *content_type; /* of the body */
    const char *headers;      /* more request headers, each ending \r\n (from scripts) */
    int navigation;           /* opening a page (rather than something a page uses) */
} Request;

#pragma clang section bss=".rlmb$m" data=".rlmd$m"
static int fetch_navigating; /* fetch_url is opening a page */
#pragma clang section bss="" data=""

/* The Referer to send, from the page at `from` to `to`: the whole address
   to the same origin, only the origin to another, nothing from a secure
   page to an insecure one (strict-origin-when-cross-origin). */
static void referrer_for(const char *from, const char *to, char *out, int max) {
    out[0] = 0;
    int from_https = !strncasecmp(from, "https://", 8);
    if (!from_https && strncasecmp(from, "http://", 7)) return;
    if (from_https && strncasecmp(to, "https://", 8)) return;
    const char *p = from + (from_https ? 8 : 7);
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    size_t origin = (size_t)(p - from);
    int same = strlen(to) >= origin && !strncasecmp(from, to, origin) && (to[origin] == 0 || to[origin] == '/' || to[origin] == '?');
    size_t n = same ? strcspn(from, "#") : origin;
    if ((int)n > max - 2) n = (size_t)max - 2;
    memcpy(out, from, n);
    out[n] = 0;
    if (!same || n == origin) strcat(out, "/");
}

static void add_number(char *s, size_t v) {
    char digits[24];
    int k = 0;
    do digits[k++] = (char)('0' + v % 10);
    while (v /= 10);
    size_t at = strlen(s);
    while (k) s[at++] = digits[--k];
    s[at] = 0;
}

/* Get `url` (updated to where redirects lead). Returns 0, or -1 with the
   reason in `error`. */
static int fetch_request_locked(char *url, const Request *rq, Fetched *f, char *error, char *request, char *cookie, char *referrer, char *path);

static int fetch_request(char *url, const Request *rq, Fetched *f, char *error) {
    /* (big buffers on the heap: threads have small stacks) */
    char *request = malloc(16384 + 8192 + 1100 + 2048);
    if (!request) {
        memset(f, 0, sizeof(*f));
        strcpy(error, "Not enough memory for that page.");
        return -1;
    }
    mutex_lock(&fetch_lock);
    int r = fetch_request_locked(url, rq, f, error, request, request + 16384, request + 16384 + 8192, request + 16384 + 8192 + 1100);
    mutex_unlock(&fetch_lock);
    request_buffers_free(request);
    return r;
}

/* blob: addresses (URL.createObjectURL): what a page's script made, kept
   until it is revoked or the page goes. */
typedef struct {
    char *url, *data;
    size_t len;
    char type[96];
    int owner; /* the realm that made it (web/realm.c) */
} BlobEntry;
static int fetch_realm_id = 1; /* the realm running (kept up to date by realm.c) */
static BlobEntry *blob_store;
static int blob_count, blob_cap;

static void blob_put(const char *url, const char *data, size_t len, const char *type) {
    mutex_lock(&fetch_lock);
    if (blob_count == blob_cap) {
        int cap = blob_cap ? blob_cap * 2 : 8;
        BlobEntry *p = realloc(blob_store, sizeof(BlobEntry) * (size_t)cap);
        if (!p) {
            mutex_unlock(&fetch_lock);
            return;
        }
        blob_store = p;
        blob_cap = cap;
    }
    BlobEntry *b = &blob_store[blob_count];
    b->url = malloc(strlen(url) + 1);
    b->data = malloc(len + 1);
    if (b->url && b->data) {
        strcpy(b->url, url);
        memcpy(b->data, data, len);
        b->data[len] = 0;
        b->len = len;
        b->type[0] = 0;
        strncat_safe(b->type, type, sizeof(b->type) - 1);
        b->owner = fetch_realm_id;
        blob_count++;
    } else {
        free(b->url);
        free(b->data);
    }
    mutex_unlock(&fetch_lock);
}

static void blob_drop(int i) {
    free(blob_store[i].url);
    free(blob_store[i].data);
    blob_store[i] = blob_store[--blob_count];
}

static void blob_revoke(const char *url) {
    mutex_lock(&fetch_lock);
    for (int i = 0; i < blob_count; i++) {
        if (!strcmp(blob_store[i].url, url)) {
            blob_drop(i);
            break;
        }
    }
    mutex_unlock(&fetch_lock);
}

/* the running realm's page is going: its blob: addresses go too */
static void blobs_clear(void) {
    mutex_lock(&fetch_lock);
    for (int i = 0; i < blob_count;) {
        if (blob_store[i].owner == fetch_realm_id) blob_drop(i);
        else i++;
    }
    mutex_unlock(&fetch_lock);
}

static int fetch_request_locked(char *url, const Request *rq, Fetched *f, char *error, char *request, char *cookie, char *referrer, char *path) {
    memset(f, 0, sizeof(*f));
    if (!strncasecmp(url, "blob:", 5)) {
        char *fragment = strchr(url, '#');
        size_t n = fragment ? (size_t)(fragment - url) : strlen(url);
        for (int i = 0; i < blob_count; i++) {
            if (strlen(blob_store[i].url) != n || strncmp(blob_store[i].url, url, n)) continue;
            f->body = malloc(blob_store[i].len + 1);
            if (!f->body) {
                strcpy(error, "Not enough memory.");
                return -1;
            }
            memcpy(f->body, blob_store[i].data, blob_store[i].len + 1);
            f->len = blob_store[i].len;
            strcpy(f->type, blob_store[i].type[0] ? blob_store[i].type : "application/octet-stream");
            strcpy(f->security, "made by the page");
            f->status = 200;
            return 0;
        }
        strcpy(error, "That blob: address is gone (revoked, or from another page).");
        return -1;
    }
    char *fragment = strchr(url, '#'); /* (a place in the page: not part of the request) */
    if (fragment && strncasecmp(url, "javascript:", 11)) *fragment = 0;
    const char *method = rq->method ? rq->method : rq->body ? "POST" : "GET";
    const char *body = rq->body;
    size_t body_len = rq->body ? rq->body_len : 0;
    for (int hops = 0; hops < 12; hops++) {
        int https = strncasecmp(url, "https://", 8) == 0;
        if (!https && strncasecmp(url, "http://", 7) != 0) {
            const char *colon = strchr(url, ':'), *slash = strchr(url, '/');
            if (strncasecmp(url, "data:", 5) == 0) {
                strcpy(error, "data: addresses are not supported");
                return -1;
            }
            if (colon && (!slash || colon < slash)) {
                strcpy(error, "That kind of address can't be opened here.");
                return -1;
            }
            /* a file's name ends where a ?query or #fragment begins */
            char path[1024];
            int pn = 0;
            while (url[pn] && url[pn] != '?' && url[pn] != '#' && pn < 1023) path[pn] = url[pn], pn++;
            path[pn] = 0;
            char *data = read_all(path, &f->len);
            if (!data) {
                strcpy(error, "There is no file called ");
                strncat_safe(error, path, 200);
                return -1;
            }
            f->body = data;
            size_t n = strlen(path);
            if ((n > 5 && !strncasecmp(path + n - 5, ".html", 5)) || (n > 4 && !strncasecmp(path + n - 4, ".htm", 4))) strcpy(f->type, "text/html");
            else if (n > 4 && !strncasecmp(path + n - 4, ".css", 4)) strcpy(f->type, "text/css");
            else if (image_sniff((unsigned char *)data, f->len)) strcpy(f->type, "image/any");
            else strcpy(f->type, "text/plain");
            strcpy(f->security, "a file on this machine");
            f->status = 200;
            return 0;
        }
        char host[256];
        const char *p = url + (https ? 8 : 7);
        int n = 0, port = https ? 443 : 80;
        while (*p && *p != '/' && *p != ':' && *p != '?' && n < 255) host[n++] = *p++;
        host[n] = 0;
        if (*p == ':') {
            port = atoi(p + 1);
            while (*p && *p != '/' && *p != '?') p++;
        }
        strcpy(path, "/");
        if (*p == '/') strncpy(path, p, 2047);
        else if (*p == '?') strncpy(path + 1, p, 2046);
        path[2047] = 0;
        int unsafe = strcmp(method, "GET") && strcmp(method, "HEAD");

        /* the request */
        strcpy(request, method);
        strcat(request, " ");
        { /* bytes outside ASCII (and spaces) %-encoded, as browsers send them */
            static char sent[2048];
            int k = 0;
            for (const unsigned char *q = (const unsigned char *)path; *q && k < 2040; q++) {
                if (*q >= 0x80 || *q == ' ') {
                    static const char hex[] = "0123456789ABCDEF";
                    sent[k++] = '%';
                    sent[k++] = hex[*q >> 4];
                    sent[k++] = hex[*q & 15];
                } else sent[k++] = (char)*q;
            }
            sent[k] = 0;
            strncat_safe(request, sent, 2040);
        }
        strcat(request, " HTTP/1.1\r\nHost: ");
        strncat_safe(request, host, 255);
        if (port != (https ? 443 : 80)) {
            strcat(request, ":");
            add_number(request, (size_t)port);
        }
        strcat(request, "\r\nUser-Agent: Mozilla/5.0 (RDX; x86_64) RDX-Browser/0.4\r\n"
                        "Accept: text/html,application/xhtml+xml,text/css,image/png,image/jpeg,image/gif,image/svg+xml,text/plain;q=0.9,*/*;q=0.5\r\n"
                        "Accept-Encoding: gzip, deflate\r\nAccept-Language: en\r\nConnection: keep-alive\r\n");
        referrer_for(current_url, url, referrer, 1100);
        if (referrer[0]) {
            strcat(request, "Referer: ");
            strcat(request, referrer);
            strcat(request, "\r\n");
        }
        if (unsafe) { /* where the request comes from: sites check this against forged forms */
            const char *o = current_url;
            int on = 0;
            if (!strncasecmp(o, "http://", 7) || !strncasecmp(o, "https://", 8)) {
                const char *q = strstr(o, "://") + 3;
                while (*q && *q != '/' && *q != '?' && *q != '#') q++;
                on = (int)(q - o);
            }
            strcat(request, "Origin: ");
            if (on && on < 290) strncat_safe(request, o, (size_t)on);
            else strcat(request, "null");
            strcat(request, "\r\n");
        }
        int context = cookie_context(url, current_url, rq->navigation, unsafe);
        cookie_header(url, context, 0, cookie, 8192);
        if (cookie[0]) {
            strcat(request, "Cookie: ");
            strcat(request, cookie);
            strcat(request, "\r\n");
        }
        if (body) {
            strcat(request, "Content-Type: ");
            strncat_safe(request, rq->content_type ? rq->content_type : "application/x-www-form-urlencoded", 200);
            strcat(request, "\r\n");
        }
        if (body || unsafe) {
            strcat(request, "Content-Length: ");
            add_number(request, body_len);
            strcat(request, "\r\n");
        }
        if (rq->headers && strlen(request) + strlen(rq->headers) < 16384 - 8) strcat(request, rq->headers);
        strcat(request, "\r\n");

        /* send it, and read the reply: on a kept connection the server may
           have closed meanwhile, and then it is tried once on a new one */
        Conn c;
        char *reply = NULL;
        size_t len = 0, header_end = 0;
        int keep = 0, got = -2;
        for (int attempt = 0; attempt < 2; attempt++) {
            int reused = conn_take(&c, host, port, https, attempt > 0);
            mutex_unlock(&fetch_lock); /* (the network part: other threads may go on meanwhile) */
            int opened = reused || conn_connect(&c, host, port, https, https && host_is_insecure(host), error) == 0;
            u64 wait_t0 = uptime_ms();
            if (opened) {
                int sent = conn_write(&c, request, strlen(request)) == 0 && (!body_len || conn_write(&c, body, body_len) == 0);
                got = sent ? read_reply(&c, !strcmp(method, "HEAD"), &reply, &len, &header_end, &keep) : -1;
                if (got != 0 && reused && got != -3) conn_close(&c); /* (stale: try a new connection) */
            }
            mutex_lock(&fetch_lock);
            stat_wait_ms += uptime_ms() - wait_t0;
            if (!opened) return -1;
            if (got == 0 || !reused || got == -3) break;
        }
        if (got != 0) {
            if (c.fd >= 0) conn_close(&c);
            strcpy(error, got == -3 ? "Not enough memory for that page." : got == -1 ? "The connection was lost before the server answered." : "The server did not send a proper answer.");
            return -1;
        }
        strcpy(f->security, c.security);
        if (keep) conn_keep(&c);
        else conn_close(&c);
        if (memcmp(reply, "HTTP/", 5) != 0) {
            free(reply);
            strcpy(error, "The server did not send a proper answer.");
            return -1;
        }
        reply[header_end - 2] = 0;
        f->status = atoi(strchr(reply, ' ') ? strchr(reply, ' ') + 1 : "0");
        /* cookies the server sets: also on redirects (logins often redirect) */
        for (char *line = reply; line && *line;) {
            if (!strncasecmp(line, "Set-Cookie:", 11)) {
                char *v = line + 11, *e = strstr(v, "\r\n");
                char saved = e ? *e : 0;
                if (e) *e = 0;
                cookie_set(url, v, 0);
                if (e) *e = saved;
            }
            line = strchr(line, '\n');
            if (line) line++;
        }
        char location[1024];
        if (f->status >= 300 && f->status < 400 && f->status != 304 && header_value(reply, "Location", location, sizeof(location))) {
            char next[1024];
            resolve_url(url, location, next, sizeof(next));
            /* the fragment of the original address carries over when the new one has none */
            strcpy(url, next);
            free(reply);
            /* 307 and 308 repeat the same request; the others become a GET */
            if (f->status != 307 && f->status != 308 && strcmp(method, "HEAD")) {
                method = "GET";
                body = NULL;
                body_len = 0;
            }
            continue;
        }
        char encoding[32] = "", content_encoding[32] = "";
        strcpy(f->type, "text/html");
        header_value(reply, "Content-Type", f->type, sizeof(f->type));
        header_value(reply, "Transfer-Encoding", encoding, sizeof(encoding));
        header_value(reply, "Content-Encoding", content_encoding, sizeof(content_encoding));
        header_value(reply, "Refresh", f->refresh, sizeof(f->refresh));
        char *content = reply + header_end;
        size_t content_len = len - header_end;
        if (!strncasecmp(encoding, "chunked", 7)) content_len = unchunk(content, content_len);
        unsigned char *unpacked = NULL;
        size_t unpacked_len = 0;
        u64 unpack_t0 = uptime_ms();
        if (!strncasecmp(content_encoding, "gzip", 4) || !strncasecmp(content_encoding, "x-gzip", 6)) {
            if (gzip_decompress((unsigned char *)content, content_len, &unpacked, &unpacked_len, MAX_PAGE * 4) != 0) {
                free(reply);
                strcpy(error, "The page arrived compressed and could not be unpacked.");
                return -1;
            }
        } else if (!strncasecmp(content_encoding, "deflate", 7)) {
            if (zlib_decompress((unsigned char *)content, content_len, &unpacked, &unpacked_len, MAX_PAGE * 4) != 0 &&
                inflate_raw((unsigned char *)content, content_len, &unpacked, &unpacked_len, MAX_PAGE * 4) != 0) {
                free(reply);
                strcpy(error, "The page arrived compressed and could not be unpacked.");
                return -1;
            }
        } else if (content_encoding[0] && strncasecmp(content_encoding, "identity", 8)) {
            free(reply);
            strcpy(error, "The page arrived in a compressed form this browser cannot unpack (");
            strncat_safe(error, content_encoding, 30);
            strncat_safe(error, ").", 3);
            return -1;
        }
        stat_unpack_ms += uptime_ms() - unpack_t0;
        if (unpacked) {
            free(reply);
            f->body = (char *)unpacked;
            f->len = unpacked_len;
            return 0;
        }
        f->body = malloc(content_len + 1);
        if (!f->body) {
            free(reply);
            strcpy(error, "Not enough memory for that page.");
            return -1;
        }
        memcpy(f->body, content, content_len);
        f->body[content_len] = 0;
        f->len = content_len;
        free(reply);
        return 0;
    }
    strcpy(error, "The site redirected too many times.");
    return -1;
}

/* Get `url` (updated to where redirects lead). `post` is a form body to
   POST, or NULL for GET. Returns 0, or -1 with the reason in `error`. */
static int fetch_url(char *url, const char *post, Fetched *f, char *error) {
    Request rq;
    memset(&rq, 0, sizeof(rq));
    if (post) {
        rq.method = "POST";
        rq.body = post;
        rq.body_len = strlen(post);
        rq.content_type = "application/x-www-form-urlencoded";
    }
    rq.navigation = fetch_navigating;
    return fetch_request(url, &rq, f, error);
}

/* ---- fetching several things at once ----
   Each request mostly waits for the server: done one after another, a page
   with a hundred scripts or pictures takes a hundred round trips. A set
   fetches its addresses on a few threads; the caller takes them in its own
   order (waiting for any not there yet). */

#define PREFETCH_THREADS 6
#define PREFETCH_AHEAD_BYTES (8u << 20) /* fetched but not yet taken: no more is started beyond this */

typedef struct {
    char url[2048];
    Fetched f;
    int state; /* 0 waiting, 1 being fetched, 2 done, 3 failed, 4 taken */
} Prefetch;

static int prefetch_trace; /* (BROWSER_TRACE, read by the main thread: getenv is not for threads) */

typedef struct {
    Prefetch *items;
    int n, cap, next;
    size_t ready_bytes; /* fetched, not taken yet */
    mutex_t lock;
    int tids[PREFETCH_THREADS], threads;
} PrefetchSet;

static PrefetchSet *prefetch_begin(int capacity) {
    PrefetchSet *s = malloc(sizeof(PrefetchSet));
    if (!s) return NULL;
    memset(s, 0, sizeof(*s));
    s->items = malloc(sizeof(Prefetch) * (size_t)(capacity > 0 ? capacity : 1));
    if (!s->items) {
        free(s);
        return NULL;
    }
    s->cap = capacity;
    return s;
}

static void prefetch_add(PrefetchSet *s, const char *url) {
    if (!s || s->n == s->cap || strlen(url) >= sizeof(s->items[0].url)) return;
    if (strncasecmp(url, "http://", 7) && strncasecmp(url, "https://", 8)) return; /* (files are quick anyway) */
    for (int i = 0; i < s->n; i++) {
        if (!strcmp(s->items[i].url, url)) return;
    }
    Prefetch *p = &s->items[s->n++];
    memset(p, 0, sizeof(*p));
    strcpy(p->url, url);
}

static void prefetch_worker(void *arg) {
    PrefetchSet *s = arg;
    char *url = malloc(2048 + 400);
    for (;;) {
        mutex_lock(&s->lock);
        int ahead = s->ready_bytes > PREFETCH_AHEAD_BYTES && s->next < s->n;
        int i = !ahead && s->next < s->n ? s->next++ : -1;
        if (i >= 0) s->items[i].state = 1;
        mutex_unlock(&s->lock);
        if (ahead) { /* far enough ahead of what is being used: wait */
            sleep_ms(5);
            continue;
        }
        if (i < 0) break;
        Fetched got;
        memset(&got, 0, sizeof(got));
        int ok = 0;
        if (url) {
            strcpy(url, s->items[i].url);
            Request rq;
            memset(&rq, 0, sizeof(rq));
            ok = fetch_request(url, &rq, &got, url + 2048) == 0;
            if (!ok && prefetch_trace) printf("[fetch] in the background, %s failed: %s\n", s->items[i].url, url + 2048);
        }
        mutex_lock(&s->lock);
        s->items[i].f = got;
        s->items[i].state = ok ? 2 : 3;
        if (ok) s->ready_bytes += got.len;
        mutex_unlock(&s->lock);
    }
    free(url);
    thread_exit();
}

static void prefetch_go(PrefetchSet *s) {
    if (!s || s->n < 2) return;
    tls_prepare();
    prefetch_trace = getenv("BROWSER_TRACE") != NULL;
    int threads = s->n < PREFETCH_THREADS ? s->n : PREFETCH_THREADS;
    for (int i = 0; i < threads; i++) {
        int tid = thread_create(prefetch_worker, s);
        if (tid >= 0) s->tids[s->threads++] = tid;
    }
    if (!s->threads) s->n = 0; /* (no threads: everything is fetched as it is needed) */
}

/* The fetched copy of `url`, waiting for it if it is still coming. 1 with
   it in *f (the caller frees the body), 0 if the set does not have it (or
   is not fetching), -1 if fetching it failed. */
static int prefetch_take(PrefetchSet *s, const char *url, Fetched *f) {
    if (!s || !s->threads) return 0;
    char want[2048];
    int k = 0;
    while (url[k] && url[k] != '#' && k < 2047) want[k] = url[k], k++; /* (fetching drops the fragment) */
    want[k] = 0;
    for (int i = 0; i < s->n; i++) {
        if (strcmp(s->items[i].url, want) && strcmp(s->items[i].url, url)) continue;
        for (;;) {
            mutex_lock(&s->lock);
            int state = s->items[i].state;
            if (state == 2 || state == 3) {
                *f = s->items[i].f;
                s->items[i].state = 4;
                if (state == 2) s->ready_bytes -= f->len < s->ready_bytes ? f->len : s->ready_bytes;
            }
            mutex_unlock(&s->lock);
            if (state == 2) return 1;
            if (state == 3) return -1;
            if (state == 4) return 0; /* (already taken: fetch again) */
            sleep_ms(2);
        }
    }
    return 0;
}

/* Done: what is still being fetched finishes; what was not taken is thrown away. */
static void prefetch_end(PrefetchSet *s) {
    if (!s) return;
    mutex_lock(&s->lock);
    s->next = s->n; /* nothing more is started */
    mutex_unlock(&s->lock);
    for (int i = 0; i < s->threads; i++) thread_join(s->tids[i]);
    for (int i = 0; i < s->n; i++) {
        if (s->items[i].state == 2) free(s->items[i].f.body);
    }
    free(s->items);
    free(s);
}

/* ---- <meta http-equiv=refresh> and the Refresh header: go to another
   address (or reload) after some seconds ---- */

#pragma clang section bss=".rlmb$m" data=".rlmd$m"
static char refresh_url[1024];
static u64 refresh_due; /* uptime; 0: none */
#pragma clang section bss="" data=""

/* "5", "0; url=/next", "3;URL='page.html'" */
static void schedule_refresh(const char *value, const char *base) {
    const char *p = value;
    while (*p == ' ') p++;
    if (!(*p >= '0' && *p <= '9') && *p != '.' && *p != ';' && *p != ',') return;
    long seconds = 0;
    while (*p >= '0' && *p <= '9') seconds = seconds * 10 + (*p++ - '0');
    while (*p && *p != ';' && *p != ',') p++; /* (fractions) */
    while (*p == ';' || *p == ',' || *p == ' ') p++;
    if (!strncasecmp(p, "url", 3)) {
        const char *q = p + 3;
        while (*q == ' ') q++;
        if (*q == '=') p = q + 1;
    }
    while (*p == ' ') p++;
    char target[1024];
    int n = 0;
    char quote = *p == '\'' || *p == '"' ? *p++ : 0;
    while (*p && n < 1023 && (quote ? *p != quote : 1)) target[n++] = *p++;
    while (n > 0 && target[n - 1] == ' ') n--;
    target[n] = 0;
    if (!target[0]) strcpy(target, base);
    resolve_url(base, target, refresh_url, sizeof(refresh_url));
    if (seconds > 3600) seconds = 3600;
    refresh_due = uptime_ms() + (u64)seconds * 1000 + 1;
}
