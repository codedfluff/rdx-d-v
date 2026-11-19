/* wget - fetch a web page or file over HTTP or HTTPS.
   Usage: wget <url> [file]
     wget http://example.com/              print the page
     wget https://example.com/ page.html   save it as page.html
   https connections are encrypted, but the site's certificate is not
   checked yet (see tls.h), and wget says so. */
#include "tls.h"

/* Find `needle` in the first `len` bytes of `data`; -1 if absent. */
static i64 find(const char *data, size_t len, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(data + i, needle, n) == 0) return (i64)i;
    }
    return -1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: wget <url> [file]\n");
        return 1;
    }
    /* Pull the URL apart: http(s)://host[:port]/path */
    char *url = argv[1];
    int https = memcmp(url, "https://", 8) == 0;
    if (https) url += 8;
    else if (memcmp(url, "http://", 7) == 0) url += 7;
    char host[128];
    int port = https ? 443 : 80, n = 0;
    while (*url && *url != '/' && *url != ':' && n < 127) host[n++] = *url++;
    host[n] = 0;
    if (*url == ':') {
        port = atoi(url + 1);
        while (*url && *url != '/') url++;
    }
    const char *path = *url ? url : "/";

    unsigned char ip[4];
    if (net_resolve(host, ip) != 0) {
        printf("wget: cannot find the address of %s\n", host);
        return 1;
    }
    int fd = tcp_connect(ip, port);
    if (fd < 0) {
        printf("wget: could not connect to %s (%d.%d.%d.%d) port %d\n", host, ip[0], ip[1], ip[2], ip[3], port);
        return 1;
    }
    tls_session *tls = NULL;
    if (https) {
        char why[160];
        tls = tls_connect(fd, host, why);
        if (!tls) {
            printf("wget: the secure connection failed: %s\n", why);
            if (strstr(why, "untrusted certificate"))
                printf("      (to download anyway, without knowing who you are talking to: set TLS_INSECURE=1)\n");
            return 1;
        }
    }

    /* The request. "Connection: close" makes the server end the connection
       after the reply, which is how we know where the reply ends. */
    char request[512];
    char *r = request;
    const char *parts[] = {"GET ", path, " HTTP/1.0\r\nHost: ", host,
                           "\r\nUser-Agent: rdx-wget\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n"};
    for (int i = 0; i < 5; i++) {
        for (const char *p = parts[i]; *p && r < request + sizeof(request) - 1; p++) *r++ = *p;
    }
    size_t request_len = (size_t)(r - request);
    if (tls ? tls_write(tls, request, request_len) != 0 : fd_write(fd, request, request_len) < 0) {
        printf("wget: the connection was lost while sending the request\n");
        return 1;
    }

    /* Read the whole reply. */
    size_t len = 0, cap = 16384;
    char *reply = malloc(cap);
    for (;;) {
        if (len + 16384 > cap) reply = realloc(reply, cap *= 2);
        if (!reply) {
            printf("wget: out of memory\n");
            return 1;
        }
        i64 got = tls ? tls_read(tls, reply + len, 16384, 15000) : read(fd, reply + len, 16384);
        if (got <= 0) break;
        len += (size_t)got;
    }
    char security[256] = "";
    if (tls) {
        tls_describe(tls, security, sizeof(security));
        tls_close(tls);
    }
    close(fd);

    /* Headers end at the first blank line; the rest is the content. */
    i64 split = find(reply, len, "\r\n\r\n");
    if (split < 0 || len < 12 || memcmp(reply, "HTTP/", 5) != 0) {
        printf("wget: the server did not send a valid HTTP reply (%d bytes received)\n", (int)len);
        return 1;
    }
    char *body = reply + split + 4;
    size_t body_len = len - (size_t)split - 4;
    int code = atoi(reply + 9);

    if (argc > 2) {
        i64 line_end = find(reply, len, "\r\n");
        reply[line_end] = 0;
        printf("%s\n", reply);
        if (security[0]) printf("connection: %s\n", security);
        if (code != 200) return 1;
        if (file_write(argv[2], body, body_len) != 0) {
            printf("wget: could not save %s: %s\n", argv[2], last_error());
            return 1;
        }
        printf("saved %d bytes as %s\n", (int)body_len, argv[2]);
    } else {
        flush();
        fd_write(1, body, body_len);
    }
    return code == 200 ? 0 : 1;
}
