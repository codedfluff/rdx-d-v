/* rdx.h - the RDX C runtime. Include this, write main(), run build.cmd. */
#pragma once

typedef __SIZE_TYPE__ size_t;
typedef long long i64;
typedef unsigned long long u64;
typedef __builtin_va_list va_list;
#define va_start(ap, last) __builtin_va_start(ap, last)
#define va_arg(ap, type) __builtin_va_arg(ap, type)
#define va_end(ap) __builtin_va_end(ap)
#define NULL ((void *)0)

/* ---- system calls (see src/syscall.rs) ---- */
i64 syscall(i64 number, i64 a, i64 b, i64 c);
i64 syscall5(i64 number, i64 a, i64 b, i64 c, i64 d, i64 e);
__attribute__((noreturn)) void exit(int code);
i64 write(const char *buf, size_t len);         /* print bytes to the console */
int getchar(void);                              /* wait for one key (or byte of piped input; -1 at its end) */
int isatty(int fd);                             /* is this descriptor the console, not a pipe or file? */
int pollchar(void);                             /* a key if one is waiting, else -1 */
void sleep_ms(u64 ms);
u64 uptime_ms(void);                            /* milliseconds since boot */
int getpid(void);
void *sbrk(i64 increment);                      /* grow the heap; NULL on failure */
i64 file_read(const char *name, void *buf, size_t max);  /* returns file size, -1 if missing */
i64 file_write(const char *name, const void *buf, size_t len);
void yield(void);

/* ---- standard input/output, pipes and other programs ----
   Every program has numbered "file descriptors": 0 is standard input, 1
   standard output. Normally both are the console, but the shell can connect
   them to files (prog > out.txt) or to other programs (a | b). */
i64 read(int fd, void *buf, size_t max);        /* bytes read; 0 = end of input */
i64 recv_nb(int fd, void *buf, size_t max);     /* like read, but -2 = nothing ready yet */
i64 fd_write(int fd, const void *buf, size_t len);    /* writes it all; -1 on error */
int pipe(int fds[2]);                           /* fds[0] = read end, fds[1] = write end */
int close(int fd);
/* Start another program. Its stdin/stdout are copies of the given
   descriptors of yours (-1 = same as your own). Returns its process ID. */
int spawn(const char *cmdline, int stdin_fd, int stdout_fd);
int wait(int pid);                              /* its exit code, or -1 if it was killed */
int console_key(void);                          /* a key from the keyboard even if stdin is a pipe */
/* spawn returns SPAWN_NO_PROGRAM if nothing of that name was found, or
   SPAWN_FAILED if it could not be started (last_error() says why). */
#define SPAWN_NO_PROGRAM (-1)
#define SPAWN_FAILED (-2)
/* Open a file as a descriptor, e.g. to hand to spawn. What is written to
   one becomes the file when it is closed (by everyone holding a copy). */
#define OPEN_READ 0
#define OPEN_WRITE 1
#define OPEN_APPEND 2
int open(const char *path, int mode);           /* descriptor, or -1 */
/* Say which processes Ctrl+C should go to (count 0: none). A shell names
   the programs it has just started and is waiting for. */
int set_foreground(const int *pids, int count);
/* Why the last system call that failed, failed - in plain words. */
const char *last_error(void);
/* Everything on the command line after the program's name, exactly as it
   was typed (argv splits it at spaces; this keeps the spacing). */
const char *command_tail(void);

/* ---- files and directories ---- */
i64 file_list(int index, char *name, size_t max);     /* size; -2 if a directory; -1 past the end */
int file_delete(const char *name);
int getcwd(char *buf, size_t max);              /* the working directory, e.g. "/docs" */
int chdir(const char *path);                    /* change the working directory */
int mkdir(const char *path);
int rmdir(const char *path);                    /* remove an empty directory */
/* 0 for a file, 1 for a directory, -1 if there is nothing there. size and
   modified (seconds since 1970; 0 = unknown) may be NULL. */
int file_stat(const char *path, i64 *size, u64 *modified);
/* The whole of a file - or of standard input when name is NULL - in a
   malloc'd, NUL-terminated buffer. NULL if the file does not exist. */
char *read_all(const char *name, size_t *len);
/* Copy a file to standard output. Returns 0, or 1 if there is no such file. */
int print_file(const char *path);
/* Cut text into lines in place (newlines become NULs). Returns a malloc'd
   array of pointers to the lines and stores how many there are. */
char **split_lines(char *text, int *count);

/* ---- networking ----
   Addresses are 4 bytes (IPv4). Sockets are file descriptors: read(),
   fd_write() and close() work on them like on anything else. */
int net_resolve(const char *name, unsigned char ip[4]);  /* name or "1.2.3.4" -> address; 0 if found */
int net_info(unsigned char out[16]);            /* our address, mask, gateway, DNS; 0 once configured */

/* TCP: a reliable two-way stream of bytes. */
int tcp_connect(const unsigned char ip[4], int port);    /* descriptor, or -1 */
int tcp_connect_name(const char *host, int port);        /* looks the name up first */
int tcp_listen(int port);                       /* descriptor to accept connections on */
int tcp_accept(int listen_fd);                  /* waits for a connection; returns its descriptor */
int tcp_shutdown(int fd);                       /* finished sending (peer sees end of input); can still read */

/* UDP: individual packets, with no guarantee of delivery. */
int udp_open(int port);                         /* 0 = any free port */
i64 udp_sendto(int fd, const unsigned char ip[4], int port, const void *buf, size_t len);
/* Waits for a packet. ip and port (either may be NULL) get the sender's. */
i64 udp_recvfrom(int fd, void *buf, size_t max, unsigned char ip[4], int *port);

/* ---- waiting for several things at once ----
   poll() waits until at least one of the descriptors is ready: has input
   (POLLIN), can take output (POLLOUT), or has been closed at the other end
   (POLLHUP, always reported). A negative fd is skipped. timeout_ms -1
   waits as long as it takes. Returns how many entries have revents set, 0
   if the time ran out, or EINTR if a signal handler ran. */
typedef struct {
    int fd;
    short events;
    short revents;
} pollfd;
#define POLLIN 1
#define POLLOUT 4
#define POLLHUP 16
#define POLLNVAL 32
int poll(pollfd *fds, int count, int timeout_ms);

/* ---- pseudo-terminals: being the terminal for another program ----
   pty_open gives two descriptors: fds[0] is the terminal's end, fds[1] the
   program's. Hand fds[1] to a program (spawn_fds) as its input and output,
   and it behaves exactly as on the console: isatty() is true, Ctrl+C works,
   and its screen control arrives at fds[0] as ANSI escape sequences. Write
   keystrokes to fds[0]; read what it prints from fds[0]. */
int pty_open(int fds[2]);
int pty_resize(int fd, int cols, int rows);     /* the screen size it is told */
/* Which processes Ctrl+C typed at the pty, and closing its terminal end,
   are sent to. (A shell running in it does this itself.) */
int pty_foreground(int fd, const int *pids, int count);
int spawn_fds(const char *cmdline, int stdin_fd, int stdout_fd, int stderr_fd);

/* ---- graphics: the whole screen, for one program at a time ----
   display_open takes over the screen, keyboard and mouse (the text console
   steps aside until this program closes the descriptor or exits). It
   fills in `info` and returns a descriptor that delivers input events. */
typedef struct {
    unsigned *pixels;                           /* the screen itself */
    int width, height;
    int stride;                                 /* pixels from one row to the next */
    int format;                                 /* 0: 0xRRGGBB as is; 1: red and blue swapped */
    int mouse_x, mouse_y;                       /* where the pointer is now */
} display_info;
int display_open(display_info *info);           /* -1 if it cannot (see last_error) */

typedef struct {
    unsigned kind;                              /* EVENT_KEY or EVENT_MOUSE */
    unsigned code;                              /* the key (as getchar gives it), or MOUSE_ buttons held */
    short dx, dy;                               /* how far the mouse moved */
    short x, y;                                 /* where the pointer is now */
} input_event;
#define EVENT_KEY 1
#define EVENT_MOUSE 2
#define MOUSE_LEFT 1
#define MOUSE_RIGHT 2
#define MOUSE_MIDDLE 4
/* Waits for at least one event, then returns how many it stored (or
   EINTR). Use poll() first to wait on other descriptors at the same time. */
int read_events(int fd, input_event *events, int max);

/* Drawing, into any block of pixels: the screen, or one of your own (draw
   off-screen, then copy the changed part to the screen with present).
   Colours are 0xRRGGBB. Everything is clipped to the canvas. */
typedef struct {
    unsigned *pixels;
    int width, height, stride;
} canvas;
canvas canvas_new(int width, int height);       /* pixels from malloc; NULL if out of memory */
void fill_rect(canvas *c, int x, int y, int w, int h, unsigned color);
void draw_rect(canvas *c, int x, int y, int w, int h, unsigned color);   /* outline */
void draw_line(canvas *c, int x0, int y0, int x1, int y1, int thickness, unsigned color);
void draw_char(canvas *c, int x, int y, char ch, unsigned color);       /* 8 x 16 font, no background */
void draw_text(canvas *c, int x, int y, const char *text, unsigned color);
/* Text drawn bigger: each pixel of the font becomes a scale x scale block. */
void draw_text_scaled(canvas *c, int x, int y, const char *text, int scale, unsigned color);
/* `len` characters in cells `cell` pixels wide and twice as high (8 is the
   font's own size; 12 and 16 enlarge it), optionally bold and/or slanted. */
#define GLYPH_BOLD 1
#define GLYPH_ITALIC 2
void draw_glyphs(canvas *c, int x, int y, const char *text, int len, int cell, unsigned color, int flags);
#define CHAR_WIDTH 8
#define CHAR_HEIGHT 16
/* Copy part of one canvas onto another. */
void blit(canvas *dst, int dx, int dy, const canvas *src, int sx, int sy, int w, int h);
/* Copy part of a canvas (which covers the whole screen) to the same place
   on the screen, converting colours to the screen's format. */
void present(const display_info *screen, const canvas *c, int x, int y, int w, int h);
/* The screen itself as a canvas (only right for format 0 screens). */
canvas screen_canvas(const display_info *screen);

/* ---- signals: a way to be told "please stop" instead of just being ended.
   Ctrl+C sends SIGINT to the program in the foreground; kill sends SIGTERM
   (or whatever it is asked to). A program with no handler for a signal is
   ended by it. With one, the handler runs - on the program's first thread,
   interrupting whatever that was doing, even a read() that was waiting -
   and the program then carries on where it was. SIGKILL cannot be handled.

   A call that was waiting when the handler ran (read, getchar, wait,
   tcp_accept, ...) does not go back to waiting: it returns EINTR, so a
   loop like `while (!stop) { n = read(...); ... }` notices the flag.

   Keep handlers small: set a flag, print a line, or clean up and exit(). */
#define EINTR (-4)
#define SIGHUP 1                                /* the terminal went away */
#define SIGINT 2
#define SIGKILL 9
#define SIGTERM 15
typedef void (*sighandler_t)(int signal_number);
#define SIG_DFL ((sighandler_t)0)               /* the default: end the program */
#define SIG_IGN ((sighandler_t)1)               /* ignore the signal */
sighandler_t signal(int signal_number, sighandler_t handler);  /* returns the old handler */
int kill(int pid, int signal_number);           /* 0 if the process existed */

/* ---- environment variables: named strings every program inherits from
        the one that started it (the shell's `set NAME=value`) ---- */
char *getenv(const char *name);                 /* NULL if unset; valid until the next getenv */
int setenv(const char *name, const char *value);
int unsetenv(const char *name);
i64 env_entry(int index, char *buf, size_t max);      /* "NAME=value"; -1 past the end */

/* ---- time ---- */
u64 time_now(void);                             /* seconds since 1970-01-01 (hardware clock) */
/* Fill buf with unpredictable bytes, good enough for encryption keys. */
void getrandom(void *buf, size_t len);
void format_time(u64 seconds, char buf[20]);    /* "2026-11-13 14:02:11" */

/* ---- the system: tasks, memory, disk, power ---- */
typedef struct {
    unsigned pid, tid;
    unsigned state;                             /* index into TASK_STATES */
    int cpu;                                    /* core it is running on, or -1 */
    u64 cpu_ms;                                 /* processor time used */
    u64 started_ms;                             /* uptime when it started */
    unsigned user;                              /* 1 = a program, 0 = part of the kernel */
    char name[28];
} proc_info;
#define TASK_STATES {"ready", "running", "sleeping", "waiting", "dead"}
int proc_list(int index, proc_info *out);       /* 0, or -1 past the end */

typedef struct {
    u64 mem_total, mem_used;                    /* physical memory, bytes */
    u64 heap_size, heap_used;                   /* the kernel's own heap */
    u64 disk_capacity, disk_used;               /* the RDX disk (0 if none) */
    u64 cpus, uptime_ms;
    u64 file_bytes, file_count;                 /* everything in the filesystem */
    char version[16];
} sys_info;
int sysinfo(sys_info *out);
void shutdown(void);
void reboot(void);
/* Send one ICMP echo request ("ping"). 0 sent, -1 no network adapter, -2 no
   address yet, -3 still finding the way there (try again in a moment). */
int ping_send(const unsigned char ip[4], int sequence);
i64 ping_reply(int sequence);                   /* uptime_ms() when the answer came, or -1 */
/* Kernel self-tests: 0 starts a kernel worker thread, 1 a kernel thread
   that crashes, 2 panics the kernel. Returns the new thread's ID. */
int kernel_debug(int what);

/* ---- shared memory: a named block several programs can all map and then
        read and write directly ---- */
void *shm_open(const char *name, size_t size);  /* creates it if new; NULL on failure */
int shm_unlink(const char *name);
int shm_close(void *address);                   /* unmap it from this program */

/* ---- local connections: talking to a server on the same machine ----
   A server listens on a name; clients connect to it by that name. Each
   connection is one descriptor at each end, readable and writable both
   ways (and pollable) like a pipe. */
int local_listen(const char *name);             /* descriptor to accept on, or -1 */
int local_accept(int listen_fd);                /* waits for a client; its descriptor */
int local_connect(const char *name);            /* -1 if nothing is listening */

/* ---- threads: several flows of control in one program, sharing its memory.
        On a multi-core machine they really do run at the same time. ---- */
int thread_create(void (*entry)(void *), void *arg);  /* returns a thread ID, or -1 */
void thread_join(int tid);                      /* wait for that thread to finish */
__attribute__((noreturn)) void thread_exit(void);     /* end the calling thread only */
int gettid(void);
int cpu_count(void);                            /* CPU cores the kernel is using */

/* A lock for data that several threads touch. Start it as {0}. */
typedef struct {
    volatile int locked;
} mutex_t;
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* ---- keys: getchar() returns ASCII, or one of these ---- */
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
#define CTRL(letter) ((letter) & 0x1F)          /* e.g. CTRL('S') */

/* ---- full-screen console control ---- */
void screen_clear(void);
void screen_goto(int col, int row);
void screen_size(int *cols, int *rows);
void screen_color(unsigned rgb);                /* 0xRRGGBB text colour */
void screen_clear_eol(void);                    /* blank from cursor to end of line */
void screen_mirror(int on);                     /* copy output to the serial log? */
void screen_left(int cells);                    /* step the cursor back without erasing */
#define COLOR_DEFAULT 0xD0D4DC

/* ---- strings and memory ---- */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
int strncasecmp(const char *a, const char *b, size_t n);  /* ignoring upper/lower case */
char *strchr(const char *s, int c);             /* first c in s, or NULL */
char *strstr(const char *haystack, const char *needle);
int tolower(int c);
int toupper(int c);
char *strcpy(char *dst, const char *src);
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int value, size_t n);
int memcmp(const void *a, const void *b, size_t n);
int atoi(const char *s);
void *malloc(size_t size);
void *realloc(void *ptr, size_t size);
void free(void *ptr);

/* ---- console ---- */
int putchar(int c);
int puts(const char *s);
/* Supports %d %i %u %x %c %s %p %%, the l/ll length modifiers, and a
   zero-padded or space-padded width such as %5d or %08x. */
int printf(const char *fmt, ...);
int vprintf(const char *fmt, va_list ap);
/* Output is buffered until a newline; this pushes it out now. */
void flush(void);
/* Read a line with echo and backspace. Returns its length. */
int readline(char *buf, int max);
