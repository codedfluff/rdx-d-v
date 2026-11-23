/* rdx.c - the RDX C runtime: startup code, system calls, and a tiny libc. */
#include "rdx.h"
#include "font8x16.h"

/* Compilers targeting Windows-style programs emit a reference to this in
   any code that uses floating point (the C library normally defines it). */
int _fltused = 0;

/* ---------------------------------------------------------------- syscalls */

i64 syscall(i64 number, i64 a, i64 b, i64 c) {
    i64 result;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c)
                     : "memory", "cc");
    return result;
}

i64 syscall5(i64 number, i64 a, i64 b, i64 c, i64 d, i64 e) {
    i64 result;
    register i64 fourth __asm__("r10") = d;
    register i64 fifth __asm__("r8") = e;
    __asm__ volatile("int $0x80"
                     : "=a"(result)
                     : "a"(number), "D"(a), "S"(b), "d"(c), "r"(fourth), "r"(fifth)
                     : "memory", "cc");
    return result;
}

static void flush_at_exit(void);

void exit(int code) {
    flush_at_exit();
    syscall(0, code, 0, 0);
    for (;;) {
    }
}

i64 fd_write(int fd, const void *buf, size_t len) {
    /* A pipe may take only part of it at a time; keep going until done. */
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done > 65536 ? 65536 : len - done;
        i64 n = syscall(17, fd, (i64)((const char *)buf + done), (i64)chunk);
        if (n < 0) return -1;
        done += (size_t)n;
    }
    return (i64)done;
}

i64 write(const char *buf, size_t len) { return fd_write(1, buf, len); }
i64 read(int fd, void *buf, size_t max) { return syscall(18, fd, (i64)buf, (i64)max); }
i64 recv_nb(int fd, void *buf, size_t max) { return syscall(38, fd, (i64)buf, (i64)max); }
int pipe(int fds[2]) { return (int)syscall(19, (i64)fds, 0, 0); }
int close(int fd) { return (int)syscall(20, fd, 0, 0); }
int spawn(const char *cmdline, int stdin_fd, int stdout_fd) { return (int)syscall(21, (i64)cmdline, stdin_fd, stdout_fd); }
int wait(int pid) { return (int)syscall(22, pid, 0, 0); }
int console_key(void) { return (int)syscall(27, 0, 0, 0); }
int open(const char *path, int mode) { return (int)syscall(56, (i64)path, mode, 0); }
int poll(pollfd *fds, int count, int timeout_ms) {
    flush();
    return (int)syscall(61, (i64)fds, count, timeout_ms);
}
int pty_open(int fds[2]) { return (int)syscall(62, (i64)fds, 0, 0); }
int pty_resize(int fd, int cols, int rows) { return (int)syscall(63, fd, cols, rows); }
int pty_foreground(int fd, const int *pids, int count) { return (int)syscall(70, fd, (i64)pids, count); }
int spawn_fds(const char *cmdline, int stdin_fd, int stdout_fd, int stderr_fd) {
    return (int)syscall5(64, (i64)cmdline, stdin_fd, stdout_fd, stderr_fd, 0);
}
int set_foreground(const int *pids, int count) { return (int)syscall(55, (i64)pids, count, 0); }

const char *last_error(void) {
    static char text[128];
    if (syscall(57, (i64)text, sizeof(text), 0) < 0) text[0] = 0;
    return text;
}

/* ---------------------------------------------------------------- graphics */

int display_open(display_info *info) {
    struct {
        u64 address;
        unsigned width, height, stride, format;
        int x, y;
    } raw;
    flush();
    int fd = (int)syscall(65, (i64)&raw, 0, 0);
    if (fd < 0) return -1;
    info->pixels = (unsigned *)raw.address;
    info->width = (int)raw.width;
    info->height = (int)raw.height;
    info->stride = (int)raw.stride;
    info->format = (int)raw.format;
    info->mouse_x = raw.x;
    info->mouse_y = raw.y;
    return fd;
}

int read_events(int fd, input_event *events, int max) {
    i64 n = read(fd, events, (size_t)max * sizeof(input_event));
    if (n == EINTR) return EINTR;
    return n < 0 ? -1 : (int)(n / (i64)sizeof(input_event));
}

canvas canvas_new(int width, int height) {
    canvas c = {malloc((size_t)width * (size_t)height * 4), width, height, width};
    return c;
}

/* Clip a rectangle to the canvas; returns 0 if nothing is left. */
static int clip(const canvas *c, int *x, int *y, int *w, int *h) {
    if (*x < 0) {
        *w += *x;
        *x = 0;
    }
    if (*y < 0) {
        *h += *y;
        *y = 0;
    }
    if (*x + *w > c->width) *w = c->width - *x;
    if (*y + *h > c->height) *h = c->height - *y;
    return *w > 0 && *h > 0;
}

void fill_rect(canvas *c, int x, int y, int w, int h, unsigned color) {
    if (!c->pixels || !clip(c, &x, &y, &w, &h)) return;
    for (int row = y; row < y + h; row++) {
        unsigned *p = c->pixels + (size_t)row * (size_t)c->stride + x;
        for (int i = 0; i < w; i++) p[i] = color;
    }
}

void draw_rect(canvas *c, int x, int y, int w, int h, unsigned color) {
    fill_rect(c, x, y, w, 1, color);
    fill_rect(c, x, y + h - 1, w, 1, color);
    fill_rect(c, x, y, 1, h, color);
    fill_rect(c, x + w - 1, y, 1, h, color);
}

void draw_line(canvas *c, int x0, int y0, int x1, int y1, int thickness, unsigned color) {
    /* Bresenham, stamping a square of the given thickness at each step. */
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
    int dy = y1 > y0 ? y0 - y1 : y1 - y0, sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, half = thickness / 2;
    for (;;) {
        fill_rect(c, x0 - half, y0 - half, thickness, thickness, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx) {
            err += dx;
            y0 += sy;
        }
    }
}

void draw_char(canvas *c, int x, int y, char ch, unsigned color) {
    unsigned char code = (unsigned char)ch;
    if (code < 32 || code > 127) code = '?';
    const unsigned char *glyph = FONT_8X16 + (code - 32) * CHAR_HEIGHT;
    for (int row = 0; row < CHAR_HEIGHT; row++) {
        int py = y + row;
        if (py < 0 || py >= c->height) continue;
        for (int col = 0; col < CHAR_WIDTH; col++) {
            int px = x + col;
            if (px >= 0 && px < c->width && (glyph[row] & (0x80 >> col)))
                c->pixels[(size_t)py * (size_t)c->stride + px] = color;
        }
    }
}

void draw_text(canvas *c, int x, int y, const char *text, unsigned color) {
    for (; *text; text++, x += CHAR_WIDTH) draw_char(c, x, y, *text, color);
}

void draw_text_scaled(canvas *c, int x, int y, const char *text, int scale, unsigned color) {
    if (scale <= 1) {
        draw_text(c, x, y, text, color);
        return;
    }
    for (; *text; text++, x += CHAR_WIDTH * scale) {
        unsigned char code = (unsigned char)*text;
        if (code < 32 || code > 127) code = '?';
        const unsigned char *glyph = FONT_8X16 + (code - 32) * CHAR_HEIGHT;
        for (int row = 0; row < CHAR_HEIGHT; row++) {
            for (int col = 0; col < CHAR_WIDTH; col++) {
                if (glyph[row] & (0x80 >> col)) fill_rect(c, x + col * scale, y + row * scale, scale, scale, color);
            }
        }
    }
}

void draw_glyphs(canvas *c, int x, int y, const char *text, int len, int cell, unsigned color, int flags) {
    int h = cell * 2;
    if (y + h <= 0 || y >= c->height) return;
    for (int i = 0; i < len; i++, x += cell) {
        if (x >= c->width) break;
        if (x + cell + 4 <= 0) continue;
        unsigned char code = (unsigned char)text[i];
        if (code < 32 || code > 127) code = '?';
        if (code == ' ') continue;
        const unsigned char *glyph = FONT_8X16 + (code - 32) * CHAR_HEIGHT;
        for (int ty = 0; ty < h; ty++) {
            int py = y + ty;
            if (py < 0 || py >= c->height) continue;
            unsigned char bits = glyph[ty * CHAR_HEIGHT / h];
            if (!bits) continue;
            int shift = (flags & GLYPH_ITALIC) ? (h - 1 - ty) * cell / 48 : 0;
            unsigned *row = c->pixels + (size_t)py * (size_t)c->stride;
            for (int tx = 0; tx < cell; tx++) {
                if (!(bits & (0x80 >> (tx * CHAR_WIDTH / cell)))) continue;
                int px = x + tx + shift;
                if (px >= 0 && px < c->width) row[px] = color;
                if ((flags & GLYPH_BOLD) && px + 1 >= 0 && px + 1 < c->width) row[px + 1] = color;
            }
        }
    }
}

void blit(canvas *dst, int dx, int dy, const canvas *src, int sx, int sy, int w, int h) {
    /* Clip against the source, then the destination. */
    if (sx < 0) { w += sx; dx -= sx; sx = 0; }
    if (sy < 0) { h += sy; dy -= sy; sy = 0; }
    if (sx + w > src->width) w = src->width - sx;
    if (sy + h > src->height) h = src->height - sy;
    if (dx < 0) { w += dx; sx -= dx; dx = 0; }
    if (dy < 0) { h += dy; sy -= dy; dy = 0; }
    if (dx + w > dst->width) w = dst->width - dx;
    if (dy + h > dst->height) h = dst->height - dy;
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; row++)
        memcpy(dst->pixels + (size_t)(dy + row) * (size_t)dst->stride + dx,
               src->pixels + (size_t)(sy + row) * (size_t)src->stride + sx, (size_t)w * 4);
}

void present(const display_info *screen, const canvas *c, int x, int y, int w, int h) {
    canvas target = screen_canvas(screen);
    if (!clip(&target, &x, &y, &w, &h) || !clip(c, &x, &y, &w, &h)) return;
    for (int row = y; row < y + h; row++) {
        unsigned *to = screen->pixels + (size_t)row * (size_t)screen->stride + x;
        const unsigned *from = c->pixels + (size_t)row * (size_t)c->stride + x;
        if (screen->format == 0) {
            memcpy(to, from, (size_t)w * 4);
        } else {
            for (int i = 0; i < w; i++) {
                unsigned v = from[i];
                to[i] = (v & 0xFF00) | (v >> 16 & 0xFF) | (v & 0xFF) << 16;
            }
        }
    }
}

canvas screen_canvas(const display_info *screen) {
    canvas c = {screen->pixels, screen->width, screen->height, screen->stride};
    return c;
}

int ping_send(const unsigned char ip[4], int sequence) { return (int)syscall(59, (i64)ip, sequence, 0); }
i64 ping_reply(int sequence) { return syscall(60, sequence, 0, 0); }
int kernel_debug(int what) { return (int)syscall(58, what, 0, 0); }

void *shm_open(const char *name, size_t size) {
    i64 r = syscall(23, (i64)name, (i64)size, 0);
    return r == -1 ? NULL : (void *)r;
}

int shm_unlink(const char *name) { return (int)syscall(24, (i64)name, 0, 0); }
int shm_close(void *address) { return (int)syscall(69, (i64)address, 0, 0); }
int local_listen(const char *name) { return (int)syscall(66, (i64)name, 0, 0); }
int local_connect(const char *name) { return (int)syscall(67, (i64)name, 0, 0); }
int local_accept(int listen_fd) { return (int)syscall(68, listen_fd, 0, 0); }
i64 file_list(int index, char *name, size_t max) { return syscall(25, index, (i64)name, (i64)max); }
int file_delete(const char *name) { return (int)syscall(26, (i64)name, 0, 0); }
int getcwd(char *buf, size_t max) {
    (void)max;
    return (int)syscall(39, (i64)buf, 0, 0);
}
int chdir(const char *path) { return (int)syscall(40, (i64)path, 0, 0); }
int mkdir(const char *path) { return (int)syscall(41, (i64)path, 0, 0); }
int rmdir(const char *path) { return (int)syscall(42, (i64)path, 0, 0); }

int isatty(int fd) { return syscall(54, fd, 0, 0) == 1; }

/* Standard input that is not the keyboard (a pipe or a file) is read a
   block at a time and handed out from here, so getchar() does not cost a
   system call per character. Keys are never buffered: each one is wanted
   the moment it is pressed. (Don't mix getchar() with read(0, ...) on the
   same piped input: read would miss what is waiting here.) */
static char in_buf[4096];
static i64 in_len, in_pos;
static int in_kind; /* 0 not known yet, 1 keyboard, 2 pipe or file */

static int stdin_is_buffered(void) {
    if (!in_kind) in_kind = isatty(0) ? 1 : 2;
    return in_kind == 2;
}

int getchar(void) {
    if (!stdin_is_buffered()) {
        flush(); /* show any prompt before waiting for a key */
        return (int)syscall(2, 0, 0, 0);
    }
    if (in_pos == in_len) {
        /* About to wait for more input: push our own output out first, so
           whoever is reading it is not kept waiting on us. */
        flush();
        in_len = read(0, in_buf, sizeof(in_buf));
        in_pos = 0;
        if (in_len <= 0) {
            in_len = 0;
            return -1;
        }
    }
    return (unsigned char)in_buf[in_pos++];
}

int pollchar(void) {
    flush();
    if (stdin_is_buffered() && in_pos < in_len) return (unsigned char)in_buf[in_pos++];
    return (int)syscall(11, 0, 0, 0);
}

void sleep_ms(u64 ms) {
    flush();
    syscall(3, (i64)ms, 0, 0);
}

u64 uptime_ms(void) { return (u64)syscall(4, 0, 0, 0); }
int getpid(void) { return (int)syscall(5, 0, 0, 0); }

void *sbrk(i64 increment) {
    i64 r = syscall(6, increment, 0, 0);
    return r == -1 ? NULL : (void *)r;
}

i64 file_read(const char *name, void *buf, size_t max) { return syscall(7, (i64)name, (i64)buf, (i64)max); }
i64 file_write(const char *name, const void *buf, size_t len) { return syscall(8, (i64)name, (i64)buf, (i64)len); }
void yield(void) { syscall(9, 0, 0, 0); }

static i64 console(i64 op, i64 a, i64 b) {
    flush();
    return syscall(10, op, a, b);
}

void screen_clear(void) { console(0, 0, 0); }
void screen_goto(int col, int row) { console(1, col, row); }
void screen_size(int *cols, int *rows) {
    i64 r = console(2, 0, 0);
    *cols = (int)(r >> 16);
    *rows = (int)(r & 0xFFFF);
}
void screen_color(unsigned rgb) { console(3, rgb, 0); }
void screen_clear_eol(void) { console(4, 0, 0); }
void screen_mirror(int on) { console(5, on, 0); }
void screen_left(int cells) {
    if (cells > 0) console(6, cells, 0);
}

/* ---------------------------------------------------------------- networking */

int net_resolve(const char *name, unsigned char ip[4]) { return (int)syscall(28, (i64)name, (i64)ip, 0); }
int net_info(unsigned char out[16]) { return (int)syscall(36, (i64)out, 0, 0); }

int tcp_connect(const unsigned char ip[4], int port) {
    int fd = (int)syscall(29, (i64)ip, port, 0);
    if (fd < 0) return -1;
    /* The handshake runs in the background; wait for its outcome. */
    if (syscall(30, fd, 0, 0) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

int tcp_connect_name(const char *host, int port) {
    unsigned char ip[4];
    if (net_resolve(host, ip) != 0) return -1;
    return tcp_connect(ip, port);
}

int tcp_listen(int port) { return (int)syscall(31, port, 0, 0); }
int tcp_accept(int listen_fd) { return (int)syscall(32, listen_fd, 0, 0); }
int tcp_shutdown(int fd) { return (int)syscall(37, fd, 0, 0); }
int udp_open(int port) { return (int)syscall(33, port, 0, 0); }

i64 udp_sendto(int fd, const unsigned char ip[4], int port, const void *buf, size_t len) {
    return syscall5(34, fd, (i64)ip, port, (i64)buf, (i64)len);
}

i64 udp_recvfrom(int fd, void *buf, size_t max, unsigned char ip[4], int *port) {
    unsigned char from[6];
    i64 n = syscall5(35, fd, (i64)buf, (i64)max, (i64)from, 0);
    if (n >= 0) {
        if (ip) memcpy(ip, from, 4);
        if (port) *port = from[4] | from[5] << 8;
    }
    return n;
}

/* ---------------------------------------------------------------- threads */

void thread_exit(void) {
    syscall(13, 0, 0, 0);
    for (;;) {
    }
}

/* Every thread starts here, so that returning from its function ends it. */
static void thread_start(void (*entry)(void *), void *arg) {
    entry(arg);
    thread_exit();
}

/* Has this program ever had more than one thread? */
static volatile int threaded;

int thread_create(void (*entry)(void *), void *arg) {
    threaded = 1;
    return (int)syscall(12, (i64)thread_start, (i64)entry, (i64)arg);
}

void thread_join(int tid) { syscall(14, tid, 0, 0); }
int gettid(void) { return (int)syscall(15, 0, 0, 0); }
int cpu_count(void) { return (int)syscall(16, 0, 0, 0); }

void mutex_lock(mutex_t *m) {
    /* Try to take it; spin briefly (the holder may be on another core and
       about to release), then start giving up the CPU between tries. */
    int tries = 0;
    while (__sync_lock_test_and_set(&m->locked, 1)) {
        if (++tries > 100) yield();
    }
}

void mutex_unlock(mutex_t *m) { __sync_lock_release(&m->locked); }

/* The library's own shared state: the heap and the output buffer. */
static mutex_t heap_lock, out_lock;

/* ---------------------------------------------------------------- strings */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

int tolower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }
int toupper(int c) { return c >= 'a' && c <= 'z' ? c - 32 : c; }

int strncmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i] || !a[i]) return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}

int strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int x = tolower((unsigned char)a[i]), y = tolower((unsigned char)b[i]);
        if (x != y || !x) return x - y;
    }
    return 0;
}

char *strchr(const char *s, int c) {
    for (;; s++) {
        if (*s == (char)c) return (char *)s;
        if (!*s) return NULL;
    }
}

char *strstr(const char *haystack, const char *needle) {
    size_t n = strlen(needle);
    for (; *haystack; haystack++) {
        if (strncmp(haystack, needle, n) == 0) return (char *)haystack;
    }
    return n == 0 ? (char *)haystack : NULL;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char *strcpy(char *dst, const char *src) {
    char *d = dst;
    while ((*d++ = *src++)) {
    }
    return dst;
}

void *memcpy(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    while (n--) *d++ = *s++;
    return dst;
}

void *memmove(void *dst, const void *src, size_t n) {
    unsigned char *d = dst;
    const unsigned char *s = src;
    if (d < s) {
        while (n--) *d++ = *s++;
    } else {
        while (n--) d[n] = s[n];
    }
    return dst;
}

void *memset(void *dst, int value, size_t n) {
    unsigned char *d = dst;
    while (n--) *d++ = (unsigned char)value;
    return dst;
}

int memcmp(const void *a, const void *b, size_t n) {
    const unsigned char *x = a, *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}

int atoi(const char *s) {
    int sign = 1, n = 0;
    while (*s == ' ') s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return sign * n;
}

/* ---------------------------------------------------------------- heap
   A buddy allocator. Blocks are powers of two in size (32 bytes
   up), each at an address that is a multiple of its size (counted from
   the heap's start), with a 16-byte header recording its size class. A
   freed block whose "buddy" (the other half of the block twice its size)
   is free too is joined with it, again and again: memory freed in small
   pieces can come back together for big requests, and big free blocks
   are split for small ones. */

#define BLOCK_FREE ((size_t)1 << 63) /* (in Block.class: on a free list) */

typedef struct Block {
    size_t class;            /* size class (block size = 1 << class), | BLOCK_FREE */
    struct Block *next_free; /* free blocks: the next on the list ... */
} Block;                     /* ... and the previous, just after the header */

static Block *free_lists[48];
static char *heap_start, *heap_end; /* what sbrk has given */

static size_t class_for(size_t size) {
    size_t class = 5; /* smallest block: 32 bytes including the header */
    while (((size_t)1 << class) < size + sizeof(Block)) class++;
    return class;
}

static Block **prev_of(Block *b) { return (Block **)(b + 1); }

static void list_push(Block *b, size_t class) {
    b->class = class | BLOCK_FREE;
    b->next_free = free_lists[class];
    *prev_of(b) = NULL;
    if (free_lists[class]) *prev_of(free_lists[class]) = b;
    free_lists[class] = b;
}

static void list_remove(Block *b, size_t class) {
    Block *prev = *prev_of(b);
    if (prev) prev->next_free = b->next_free;
    else free_lists[class] = b->next_free;
    if (b->next_free) *prev_of(b->next_free) = prev;
    b->class = class;
}

/* A block of `class` from fresh memory at the heap's end, aligned to its
   size (the gap before it becomes free blocks). */
static Block *heap_grow(size_t class) {
    if (!heap_start) heap_start = heap_end = sbrk(0);
    if (!heap_start) return NULL;
    size_t size = (size_t)1 << class;
    size_t at = (size_t)(heap_end - heap_start);
    size_t aligned = (at + size - 1) & ~(size - 1);
    if (!sbrk((i64)(aligned + size - at))) return NULL;
    /* the gap: the biggest aligned free blocks that fit */
    while (at < aligned) {
        size_t c = 5;
        while (c + 1 < 48 && (at & (((size_t)1 << (c + 1)) - 1)) == 0 && at + ((size_t)1 << (c + 1)) <= aligned) c++;
        list_push((Block *)(heap_start + at), c);
        at += (size_t)1 << c;
    }
    heap_end = heap_start + aligned + size;
    Block *b = (Block *)(heap_start + aligned);
    b->class = class;
    return b;
}

void *malloc(size_t size) {
    size_t class = class_for(size);
    if (class >= 40) return NULL;
    mutex_lock(&heap_lock);
    Block *b = NULL;
    size_t c = class;
    while (c < 48 && !free_lists[c]) c++;
    if (c < 48) { /* a free block this size, or a bigger one split in halves */
        b = free_lists[c];
        list_remove(b, c);
        while (c > class) {
            c--;
            list_push((Block *)((char *)b + ((size_t)1 << c)), c);
        }
        b->class = class;
    } else {
        b = heap_grow(class);
    }
    mutex_unlock(&heap_lock);
    return b ? b + 1 : NULL;
}

size_t heap_free_bytes(void) {
    size_t total = 0;
    mutex_lock(&heap_lock);
    for (int c = 0; c < 48; c++) {
        for (Block *b = free_lists[c]; b; b = b->next_free) total += (size_t)1 << c;
    }
    mutex_unlock(&heap_lock);
    return total;
}

void free(void *ptr) {
    if (!ptr) return;
    Block *b = (Block *)ptr - 1;
    mutex_lock(&heap_lock);
    size_t class = b->class & ~BLOCK_FREE;
    /* join with the buddy while it is free and whole */
    while (class < 39) {
        size_t off = (size_t)((char *)b - heap_start);
        Block *buddy = (Block *)(heap_start + (off ^ ((size_t)1 << class)));
        if ((char *)buddy + ((size_t)1 << class) > heap_end || buddy->class != (class | BLOCK_FREE)) break;
        list_remove(buddy, class);
        if (buddy < b) b = buddy;
        class++;
    }
    list_push(b, class);
    mutex_unlock(&heap_lock);
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    Block *b = (Block *)ptr - 1;
    size_t have = ((size_t)1 << (b->class & ~BLOCK_FREE)) - sizeof(Block);
    if (size <= have) return ptr;
    void *fresh = malloc(size);
    if (!fresh) return NULL;
    memcpy(fresh, ptr, have);
    free(ptr);
    return fresh;
}

/* ---------------------------------------------------------------- whole files and lines */

char *read_all(const char *name, size_t *len) {
    size_t size = 0, cap = 0;
    char *data = NULL;
    if (name) {
        i64 n = file_read(name, NULL, 0);
        if (n < 0) return NULL;
        size = (size_t)n;
        data = malloc(size + 1);
        if (!data) return NULL;
        file_read(name, data, size);
    } else {
        /* Standard input: read until it ends, growing the buffer. */
        for (;;) {
            if (size + 4096 + 1 > cap) {
                cap = cap ? cap * 2 : 8192;
                data = realloc(data, cap);
                if (!data) return NULL;
            }
            i64 n = read(0, data + size, 4096);
            if (n <= 0) break;
            size += (size_t)n;
        }
    }
    data[size] = 0;
    if (len) *len = size;
    return data;
}

char **split_lines(char *text, int *count) {
    int cap = 64, n = 0;
    char **lines = malloc(sizeof(char *) * (size_t)cap);
    char *p = text;
    while (*p) {
        if (n == cap) lines = realloc(lines, sizeof(char *) * (size_t)(cap *= 2));
        lines[n++] = p;
        while (*p && *p != '\n') p++;
        if (*p) *p++ = 0;
    }
    *count = n;
    return lines;
}

/* ---------------------------------------------------------------- console */

/* Output is collected here and flushed on newline, so printf doesn't cost
   one system call per character. */
static char out_buf[512];
static size_t out_len;

/* The buffer is shared by all threads. The public functions take out_lock
   once per call, so a whole printf from one thread stays in one piece; the
   helpers below assume it is already held. */
static void flush_unlocked(void) {
    if (out_len) {
        write(out_buf, out_len);
        out_len = 0;
    }
}

static void put(int c) {
    out_buf[out_len++] = (char)c;
    if (c == '\n' || out_len == sizeof(out_buf)) flush_unlocked();
}

void flush(void) {
    mutex_lock(&out_lock);
    flush_unlocked();
    mutex_unlock(&out_lock);
}

/* exit() may be called from a signal handler that interrupted this very
   thread in the middle of printing, with the lock held. Waiting for it
   would then wait for ever, so give up after a while and exit anyway. */
static void flush_at_exit(void) {
    int tries = 0;
    while (__sync_lock_test_and_set(&out_lock.locked, 1)) {
        if (++tries > 200) return;
        yield();
    }
    flush_unlocked();
    mutex_unlock(&out_lock);
}

/* ---------------------------------------------------------------- signals */

static sighandler_t signal_handlers[32];

/* The kernel calls this (on the first thread) for every signal. */
static void signal_entry(int number) {
    sighandler_t handler = signal_handlers[number & 31];
    if (handler == SIG_IGN) return;
    if (handler == SIG_DFL) {
        syscall(0, 128 + number, 0, 0); /* end at once: no flushing, no waiting */
    }
    /* If this thread was interrupted while printing or allocating, it holds
       the library's locks and the handler could never take them. In a
       single-threaded program that is certainly what a held lock means, so
       lend them to the handler. */
    int had_out = out_lock.locked, had_heap = heap_lock.locked;
    if (!threaded) out_lock.locked = heap_lock.locked = 0;
    handler(number);
    if (!threaded) {
        out_lock.locked = had_out;
        heap_lock.locked = had_heap;
    }
}

sighandler_t signal(int number, sighandler_t handler) {
    if (number < 1 || number > 31 || number == SIGKILL) return SIG_DFL;
    sighandler_t old = signal_handlers[number];
    signal_handlers[number] = handler;
    /* The kernel knows one entry point per program; it is needed as long as
       any signal has a handler. */
    int any = 0;
    for (int i = 1; i < 32; i++) any |= signal_handlers[i] != SIG_DFL;
    syscall(43, any ? (i64)signal_entry : 0, 0, 0);
    return old;
}

int kill(int pid, int number) { return (int)syscall(45, pid, number, 0); }

/* ---------------------------------------------------------------- environment, time, system */

char *getenv(const char *name) {
    static char value[1024];
    return syscall(46, (i64)name, (i64)value, sizeof(value)) < 0 ? NULL : value;
}

int setenv(const char *name, const char *value) { return (int)syscall(47, (i64)name, (i64)(value ? value : ""), 0); }
int unsetenv(const char *name) { return (int)syscall(47, (i64)name, 0, 0); }
i64 env_entry(int index, char *buf, size_t max) { return syscall(48, index, (i64)buf, (i64)max); }

u64 time_now(void) { return (u64)syscall(49, 0, 0, 0); }

void getrandom(void *buf, size_t len) {
    for (size_t done = 0; done < len;) {
        size_t n = len - done > 4096 ? 4096 : len - done;
        syscall(71, (i64)((char *)buf + done), (i64)n, 0);
        done += n;
    }
}

static void two_digits(char *at, u64 value) {
    at[0] = (char)('0' + value / 10 % 10);
    at[1] = (char)('0' + value % 10);
}

void format_time(u64 seconds, char buf[20]) {
    /* Days since 1970 -> year, month, day (the "civil from days" method). */
    u64 z = seconds / 86400 + 719468;
    u64 era = z / 146097;
    u64 doe = z - era * 146097;
    u64 yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    u64 doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    u64 mp = (5 * doy + 2) / 153;
    u64 day = doy - (153 * mp + 2) / 5 + 1;
    u64 month = mp < 10 ? mp + 3 : mp - 9;
    u64 year = yoe + era * 400 + (month <= 2);
    u64 t = seconds % 86400;
    two_digits(buf, year / 100);
    two_digits(buf + 2, year % 100);
    buf[4] = '-';
    two_digits(buf + 5, month);
    buf[7] = '-';
    two_digits(buf + 8, day);
    buf[10] = ' ';
    two_digits(buf + 11, t / 3600);
    buf[13] = ':';
    two_digits(buf + 14, t / 60 % 60);
    buf[16] = ':';
    two_digits(buf + 17, t % 60);
    buf[19] = 0;
}

int file_stat(const char *path, i64 *size, u64 *modified) {
    u64 out[2];
    int kind = (int)syscall(50, (i64)path, (i64)out, 0);
    if (kind < 0) return -1;
    if (size) *size = (i64)out[0];
    if (modified) *modified = out[1];
    return kind;
}

int proc_list(int index, proc_info *out) { return (int)syscall(51, index, (i64)out, 0); }
int sysinfo(sys_info *out) { return (int)syscall(52, (i64)out, 0, 0); }

void shutdown(void) {
    flush();
    syscall(53, 0, 0, 0);
}

void reboot(void) {
    flush();
    syscall(53, 1, 0, 0);
}

int putchar(int c) {
    mutex_lock(&out_lock);
    put(c);
    mutex_unlock(&out_lock);
    return c;
}

int puts(const char *s) {
    mutex_lock(&out_lock);
    while (*s) put(*s++);
    put('\n');
    mutex_unlock(&out_lock);
    return 0;
}

static void print_number(u64 value, unsigned base, int negative, int width, char pad) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value % base];
        value /= base;
    } while (value);
    int len = n + negative;
    if (negative && pad == '0') put('-');
    while (len++ < width) put(pad);
    if (negative && pad != '0') put('-');
    while (n) put(digits[--n]);
}

/* One whole formatted message is written under the lock, so output from
   different threads never interleaves mid-line. */
int vprintf(const char *fmt, va_list ap) {
    mutex_lock(&out_lock);
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            put(*fmt);
            continue;
        }
        fmt++;
        char pad = ' ';
        int width = 0, longs = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        switch (*fmt) {
        case 'd':
        case 'i': {
            i64 v = longs ? va_arg(ap, i64) : va_arg(ap, int);
            print_number(v < 0 ? 0 - (u64)v : (u64)v, 10, v < 0, width, pad);
            break;
        }
        case 'u':
            print_number(longs ? va_arg(ap, u64) : va_arg(ap, unsigned), 10, 0, width, pad);
            break;
        case 'x':
            print_number(longs ? va_arg(ap, u64) : va_arg(ap, unsigned), 16, 0, width, pad);
            break;
        case 'p':
            put('0');
            put('x');
            print_number(va_arg(ap, u64), 16, 0, 0, ' ');
            break;
        case 'c':
            put(va_arg(ap, int));
            break;
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int len = (int)strlen(s);
            while (len++ < width) put(' ');
            while (*s) put(*s++);
            break;
        }
        case '%':
            put('%');
            break;
        case '\0':
            fmt--; /* stray % at the end of the string */
            break;
        default:
            put('%');
            put(*fmt);
        }
    }
    mutex_unlock(&out_lock);
    return 0;
}

int printf(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    return 0;
}

int readline(char *buf, int max) {
    int n = 0;
    for (;;) {
        int c = getchar();
        if (c == '\n') {
            write("\n", 1);
            break;
        }
        if (c == 8) {
            if (n > 0) {
                n--;
                write("\b", 1);
            }
        } else if (c >= 32 && c < 127 && n < max - 1) {
            buf[n++] = (char)c;
            write(&buf[n - 1], 1);
        }
    }
    buf[n] = 0;
    return n;
}

int print_file(const char *path) {
    size_t len;
    char *data = read_all(path, &len);
    if (!data) {
        printf("%s: no such file\n", path);
        return 1;
    }
    flush();
    write(data, len);
    free(data);
    return 0;
}

/* ---------------------------------------------------------------- startup */

int main(int argc, char **argv);

static char tail_text[256];

const char *command_tail(void) { return tail_text; }

/* The kernel jumps here with the command line as the only argument. */
void _start(char *cmdline) {
    static char *argv[16];
    int argc = 0;
    char *p = cmdline;
    /* Keep an untouched copy of what follows the program's name. */
    while (*p == ' ') p++;
    while (*p && *p != ' ') p++;
    while (*p == ' ') p++;
    for (size_t i = 0; p[i] && i < sizeof(tail_text) - 1; i++) tail_text[i] = p[i];
    p = cmdline;
    while (*p && argc < 15) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    exit(main(argc, argv));
}
