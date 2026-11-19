/* edit.exe - a full-screen text editor. Usage: edit <file>
   Arrows / Home / End / PgUp / PgDn move, Ctrl+S saves, Ctrl+Q quits. */
#include "rdx.h"

typedef struct {
    char *text;
    int len, cap;
} Line;

static Line *lines;
static int line_count, line_cap;
static int cur_col, cur_line; /* cursor position in the file */
static int top, left;         /* first visible line / column */
static int cols, rows;        /* screen size; the last row is the status bar */
static int modified, quit_armed;
static const char *filename;
static char message[96];

static void line_reserve(Line *l, int need) {
    if (need <= l->cap) return;
    int cap = l->cap ? l->cap : 32;
    while (cap < need) cap *= 2;
    l->text = realloc(l->text, (size_t)cap);
    l->cap = cap;
}

static void insert_line(int at, const char *text, int len) {
    if (line_count == line_cap) {
        line_cap = line_cap ? line_cap * 2 : 256;
        lines = realloc(lines, sizeof(Line) * (size_t)line_cap);
    }
    memmove(&lines[at + 1], &lines[at], sizeof(Line) * (size_t)(line_count - at));
    Line fresh = {0};
    line_reserve(&fresh, len + 1);
    memcpy(fresh.text, text, (size_t)len);
    fresh.len = len;
    lines[at] = fresh;
    line_count++;
}

static void delete_line(int at) {
    free(lines[at].text);
    memmove(&lines[at], &lines[at + 1], sizeof(Line) * (size_t)(line_count - at - 1));
    line_count--;
}

static void insert_char(int c) {
    Line *l = &lines[cur_line];
    line_reserve(l, l->len + 1);
    memmove(l->text + cur_col + 1, l->text + cur_col, (size_t)(l->len - cur_col));
    l->text[cur_col++] = (char)c;
    l->len++;
    modified = 1;
}

static void split_line(void) {
    Line *l = &lines[cur_line];
    int tail = l->len - cur_col;
    /* insert_line may move the array, so copy what it needs first. */
    char *rest = malloc((size_t)tail + 1);
    memcpy(rest, l->text + cur_col, (size_t)tail);
    l->len = cur_col;
    insert_line(cur_line + 1, rest, tail);
    free(rest);
    cur_line++;
    cur_col = 0;
    modified = 1;
}

/* Join line `at + 1` onto the end of line `at`. */
static void join_lines(int at) {
    Line *a = &lines[at], *b = &lines[at + 1];
    line_reserve(a, a->len + b->len);
    memcpy(a->text + a->len, b->text, (size_t)b->len);
    a->len += b->len;
    delete_line(at + 1);
    modified = 1;
}

static void load(void) {
    i64 size = file_read(filename, NULL, 0);
    if (size <= 0) {
        insert_line(0, "", 0);
        strcpy(message, size < 0 ? "new file" : "empty file");
        return;
    }
    char *data = malloc((size_t)size);
    file_read(filename, data, (size_t)size);
    /* Split into lines, expanding tabs and dropping carriage returns. */
    char *buf = malloc((size_t)size * 4 + 1);
    int n = 0;
    for (i64 i = 0; i <= size; i++) {
        if (i == size || data[i] == '\n') {
            if (i < size || n > 0) insert_line(line_count, buf, n);
            n = 0;
        } else if (data[i] == '\t') {
            do buf[n++] = ' ';
            while (n % 4);
        } else if (data[i] != '\r') {
            buf[n++] = data[i];
        }
    }
    if (line_count == 0) insert_line(0, "", 0);
    free(buf);
    free(data);
}

static void save(void) {
    size_t total = 0;
    for (int i = 0; i < line_count; i++) total += (size_t)lines[i].len + 1;
    char *data = malloc(total + 1);
    size_t n = 0;
    for (int i = 0; i < line_count; i++) {
        memcpy(data + n, lines[i].text, (size_t)lines[i].len);
        n += (size_t)lines[i].len;
        data[n++] = '\n';
    }
    if (file_write(filename, data, n) == 0) {
        modified = 0;
        strcpy(message, "saved");
    } else {
        strcpy(message, "SAVE FAILED");
    }
    free(data);
}

static void draw(void) {
    int text_rows = rows - 1;
    /* Keep the cursor on screen. */
    if (cur_line < top) top = cur_line;
    if (cur_line >= top + text_rows) top = cur_line - text_rows + 1;
    if (cur_col < left) left = cur_col;
    if (cur_col >= left + cols) left = cur_col - cols + 1;

    for (int r = 0; r < text_rows; r++) {
        screen_goto(0, r);
        int n = top + r;
        if (n < line_count) {
            Line *l = &lines[n];
            int visible = l->len - left;
            if (visible > cols) visible = cols;
            if (visible > 0) write(l->text + left, (size_t)visible);
            if (visible < cols) screen_clear_eol();
        } else {
            screen_color(0x6C7380);
            write("~", 1);
            screen_color(COLOR_DEFAULT);
            screen_clear_eol();
        }
    }

    screen_goto(0, rows - 1);
    screen_color(0x57C7FF);
    printf(" %s%s  line %d/%d col %d  |  ^S save  ^Q quit  %s", filename, modified ? " *" : "", cur_line + 1,
           line_count, cur_col + 1, message);
    flush();
    screen_color(COLOR_DEFAULT);
    screen_clear_eol();
    screen_goto(cur_col - left, cur_line - top);
}

static void clamp_col(void) {
    if (cur_col > lines[cur_line].len) cur_col = lines[cur_line].len;
}

/* Ctrl+C (or kill) only raises this flag; without a handler the editor
   would simply be ended, taking unsaved work with it. The main loop's
   getchar() returns as soon as the handler has run, and deals with it. */
static volatile int interrupted;

static void on_interrupt(int number) {
    (void)number;
    interrupted = 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: edit <file>\n");
        return 1;
    }
    signal(SIGINT, on_interrupt);
    signal(SIGTERM, on_interrupt);
    filename = argv[1];
    screen_size(&cols, &rows);
    load();
    screen_mirror(0);
    screen_clear();

    for (;;) {
        draw();
        int c = getchar();
        message[0] = 0;
        int armed = quit_armed;
        quit_armed = 0;
        if (interrupted) {
            /* Leave at once if there is nothing to lose; otherwise stay. */
            interrupted = 0;
            if (!modified) break;
            strcpy(message, "unsaved changes - ^S saves, ^Q twice discards them");
            continue;
        }

        if (c == CTRL('Q')) {
            if (!modified || armed) break;
            quit_armed = 1;
            strcpy(message, "unsaved changes - press ^Q again to discard them");
        } else if (c == CTRL('S')) {
            save();
        } else if (c == KEY_UP) {
            if (cur_line > 0) cur_line--;
            clamp_col();
        } else if (c == KEY_DOWN) {
            if (cur_line < line_count - 1) cur_line++;
            clamp_col();
        } else if (c == KEY_LEFT) {
            if (cur_col > 0) {
                cur_col--;
            } else if (cur_line > 0) {
                cur_line--;
                cur_col = lines[cur_line].len;
            }
        } else if (c == KEY_RIGHT) {
            if (cur_col < lines[cur_line].len) {
                cur_col++;
            } else if (cur_line < line_count - 1) {
                cur_line++;
                cur_col = 0;
            }
        } else if (c == KEY_HOME) {
            cur_col = 0;
        } else if (c == KEY_END) {
            cur_col = lines[cur_line].len;
        } else if (c == KEY_PAGE_UP) {
            cur_line -= rows - 2;
            if (cur_line < 0) cur_line = 0;
            clamp_col();
        } else if (c == KEY_PAGE_DOWN) {
            cur_line += rows - 2;
            if (cur_line > line_count - 1) cur_line = line_count - 1;
            clamp_col();
        } else if (c == '\n') {
            split_line();
        } else if (c == 8) {
            if (cur_col > 0) {
                Line *l = &lines[cur_line];
                memmove(l->text + cur_col - 1, l->text + cur_col, (size_t)(l->len - cur_col));
                l->len--;
                cur_col--;
                modified = 1;
            } else if (cur_line > 0) {
                cur_col = lines[cur_line - 1].len;
                join_lines(cur_line - 1);
                cur_line--;
            }
        } else if (c == KEY_DELETE) {
            Line *l = &lines[cur_line];
            if (cur_col < l->len) {
                memmove(l->text + cur_col, l->text + cur_col + 1, (size_t)(l->len - cur_col - 1));
                l->len--;
                modified = 1;
            } else if (cur_line < line_count - 1) {
                join_lines(cur_line);
            }
        } else if (c == '\t') {
            do insert_char(' ');
            while (cur_col % 4);
        } else if (c >= 32 && c < 127) {
            insert_char(c);
        }
    }

    screen_clear();
    screen_mirror(1);
    return 0;
}
