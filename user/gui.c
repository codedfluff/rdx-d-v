/* gui.c - the client side of the RDX desktop (see gui.h). */
#include "gui.h"

static int connection = -1;

/* Messages that arrived while waiting for something else (a reply to
   MSG_CREATE), to be handed out by gui_next afterwards. */
#define QUEUE_SIZE 64
static gui_message queue[QUEUE_SIZE];
static int queue_head, queue_count;

/* Bytes of a message that has only partly arrived. */
static unsigned char partial[sizeof(gui_message)];
static int partial_have;

int gui_connect(void) {
    if (connection < 0) connection = local_connect(GUI_SERVICE);
    return connection;
}

int gui_fd(void) { return connection; }

/* Keeps the marker string alive in the program file: the compiler cannot
   leave out a string that is passed somewhere it cannot see. */
static const char *volatile program_marker;

void gui_mark(const char *marker) { program_marker = marker; }

/* Look for the marker in a program file. The marker is stored here with
   every byte one higher, so that this library - which every program
   contains - does not itself carry it and make everything look graphical. */
static int file_has_marker(const char *path) {
    i64 size = file_read(path, NULL, 0);
    if (size <= 0) return -1;
    char *data = malloc((size_t)size);
    if (!data) return -1;
    file_read(path, data, (size_t)size);
    const char *shifted = "SEY.HVJ.QSPHSBN";
    char marker[16];
    size_t n = strlen(shifted);
    for (size_t i = 0; i < n; i++) marker[i] = (char)(shifted[i] - 1);
    int found = 0;
    for (i64 i = 0; !found && i + (i64)n <= size; i++) {
        if (data[i] == marker[0] && memcmp(data + i, marker, n) == 0) found = 1;
    }
    free(data);
    return found;
}

int gui_is_graphical(const char *name) {
    /* The places the shell would look: here, then each folder in PATH. */
    char dirs[512] = ".:";
    const char *path = getenv("PATH");
    size_t at = strlen(dirs);
    for (const char *p = path ? path : "/"; *p && at < sizeof(dirs) - 1; p++) dirs[at++] = *p;
    dirs[at] = 0;
    for (char *dir = dirs; dir;) {
        char *colon = dir;
        while (*colon && *colon != ':') colon++;
        char *next = *colon ? colon + 1 : NULL;
        *colon = 0;
        for (int with_exe = 0; with_exe < 2; with_exe++) {
            char candidate[256];
            size_t k = 0;
            if (strcmp(dir, ".") != 0 && !strchr(name, '/')) {
                for (const char *d = dir; *d && k < 200; d++) candidate[k++] = *d;
                if (k && candidate[k - 1] != '/') candidate[k++] = '/';
            }
            for (const char *s = name; *s && k < 240; s++) candidate[k++] = *s;
            if (with_exe) {
                memcpy(candidate + k, ".exe", 4);
                k += 4;
            }
            candidate[k] = 0;
            int marked = file_has_marker(candidate);
            if (marked >= 0) return marked;
        }
        dir = next;
    }
    return -1;
}

int gui_launch(const char *cmdline) {
    char name[64];
    size_t n = 0;
    while (cmdline[n] && cmdline[n] != ' ' && n < sizeof(name) - 1) {
        name[n] = cmdline[n];
        n++;
    }
    name[n] = 0;
    int graphical = gui_is_graphical(name);
    if (graphical < 0) return -1;
    if (graphical) return spawn(cmdline, -1, -1);
    char line[256] = "terminal ";
    size_t at = strlen(line);
    for (const char *p = cmdline; *p && at < sizeof(line) - 1; p++) line[at++] = *p;
    line[at] = 0;
    return spawn(line, -1, -1);
}

static int send_message(const gui_message *m) {
    if (connection < 0) return -1;
    return fd_write(connection, m, sizeof(*m)) == (i64)sizeof(*m) ? 0 : -1;
}

/* Read whatever is available and complete a message if possible.
   Returns 1 with a message, 0 if none is complete yet, -1 if the
   connection has closed. With `wait`, blocks until some bytes come. */
static int receive(gui_message *m, int wait) {
    for (;;) {
        if (!wait) {
            pollfd p = {connection, POLLIN, 0};
            if (poll(&p, 1, 0) <= 0) return 0;
        }
        i64 n = read(connection, partial + partial_have, sizeof(partial) - (size_t)partial_have);
        if (n == EINTR) return 0;
        if (n <= 0) return -1;
        partial_have += (int)n;
        if (partial_have == (int)sizeof(partial)) {
            memcpy(m, partial, sizeof(*m));
            partial_have = 0;
            return 1;
        }
    }
}

static void remember(const gui_message *m) {
    if (queue_count == QUEUE_SIZE) { /* drop the oldest */
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        queue_count--;
    }
    queue[(queue_head + queue_count) % QUEUE_SIZE] = *m;
    queue_count++;
}

int gui_poll(gui_message *m) {
    if (queue_count) {
        *m = queue[queue_head];
        queue_head = (queue_head + 1) % QUEUE_SIZE;
        queue_count--;
        return 1;
    }
    if (connection < 0) return -1;
    return receive(m, 0);
}

int gui_next(gui_message *m, int timeout_ms) {
    int got = gui_poll(m);
    if (got != 0) return got;
    pollfd p = {connection, POLLIN, 0};
    u64 deadline = uptime_ms() + (u64)(timeout_ms < 0 ? 0 : timeout_ms);
    for (;;) {
        int left = timeout_ms < 0 ? -1 : (int)(deadline > uptime_ms() ? deadline - uptime_ms() : 0);
        int ready = poll(&p, 1, left);
        if (ready == EINTR) return 0;
        if (ready <= 0) return 0;
        got = receive(m, 1);
        if (got != 0) return got;
        if (timeout_ms >= 0 && uptime_ms() >= deadline) return 0;
    }
}

gui_window *gui_window_new(int width, int height, const char *title) {
    if (gui_connect() < 0 || width <= 0 || height <= 0) return NULL;
    gui_message m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_CREATE;
    m.a = width;
    m.b = height;
    for (int i = 0; title[i] && i < 39; i++) m.text[i] = title[i];
    if (send_message(&m) != 0) return NULL;
    /* Wait for the answer, keeping anything else for later. */
    for (;;) {
        int got = receive(&m, 1);
        if (got < 0) return NULL;
        if (got == 0) continue;
        if (m.type == MSG_CREATED) break;
        remember(&m);
    }
    gui_window *w = malloc(sizeof(gui_window));
    if (!w) return NULL;
    w->id = m.window;
    w->width = m.a;
    w->height = m.b;
    memcpy(w->shm_name, m.text, sizeof(w->shm_name));
    w->shm_name[39] = 0;
    unsigned *pixels = shm_open(w->shm_name, (size_t)w->width * (size_t)w->height * 4);
    if (!pixels) {
        free(w);
        return NULL;
    }
    w->pixels.pixels = pixels;
    w->pixels.width = w->pixels.stride = w->width;
    w->pixels.height = w->height;
    return w;
}

void gui_update(gui_window *w, int x, int y, int width, int height) {
    gui_message m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_UPDATE;
    m.window = w->id;
    m.a = x;
    m.b = y;
    m.c = width;
    m.d = height;
    send_message(&m);
}

void gui_set_title(gui_window *w, const char *title) {
    gui_message m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_TITLE;
    m.window = w->id;
    for (int i = 0; title[i] && i < 39; i++) m.text[i] = title[i];
    send_message(&m);
}

void gui_window_close(gui_window *w) {
    gui_message m;
    memset(&m, 0, sizeof(m));
    m.type = MSG_DESTROY;
    m.window = w->id;
    send_message(&m);
    shm_close(w->pixels.pixels);
    free(w);
}
