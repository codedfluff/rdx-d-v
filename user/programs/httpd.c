/* httpd - a small web server for the files on this machine.
   Usage: httpd [port]        (default 8080; Ctrl+C stops it)

   http://<this machine>:8080/           lists the files
   http://<this machine>:8080/name.txt   sends that file

   Each connection is handled on its own thread, so one visitor never waits
   behind another, and every request has a deadline, so a client that
   connects and then goes silent (a "slowloris") is dropped instead of tying
   the server up. At most MAX_CLIENTS connections are served at once. */
#include "rdx.h"

#define REQUEST_TIMEOUT_MS 10000
#define MAX_CLIENTS 64

static mutex_t clients_lock;
static int active_clients;

static void send_text(int fd, const char *text) { fd_write(fd, text, strlen(text)); }

static void send_number(int fd, i64 value) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value);
    while (n) fd_write(fd, &digits[--n], 1);
}

static void reply(int fd, const char *status, const char *type, const char *body, size_t len) {
    send_text(fd, "HTTP/1.0 ");
    send_text(fd, status);
    send_text(fd, "\r\nServer: rdx-httpd\r\nConnection: close\r\nContent-Type: ");
    send_text(fd, type);
    send_text(fd, "\r\nContent-Length: ");
    send_number(fd, (i64)len);
    send_text(fd, "\r\n\r\n");
    fd_write(fd, body, len);
}

static int ends_with(const char *text, const char *end) {
    size_t a = strlen(text), b = strlen(end);
    return a >= b && strcmp(text + a - b, end) == 0;
}

/* Build an HTML page listing every file. */
static char *listing(size_t *len) {
    size_t cap = 4096, n = 0;
    char *page = malloc(cap);
    const char *head = "<html><body><h1>Files on RDX</h1><ul>\n";
    strcpy(page, head);
    n = strlen(head);
    char name[64];
    for (int i = 0; file_list(i, name, sizeof(name)) != -1; i++) {
        if (file_list(i, name, sizeof(name)) == -2) continue; /* a directory */
        if (n + 200 > cap) page = realloc(page, cap *= 2);
        const char *parts[] = {"<li><a href=\"/", name, "\">", name, "</a></li>\n"};
        for (int p = 0; p < 5; p++) {
            strcpy(page + n, parts[p]);
            n += strlen(parts[p]);
        }
    }
    strcpy(page + n, "</ul></body></html>\n");
    *len = n + strlen(page + n);
    return page;
}

/* Read the request up to the blank line that ends its headers, giving up if
   it does not arrive within the deadline. Returns 1 on a complete request,
   0 if the client was too slow or hung up. */
static int read_request(int fd, char *request, int size) {
    u64 deadline = uptime_ms() + REQUEST_TIMEOUT_MS;
    int n = 0;
    while (n < size - 1) {
        i64 got = recv_nb(fd, request + n, (size_t)(size - 1 - n));
        if (got == -2) { /* nothing yet */
            if (uptime_ms() > deadline) return 0;
            sleep_ms(5);
            continue;
        }
        if (got <= 0) return 0; /* closed or error */
        n += (int)got;
        request[n] = 0;
        for (int i = 0; i + 3 < n; i++) {
            if (memcmp(request + i, "\r\n\r\n", 4) == 0) return 1;
        }
        if (uptime_ms() > deadline) return 0;
    }
    return 1; /* header buffer full: take what we have */
}

static void serve(int fd) {
    char request[2048];
    if (!read_request(fd, request, sizeof(request))) {
        /* Too slow, or disconnected: say so and move on. Nothing is blocked. */
        const char *text = "request timed out\n";
        reply(fd, "408 Request Timeout", "text/plain", text, strlen(text));
        return;
    }

    /* "GET /name HTTP/1.x" */
    if (memcmp(request, "GET /", 5) != 0) {
        const char *text = "only GET is supported\n";
        reply(fd, "400 Bad Request", "text/plain", text, strlen(text));
        return;
    }
    char *name = request + 5;
    char *end = name;
    while (*end && *end != ' ' && *end != '?' && *end != '\r') end++;
    *end = 0;
    printf("GET /%s\n", name);

    if (!*name) {
        size_t len;
        char *page = listing(&len);
        reply(fd, "200 OK", "text/html", page, len);
        free(page);
        return;
    }
    size_t len;
    char *data = read_all(name, &len);
    if (!data) {
        const char *text = "no such file\n";
        reply(fd, "404 Not Found", "text/plain", text, strlen(text));
        return;
    }
    const char *type = ends_with(name, ".html") ? "text/html"
                       : ends_with(name, ".exe") ? "application/octet-stream"
                       : "text/plain";
    reply(fd, "200 OK", type, data, len);
    free(data);
}

/* One connection, on its own thread. The descriptor arrives as the argument. */
static void handle(void *arg) {
    int fd = (int)(i64)arg;
    serve(fd);
    close(fd);
    mutex_lock(&clients_lock);
    active_clients--;
    mutex_unlock(&clients_lock);
}

static int listener = -1;

/* Ctrl+C or kill: stop taking connections and leave with a clean exit
   code. Written with fd_write, not printf, because this can interrupt the
   main thread anywhere - including halfway through a printf of its own. */
static void on_stop(int number) {
    (void)number;
    const char *text = "httpd: shutting down\n";
    fd_write(1, text, strlen(text));
    close(listener);
    exit(0);
}

int main(int argc, char **argv) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    listener = tcp_listen(port);
    signal(SIGINT, on_stop);
    signal(SIGTERM, on_stop);
    if (listener < 0) {
        printf("httpd: cannot listen on port %d (is the network up? is the port in use?)\n", port);
        return 1;
    }
    unsigned char c[16];
    if (net_info(c) == 0) {
        printf("serving files at http://%d.%d.%d.%d:%d/   (Ctrl+C to stop)\n", c[0], c[1], c[2], c[3], port);
    }
    for (;;) {
        int fd = tcp_accept(listener);
        if (fd < 0) continue;

        /* Too many already in flight: turn this one away rather than pile up
           threads without bound. (Stalled ones clear within the timeout.) */
        mutex_lock(&clients_lock);
        int room = active_clients < MAX_CLIENTS;
        if (room) active_clients++;
        mutex_unlock(&clients_lock);
        if (!room) {
            const char *busy = "HTTP/1.0 503 Service Unavailable\r\nConnection: close\r\n\r\nserver busy\n";
            fd_write(fd, busy, strlen(busy));
            close(fd);
            continue;
        }

        if (thread_create(handle, (void *)(i64)fd) < 0) {
            /* Could not start a thread: serve it here as a fallback. */
            handle((void *)(i64)fd);
        }
    }
}
