/* clib.c - the runtime library for programs compiled inside RDX by c-comp.

   c-comp compiles this file in front of every program, so everything here
   is available without an #include. It is written in the subset of C that
   c-comp understands, and mirrors rdx.h so the same source also builds
   with the Windows-side compiler. */

typedef long long i64;
typedef unsigned long long u64;
typedef unsigned long long size_t;

#define NULL ((void *)0)

#define KEY_UP 0x80
#define KEY_DOWN 0x81
#define KEY_LEFT 0x82
#define KEY_RIGHT 0x83
#define KEY_HOME 0x84
#define KEY_END 0x85
#define KEY_PAGE_UP 0x86
#define KEY_PAGE_DOWN 0x87
#define KEY_DELETE 0x88
#define KEY_ESCAPE 0x1B
#define COLOR_DEFAULT 0xD0D4DC

void flush(void);

/* ---- system calls ---- */

i64 syscall(i64 number, i64 a, i64 b, i64 c) { return __syscall(number, a, b, c); }

void exit(int code) {
    flush();
    __syscall(0, code, 0, 0);
    while (1) {
    }
}

/* A pipe may take only part of the data at a time; keep going until done. */
i64 fd_write(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done;
        if (chunk > 65536) chunk = 65536;
        i64 n = __syscall(17, fd, (i64)(p + done), chunk);
        if (n < 0) return -1;
        done += n;
    }
    return done;
}

i64 write(const char *buf, size_t len) { return fd_write(1, buf, len); }
i64 read(int fd, void *buf, size_t max) { return __syscall(18, fd, (i64)buf, max); }
int pipe(int *fds) { return (int)__syscall(19, (i64)fds, 0, 0); }
int close(int fd) { return (int)__syscall(20, fd, 0, 0); }
int spawn(const char *cmdline, int stdin_fd, int stdout_fd) { return (int)__syscall(21, (i64)cmdline, stdin_fd, stdout_fd); }
int wait(int pid) { return (int)__syscall(22, pid, 0, 0); }
int console_key(void) { return (int)__syscall(27, 0, 0, 0); }
int gettid(void) { return (int)__syscall(15, 0, 0, 0); }
int cpu_count(void) { return (int)__syscall(16, 0, 0, 0); }

int isatty(int fd) { return __syscall(54, fd, 0, 0) == 1; }

/* Standard input that is not the keyboard (a pipe or a file) is read a
   block at a time, so getchar() does not cost a system call per character.
   Keys are never buffered. */
char __in_buf[4096];
i64 __in_len;
i64 __in_pos;
int __in_kind; /* 0 not known yet, 1 keyboard, 2 pipe or file */

int getchar(void) {
    if (!__in_kind) {
        if (isatty(0)) __in_kind = 1;
        else __in_kind = 2;
    }
    if (__in_kind == 1) {
        flush(); /* show any prompt before waiting for a key */
        return (int)__syscall(2, 0, 0, 0);
    }
    if (__in_pos == __in_len) {
        flush(); /* about to wait: push our own output out first */
        __in_len = __syscall(18, 0, (i64)__in_buf, 4096);
        __in_pos = 0;
        if (__in_len <= 0) {
            __in_len = 0;
            return -1;
        }
    }
    return (unsigned char)__in_buf[__in_pos++];
}

int pollchar(void) {
    flush();
    if (__in_kind == 2 && __in_pos < __in_len) return (unsigned char)__in_buf[__in_pos++];
    return (int)__syscall(11, 0, 0, 0);
}

void sleep_ms(u64 ms) {
    flush();
    __syscall(3, ms, 0, 0);
}

u64 uptime_ms(void) { return __syscall(4, 0, 0, 0); }
int getpid(void) { return (int)__syscall(5, 0, 0, 0); }

void *sbrk(i64 increment) {
    i64 r = __syscall(6, increment, 0, 0);
    if (r == -1) return NULL;
    return (void *)r;
}

i64 file_read(const char *name, void *buf, size_t max) { return __syscall(7, (i64)name, (i64)buf, max); }
i64 file_write(const char *name, const void *buf, size_t len) { return __syscall(8, (i64)name, (i64)buf, len); }
void yield(void) { __syscall(9, 0, 0, 0); }

void *shm_open(const char *name, size_t size) {
    i64 r = __syscall(23, (i64)name, size, 0);
    if (r == -1) return NULL;
    return (void *)r;
}

int shm_unlink(const char *name) { return (int)__syscall(24, (i64)name, 0, 0); }
i64 file_list(int index, char *name, size_t max) { return __syscall(25, index, (i64)name, max); }
int file_delete(const char *name) { return (int)__syscall(26, (i64)name, 0, 0); }
int getcwd(char *buf, size_t max) { return (void)max, (int)__syscall(39, (i64)buf, 0, 0); }
int chdir(const char *path) { return (int)__syscall(40, (i64)path, 0, 0); }
int mkdir(const char *path) { return (int)__syscall(41, (i64)path, 0, 0); }
int rmdir(const char *path) { return (int)__syscall(42, (i64)path, 0, 0); }

/* Signals: 2 interrupt, 15 terminate, 9 kill. (c-comp has no function
   pointers, so programs it builds cannot install handlers.) */
int kill(int pid, int number) { return (int)__syscall(45, pid, number, 0); }

/* Environment variables. getenv copies the value into buf (NUL-terminated)
   and returns its length, or -1 if the variable is not set. */
i64 getenv(const char *name, char *buf, size_t max) { return __syscall(46, (i64)name, (i64)buf, max); }
int setenv(const char *name, const char *value) { return (int)__syscall(47, (i64)name, (i64)value, 0); }
int unsetenv(const char *name) { return (int)__syscall(47, (i64)name, 0, 0); }

/* Seconds since 1970-01-01, from the hardware clock. */
u64 time_now(void) { return __syscall(49, 0, 0, 0); }

/* 0 for a file, 1 for a directory, -1 if missing. out[0] gets the size,
   out[1] the last-modified time. */
int file_stat(const char *path, u64 *out) { return (int)__syscall(50, (i64)path, (i64)out, 0); }

void screen_clear(void) {
    flush();
    __syscall(10, 0, 0, 0);
}

void screen_goto(int col, int row) {
    flush();
    __syscall(10, 1, col, row);
}

void screen_size(int *cols, int *rows) {
    i64 r = __syscall(10, 2, 0, 0);
    *cols = (int)(r >> 16);
    *rows = (int)(r & 0xFFFF);
}

void screen_color(unsigned rgb) {
    flush();
    __syscall(10, 3, rgb, 0);
}

void screen_clear_eol(void) {
    flush();
    __syscall(10, 4, 0, 0);
}

void screen_mirror(int on) {
    flush();
    __syscall(10, 5, on, 0);
}

/* ---- strings and memory ---- */

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
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
    while (*src) *d++ = *src++;
    *d = 0;
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
    const unsigned char *x = a;
    const unsigned char *y = b;
    for (size_t i = 0; i < n; i++) {
        if (x[i] != y[i]) return x[i] - y[i];
    }
    return 0;
}

int atoi(const char *s) {
    int sign = 1;
    int n = 0;
    while (*s == ' ') s++;
    if (*s == '-') {
        sign = -1;
        s++;
    }
    while (*s >= '0' && *s <= '9') n = n * 10 + (*s++ - '0');
    return sign * n;
}

/* ---- heap: power-of-two size classes with free lists ---- */

struct __block {
    size_t size_class;
    struct __block *next_free;
};

struct __block *__free_lists[48];

void *malloc(size_t size) {
    size_t c = 5;
    while (((size_t)1 << c) < size + sizeof(struct __block)) c++;
    if (c >= 48) return NULL;
    struct __block *b = __free_lists[c];
    if (b) {
        __free_lists[c] = b->next_free;
    } else {
        b = sbrk((size_t)1 << c);
        if (!b) return NULL;
        b->size_class = c;
    }
    return b + 1;
}

void free(void *ptr) {
    if (!ptr) return;
    struct __block *b = (struct __block *)ptr - 1;
    b->next_free = __free_lists[b->size_class];
    __free_lists[b->size_class] = b;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr) return malloc(size);
    struct __block *b = (struct __block *)ptr - 1;
    size_t have = ((size_t)1 << b->size_class) - sizeof(struct __block);
    if (size <= have) return ptr;
    void *fresh = malloc(size);
    if (!fresh) return NULL;
    memcpy(fresh, ptr, have);
    free(ptr);
    return fresh;
}

/* ---- whole files and lines ---- */

/* The whole of a file - or of standard input when name is NULL - in a
   malloc'd, NUL-terminated buffer. NULL if the file does not exist. */
char *read_all(const char *name, size_t *len) {
    size_t size = 0;
    size_t cap = 0;
    char *data = NULL;
    if (name) {
        i64 n = file_read(name, NULL, 0);
        if (n < 0) return NULL;
        size = n;
        data = malloc(size + 1);
        if (!data) return NULL;
        file_read(name, data, size);
    } else {
        for (;;) {
            if (size + 4096 + 1 > cap) {
                cap = cap ? cap * 2 : 8192;
                data = realloc(data, cap);
                if (!data) return NULL;
            }
            i64 n = read(0, data + size, 4096);
            if (n <= 0) break;
            size += n;
        }
    }
    data[size] = 0;
    if (len) *len = size;
    return data;
}

/* Cut text into lines in place. Returns a malloc'd array of the lines. */
char **split_lines(char *text, int *count) {
    int cap = 64;
    int n = 0;
    char **lines = malloc(sizeof(char *) * cap);
    char *p = text;
    while (*p) {
        if (n == cap) {
            cap = cap * 2;
            lines = realloc(lines, sizeof(char *) * cap);
        }
        lines[n++] = p;
        while (*p && *p != 10) p++;
        if (*p) *p++ = 0;
    }
    *count = n;
    return lines;
}

/* ---- console output (buffered until a newline) ---- */

char __out_buf[512];
int __out_len;

void flush(void) {
    if (__out_len) {
        write(__out_buf, __out_len);
        __out_len = 0;
    }
}

int putchar(int c) {
    __out_buf[__out_len++] = (char)c;
    if (c == '\n' || __out_len == 512) flush();
    return c;
}

int puts(const char *s) {
    while (*s) putchar(*s++);
    putchar('\n');
    return 0;
}

void __print_number(u64 value, int base, int negative, int width, int pad) {
    char digits[24];
    int n = 0;
    do {
        digits[n++] = "0123456789abcdef"[value % base];
        value = value / base;
    } while (value);
    int len = n + negative;
    if (negative && pad == '0') putchar('-');
    while (len++ < width) putchar(pad);
    if (negative && pad != '0') putchar('-');
    while (n) putchar(digits[--n]);
}

/* Supports %d %i %u %x %c %s %p %%, l/ll, and widths like %5d and %08x.
   Arguments arrive in consecutive 8-byte stack slots right after fmt. */
int printf(const char *fmt, ...) {
    i64 *ap = (i64 *)&fmt + 1;
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            putchar(*fmt);
            continue;
        }
        fmt++;
        int pad = ' ';
        int width = 0;
        int longs = 0;
        if (*fmt == '0') {
            pad = '0';
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        while (*fmt == 'l') {
            longs++;
            fmt++;
        }
        int c = *fmt;
        if (c == 'd' || c == 'i') {
            i64 v = *ap++;
            if (!longs) v = (int)v;
            if (v < 0) {
                __print_number(0 - (u64)v, 10, 1, width, pad);
            } else {
                __print_number(v, 10, 0, width, pad);
            }
        } else if (c == 'u' || c == 'x') {
            u64 v = *ap++;
            if (!longs) v = (unsigned)v;
            __print_number(v, c == 'x' ? 16 : 10, 0, width, pad);
        } else if (c == 'p') {
            putchar('0');
            putchar('x');
            __print_number(*ap++, 16, 0, 0, ' ');
        } else if (c == 'c') {
            putchar((int)*ap++);
        } else if (c == 's') {
            const char *s = (const char *)*ap++;
            if (!s) s = "(null)";
            int len = strlen(s);
            while (len++ < width) putchar(' ');
            while (*s) putchar(*s++);
        } else if (c == '%') {
            putchar('%');
        } else if (c == 0) {
            break;
        } else {
            putchar('%');
            putchar(c);
        }
    }
    return 0;
}

/* Read a line with echo and backspace. Returns its length. */
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

/* ---- startup: split the command line and call main ---- */

int main(int argc, char **argv);
char *__argv[16];

void __start(char *cmdline) {
    int argc = 0;
    char *p = cmdline;
    while (*p && argc < 15) {
        while (*p == ' ') *p++ = 0;
        if (!*p) break;
        __argv[argc++] = p;
        while (*p && *p != ' ') p++;
    }
    exit(main(argc, __argv));
}
