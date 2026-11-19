/* terminal - a terminal window on the desktop, with a shell in it.

   The shell runs in a pseudo-terminal, so to it this window is just like
   the console: it edits lines, shows its prompt, and full-screen programs
   (edit, top, more) work. What the shell prints arrives here, colour and
   cursor commands included (as ANSI escape sequences), and is drawn as a
   grid of characters. Keys typed while the window has the focus go to the
   shell. Closing the window hangs up the shell.

   Started with a command - which is how the desktop and the file manager
   run text programs - the window is titled "Terminal - name.exe" and stays
   open when the program finishes, until a key is pressed, so its output can
   be read. The shell in a terminal window also renames the window after
   whatever it is running, with the standard escape sequence for that
   (ESC ] 0 ; title BEL).

   Usage: terminal [command]    (default: sh) */
#include "gui.h"

#define COLS 80
#define ROWS 25
#define PAD 4
#define BG 0x0B0E14
#define FG 0xD0D4DC

typedef struct {
    char ch;
    unsigned fg;
} Cell;

static Cell cells[ROWS][COLS];
static int cx, cy;
static unsigned fg = FG;
static gui_window *win;
static int focused = 1;
/* The window's title when nothing else is running in it. */
static char base_title[40] = "Terminal";
static char shown_title[40];

static void set_title(const char *program) {
    char title[40];
    if (program && *program) {
        strcpy(title, "Terminal - ");
        size_t n = strlen(title);
        for (size_t i = 0; program[i] && n < sizeof(title) - 1; i++) title[n++] = program[i];
        title[n] = 0;
    } else {
        strcpy(title, base_title);
    }
    if (strcmp(title, shown_title) != 0) {
        strcpy(shown_title, title);
        gui_set_title(win, title);
    }
}

/* Rows that need drawing again. */
static int dirty_top = ROWS, dirty_bottom = -1;

static void mark(int row) {
    if (row < dirty_top) dirty_top = row;
    if (row > dirty_bottom) dirty_bottom = row;
}

static void clear_screen(void) {
    for (int r = 0; r < ROWS; r++) {
        for (int c = 0; c < COLS; c++) {
            cells[r][c].ch = ' ';
            cells[r][c].fg = FG;
        }
    }
    cx = cy = 0;
    mark(0);
    mark(ROWS - 1);
}

static void newline(void) {
    cx = 0;
    if (cy + 1 < ROWS) {
        cy++;
        return;
    }
    memmove(cells[0], cells[1], sizeof(cells[0]) * (ROWS - 1));
    for (int c = 0; c < COLS; c++) {
        cells[ROWS - 1][c].ch = ' ';
        cells[ROWS - 1][c].fg = FG;
    }
    mark(0);
    mark(ROWS - 1);
}

/* ---- ANSI escape sequences: the same set the kernel console handles ---- */

static const unsigned ANSI[16] = {0x2E3440, 0xFF5C57, 0x5AF78E, 0xF3F99D, 0x57C7FF, 0xFF6AC1, 0x9AEDFE, 0xD0D4DC,
                                  0x6C7380, 0xFF8A85, 0x8CFFB0, 0xFAFFC8, 0x8FDBFF, 0xFF9CD8, 0xC6F7FF, 0xFFFFFF};
static int esc, params[8], nparams;
static char osc[64]; /* the text of an ESC ] ... BEL sequence */
static int osc_len;

/* "0;title" or "2;title": set the window title. */
static void operating_system_command(void) {
    osc[osc_len] = 0;
    if ((osc[0] == '0' || osc[0] == '2') && osc[1] == ';') set_title(osc + 2);
}

static int param(int i) { return i < nparams ? params[i] : 0; }

static void control_sequence(char last) {
    int count = param(0) > 0 ? param(0) : 1;
    switch (last) {
    case 'A': cy = cy - count < 0 ? 0 : cy - count; break;
    case 'B': cy = cy + count >= ROWS ? ROWS - 1 : cy + count; break;
    case 'C': cx = cx + count >= COLS ? COLS - 1 : cx + count; break;
    case 'D':
        while (count--) {
            if (cx > 0) {
                cx--;
            } else if (cy > 0) {
                cy--;
                cx = COLS - 1;
            }
        }
        break;
    case 'H':
    case 'f':
        cy = (param(0) > 0 ? param(0) : 1) - 1;
        cx = (param(1) > 0 ? param(1) : 1) - 1;
        if (cy >= ROWS) cy = ROWS - 1;
        if (cx >= COLS) cx = COLS - 1;
        break;
    case 'J':
        if (param(0) == 2) {
            int x = cx, y = cy;
            clear_screen();
            cx = x;
            cy = y;
        }
        break;
    case 'K':
        for (int c = cx; c < COLS; c++) {
            cells[cy][c].ch = ' ';
            cells[cy][c].fg = FG;
        }
        break;
    case 'm':
        if (nparams == 0) fg = FG;
        for (int i = 0; i < nparams; i++) {
            int p = params[i];
            if (p == 0 || p == 39) fg = FG;
            else if (p >= 30 && p <= 37) fg = ANSI[p - 30];
            else if (p >= 90 && p <= 97) fg = ANSI[p - 90 + 8];
            else if (p == 38 && param(i + 1) == 2) {
                fg = (unsigned)(param(i + 2) & 0xFF) << 16 | (unsigned)(param(i + 3) & 0xFF) << 8 |
                     (unsigned)(param(i + 4) & 0xFF);
                i += 4;
            }
        }
        break;
    }
    mark(cy);
}

static void put(unsigned char b) {
    if (esc == 1) {
        esc = b == '[' ? 2 : b == ']' ? 3 : 0;
        nparams = 0;
        osc_len = 0;
        memset(params, 0, sizeof(params));
        return;
    }
    if (esc == 3 || esc == 4) { /* ESC ] text, ended by BEL or by ESC \ */
        if (b == 7 || esc == 4) {
            esc = 0;
            operating_system_command();
        } else if (b == 0x1B) {
            esc = 4;
        } else if (osc_len < (int)sizeof(osc) - 1) {
            osc[osc_len++] = (char)b;
        }
        return;
    }
    if (esc == 2) {
        if (b >= '0' && b <= '9') {
            if (nparams == 0) nparams = 1;
            if (nparams <= 8) params[nparams - 1] = params[nparams - 1] * 10 + (b - '0');
        } else if (b == ';') {
            nparams = (nparams < 1 ? 1 : nparams) + 1;
            if (nparams > 8) nparams = 8;
        } else if (b < 0x20 || b > 0x3F) {
            esc = 0;
            control_sequence((char)b);
        }
        return;
    }
    mark(cy);
    switch (b) {
    case 0x1B: esc = 1; return;
    case '\n': newline(); break;
    case '\r': cx = 0; break;
    case 8:
        if (cx > 0) {
            cx--;
            cells[cy][cx].ch = ' ';
        }
        break;
    case '\t':
        do put(' ');
        while (cx % 4);
        break;
    default:
        if (b < 32) break;
        if (cx >= COLS) newline();
        cells[cy][cx].ch = (char)b;
        cells[cy][cx].fg = fg;
        cx++;
    }
    mark(cy);
}

/* ---- drawing ---- */

static int drawn_cy;

static void render(void) {
    /* The row the cursor was drawn on needs repainting too. */
    mark(drawn_cy);
    mark(cy);
    if (dirty_bottom < dirty_top) return;
    for (int r = dirty_top; r <= dirty_bottom; r++) {
        int y = PAD + r * CHAR_HEIGHT;
        fill_rect(&win->pixels, 0, y, win->width, CHAR_HEIGHT, BG);
        for (int c = 0; c < COLS; c++) {
            if (cells[r][c].ch != ' ') draw_char(&win->pixels, PAD + c * CHAR_WIDTH, y, cells[r][c].ch, cells[r][c].fg);
        }
    }
    /* The cursor: a bar under the character, solid when focused. */
    int x = PAD + (cx < COLS ? cx : COLS - 1) * CHAR_WIDTH, y = PAD + cy * CHAR_HEIGHT;
    if (focused) fill_rect(&win->pixels, x, y + CHAR_HEIGHT - 3, CHAR_WIDTH, 3, FG);
    else draw_rect(&win->pixels, x, y + CHAR_HEIGHT - 3, CHAR_WIDTH, 3, 0x6C7380);

    drawn_cy = cy;
    gui_update(win, 0, PAD + dirty_top * CHAR_HEIGHT, win->width, (dirty_bottom - dirty_top + 1) * CHAR_HEIGHT);
    dirty_top = ROWS;
    dirty_bottom = -1;
}

/* Show a line of our own in the window. */
static void say(const char *text) {
    for (const char *p = text; *p; p++) put((unsigned char)*p);
}

int main(int argc, char **argv) {
    GUI_PROGRAM();
    char command[256];
    strcpy(command, argc > 1 ? command_tail() : "sh");
    int for_program = argc > 1;
    if (for_program) {
        /* The program's name, as the title: "Terminal - name.exe". */
        char name[48];
        size_t n = 0;
        while (argv[1][n] && n < 40) {
            name[n] = argv[1][n];
            n++;
        }
        name[n] = 0;
        const char *base = name;
        for (const char *p = name; *p; p++) {
            if (*p == '/') base = p + 1;
        }
        size_t len = strlen(base);
        strcpy(base_title, "Terminal - ");
        strcpy(base_title + 11, base);
        if (len < 4 || strcmp(base + len - 4, ".exe") != 0) strcpy(base_title + 11 + len, ".exe");
        base_title[39] = 0;
    }
    if (gui_connect() < 0) {
        printf("terminal: the desktop is not running (start it with: desktop)\n");
        return 1;
    }
    win = gui_window_new(COLS * CHAR_WIDTH + 2 * PAD, ROWS * CHAR_HEIGHT + 2 * PAD, base_title);
    if (!win) return 1;
    strcpy(shown_title, base_title);
    /* The shell checks this to know it may rename the window. */
    setenv("TERM_WINDOW", "1");
    fill_rect(&win->pixels, 0, 0, win->width, win->height, BG);
    clear_screen();

    int ends[2];
    if (pty_open(ends) != 0) return 1;
    int master = ends[0];
    pty_resize(master, COLS, ROWS);
    int child = spawn_fds(command, ends[1], ends[1], ends[1]);
    close(ends[1]);
    /* A program with no shell around it: Ctrl+C goes straight to it, and
       closing the window hangs it up. */
    if (for_program && child >= 0) pty_foreground(master, &child, 1);
    if (child < 0) {
        say("cannot start ");
        say(command);
        say(": ");
        say(last_error());
    }
    render();

    char buf[4096];
    int window_closed = 0;
    for (;;) {
        pollfd watch[2] = {{gui_fd(), POLLIN, 0}, {master, POLLIN, 0}};
        int ready = poll(watch, 2, -1);
        if (ready == EINTR) continue;
        if (ready < 0) break;
        if (watch[1].revents & (POLLIN | POLLHUP)) {
            i64 n = read(master, buf, sizeof(buf));
            if (n <= 0) break; /* the shell has exited */
            for (i64 i = 0; i < n; i++) put((unsigned char)buf[i]);
            render();
        }
        if (watch[0].revents) {
            gui_message m;
            int got, closing = 0;
            while ((got = gui_poll(&m)) > 0) {
                if (m.type == MSG_KEY) {
                    char key = (char)m.a;
                    fd_write(master, &key, 1);
                } else if (m.type == MSG_CLOSE) {
                    closing = 1;
                } else if (m.type == MSG_FOCUS) {
                    focused = m.a;
                    render();
                }
            }
            if (got < 0 || closing) { /* the desktop has gone, or the window was closed */
                window_closed = 1;
                break;
            }
        }
    }
    /* A program run in its own window: keep its output on screen until it
       has been read. */
    if (for_program && !window_closed && child >= 0) {
        int status = wait(child);
        set_title(NULL);
        fg = 0x6C7380;
        say(cx ? "\n\n[" : "\n[");
        say(base_title + 11);
        if (status == 0) {
            say(" finished]");
        } else if (status == -1) {
            say(" was stopped]");
        } else {
            char code[16];
            int k = 0, v = status < 0 ? -status : status;
            do code[k++] = (char)('0' + v % 10);
            while (v /= 10);
            say(" exited with code ");
            if (status < 0) put('-');
            while (k) put((unsigned char)code[--k]);
            say("]");
        }
        say("  Press any key to close this window.");
        render();
        gui_message m;
        int got;
        while ((got = gui_next(&m, -1)) >= 0) {
            if (got > 0 && (m.type == MSG_KEY || m.type == MSG_CLOSE)) break;
        }
    }
    gui_window_close(win);
    close(master); /* hangs up whatever is still running in it */
    return 0;
}
