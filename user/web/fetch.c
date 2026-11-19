/* web/fetch.c - getting pages, stylesheets and pictures: from files, or
   over HTTP and HTTPS, following redirects and unpacking compressed
   replies. Part of the browser. */

#define MAX_PAGE (8 * 1024 * 1024)

static char current_url[1024];

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
static char base_url[1024]; /* what relative addresses in the page are relative to */

/* Make `href` (as found in a page at `base`) into a full address. */
static void resolve_url(const char *base, const char *href, char *out, size_t max) {
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

typedef struct {
    int fd;
    tls_session *tls;
} Conn;

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
}

typedef struct {
    char *body;           /* malloc'd, NUL-terminated */
    size_t len;
    char type[96];        /* Content-Type */
    char security[256];   /* how it travelled */
    int status;
} Fetched;

/* Get `url` (updated to where redirects lead). `post` is a form body to
   POST, or NULL for GET. Returns 0, or -1 with the reason in `error`. */
static int fetch_url(char *url, const char *post, Fetched *f, char *error) {
    memset(f, 0, sizeof(*f));
    for (int hops = 0; hops < 8; hops++) {
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
        static char path[2048];
        strcpy(path, "/");
        if (*p == '/') strcpy(path, p);
        else if (*p == '?') strcpy(path + 1, p);
        unsigned char ip[4];
        if (net_resolve(host, ip) != 0) {
            strcpy(error, "Could not find the address of ");
            strncat_safe(error, host, 100);
            strncat_safe(error, " (is the network up? see: net)", 40);
            return -1;
        }
        Conn c = {tcp_connect(ip, port), NULL};
        if (c.fd < 0) {
            strcpy(error, "Could not connect to ");
            strncat_safe(error, host, 100);
            return -1;
        }
        if (https) {
            char why[160];
            c.tls = tls_connect_options(c.fd, host, host_is_insecure(host) ? TLS_NO_VERIFY : 0, why);
            if (!c.tls) {
                close(c.fd);
                strcpy(error, "The secure (https) connection to ");
                strncat_safe(error, host, 80);
                strncat_safe(error, " failed: ", 10);
                strncat_safe(error, why, 160);
                return -1;
            }
        }
        static char request[4096];
        request[0] = 0;
        strcpy(request, post && hops == 0 ? "POST " : "GET ");
        strncat_safe(request, path, 2000);
        strncat_safe(request, " HTTP/1.0\r\nHost: ", 40);
        strncat_safe(request, host, 255);
        strncat_safe(request,
                     "\r\nUser-Agent: Mozilla/5.0 (RDX; x86_64) RDX-Browser/0.3\r\n"
                     "Accept: text/html, text/css, image/png, image/jpeg, image/gif, text/plain;q=0.9, */*;q=0.5\r\n"
                     "Accept-Encoding: gzip, deflate\r\nAccept-Language: en\r\nConnection: close\r\n",
                     400);
        if (post && hops == 0) {
            char line[96] = "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: ";
            char digits[16];
            int k = 0;
            size_t v = strlen(post);
            do digits[k++] = (char)('0' + v % 10);
            while (v /= 10);
            size_t at = strlen(line);
            while (k) line[at++] = digits[--k];
            line[at] = 0;
            strncat_safe(request, line, 96);
            strncat_safe(request, "\r\n", 3);
        }
        strncat_safe(request, "\r\n", 3);
        if (conn_write(&c, request, strlen(request)) != 0 || (post && hops == 0 && conn_write(&c, post, strlen(post)) != 0)) {
            conn_close(&c);
            strcpy(error, "The connection was lost while sending the request.");
            return -1;
        }
        size_t len = 0, cap = 65536;
        char *reply = malloc(cap + 1);
        i64 expected = -1;
        size_t header_end = 0;
        while (reply) {
            if (len + 16384 > cap) {
                if (cap >= MAX_PAGE) break;
                cap *= 2;
                char *bigger = realloc(reply, cap + 1);
                if (!bigger) break;
                reply = bigger;
            }
            i64 got = conn_read(&c, reply + len, 16384);
            if (got <= 0) break;
            len += (size_t)got;
            if (!header_end) {
                reply[len] = 0;
                char *blank = strstr(reply, "\r\n\r\n");
                if (blank) {
                    header_end = (size_t)(blank - reply) + 4;
                    char value[32];
                    if (header_value(reply, "Content-Length", value, sizeof(value))) expected = atoi(value);
                }
            }
            if (header_end && expected >= 0 && len >= header_end + (size_t)expected) break;
        }
        if (c.tls) tls_describe(c.tls, f->security, sizeof(f->security));
        else strcpy(f->security, "not encrypted (http)");
        conn_close(&c);
        if (!reply) {
            strcpy(error, "Not enough memory for that page.");
            return -1;
        }
        reply[len] = 0;
        if (!header_end || memcmp(reply, "HTTP/", 5) != 0) {
            free(reply);
            strcpy(error, "The server did not send a proper answer.");
            return -1;
        }
        reply[header_end - 2] = 0;
        f->status = atoi(strchr(reply, ' ') ? strchr(reply, ' ') + 1 : "0");
        char location[1024];
        if (f->status >= 300 && f->status < 400 && header_value(reply, "Location", location, sizeof(location))) {
            char next[1024];
            resolve_url(url, location, next, sizeof(next));
            strcpy(url, next);
            free(reply);
            post = NULL; /* a redirect after POST is followed with GET */
            continue;
        }
        char encoding[32] = "", content_encoding[32] = "";
        strcpy(f->type, "text/html");
        header_value(reply, "Content-Type", f->type, sizeof(f->type));
        header_value(reply, "Transfer-Encoding", encoding, sizeof(encoding));
        header_value(reply, "Content-Encoding", content_encoding, sizeof(content_encoding));
        char *content = reply + header_end;
        size_t content_len = len - header_end;
        if (!strncasecmp(encoding, "chunked", 7)) content_len = unchunk(content, content_len);
        unsigned char *unpacked = NULL;
        size_t unpacked_len = 0;
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
