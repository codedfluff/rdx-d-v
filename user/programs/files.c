/* files - a file manager window.

   Lists the folder it is in. Click a folder to go into it, "Up" (or
   Backspace) to go back out. Click a file to see what is in it. Keys: Up
   and Down choose, Enter opens a folder or starts a program (.exe),
   q closes the window. */
#include "gui.h"

#define W 620
#define H 420
#define BAR 30
#define ROW 20
#define LIST_W 330
#define MAX_ENTRIES 256

#define BG 0xF4F5F8
#define BAR_BG 0x2B3245
#define TEXT 0x1C2030
#define DIM 0x6A7287
#define SELECT 0xCFE0FF
#define FOLDER 0x3D7DFF
#define FILE_ICON 0x9AA3B8
#define PROGRAM 0x30A46C

typedef struct {
    char name[64];
    i64 size; /* -2: folder */
} Entry;

static gui_window *win;
static Entry entries[MAX_ENTRIES];
static int count, selected = -1, scroll;
static char preview[24][72];
static int preview_lines;
static char status[80];

static int visible_rows(void) { return (H - BAR - 4) / ROW; }

static void load(void) {
    count = 0;
    char name[64];
    i64 size;
    for (int i = 0; count < MAX_ENTRIES && (size = file_list(i, name, sizeof(name))) != -1; i++) {
        strcpy(entries[count].name, name);
        entries[count].size = size;
        count++;
    }
    selected = -1;
    scroll = 0;
    preview_lines = 0;
    status[0] = 0;
}

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && strcmp(s + n - m, tail) == 0;
}

/* Fill the preview pane with the start of a file. */
static void show(const Entry *e) {
    preview_lines = 0;
    if (e->size == -2) {
        strcpy(preview[preview_lines++], "A folder. Click it again to open.");
        return;
    }
    char head[2048];
    i64 n = file_read(e->name, head, sizeof(head));
    if (n < 0) {
        strcpy(preview[preview_lines++], "(cannot be read)");
        return;
    }
    if (n > (i64)sizeof(head)) n = sizeof(head);
    int binary = 0;
    for (i64 i = 0; i < n; i++) {
        unsigned char c = (unsigned char)head[i];
        if (c < 9 || (c > 13 && c < 32 && c != 27)) binary = 1;
    }
    if (binary) {
        strcpy(preview[preview_lines++], !ends_with(e->name, ".exe") ? "(not text)"
                                         : gui_is_graphical(e->name) == 1 ? "A program with its own window."
                                                                          : "A text program: it opens in a terminal.");
        if (ends_with(e->name, ".exe")) strcpy(preview[preview_lines++], "Click it again, or press Enter, to start it.");
        return;
    }
    int col = 0;
    preview[0][0] = 0;
    for (i64 i = 0; i < n && preview_lines < 24; i++) {
        char c = head[i];
        if (c == '\n' || col == 34) {
            preview[preview_lines][col] = 0;
            preview_lines++;
            col = 0;
            if (c == '\n') continue;
        }
        if (preview_lines < 24 && c != '\r') {
            preview[preview_lines][col++] = c == '\t' ? ' ' : c;
            preview[preview_lines][col] = 0;
        }
    }
    if (col && preview_lines < 24) preview_lines++;
}

static void size_text(char *out, i64 size) {
    if (size == -2) {
        strcpy(out, "folder");
        return;
    }
    const char *unit = " B";
    if (size >= 10240) {
        size /= 1024;
        unit = " KiB";
    }
    char digits[24];
    int n = 0;
    do digits[n++] = (char)('0' + size % 10);
    while (size /= 10);
    int k = 0;
    while (n) out[k++] = digits[--n];
    strcpy(out + k, unit);
}

static void draw(void) {
    canvas *c = &win->pixels;
    fill_rect(c, 0, 0, W, H, BG);
    /* Top bar: Up, and where we are. */
    fill_rect(c, 0, 0, W, BAR, BAR_BG);
    fill_rect(c, 6, 4, 50, BAR - 8, 0x3D4560);
    draw_text(c, 15, 7, "Up", 0xFFFFFF);
    char path[256];
    getcwd(path, sizeof(path));
    draw_text(c, 68, 7, path, 0xFFFFFF);

    /* The list. */
    int rows = visible_rows();
    for (int i = 0; i < rows && scroll + i < count; i++) {
        const Entry *e = &entries[scroll + i];
        int y = BAR + 2 + i * ROW;
        if (scroll + i == selected) fill_rect(c, 0, y, LIST_W, ROW, SELECT);
        unsigned icon = e->size == -2 ? FOLDER : ends_with(e->name, ".exe") ? PROGRAM : FILE_ICON;
        fill_rect(c, 8, y + 4, 12, 12, icon);
        if (e->size == -2) fill_rect(c, 8, y + 2, 6, 3, icon); /* a folder's tab */
        char name[30];
        int n = 0;
        for (; e->name[n] && n < 26; n++) name[n] = e->name[n];
        name[n] = 0;
        draw_text(c, 28, y + 2, name, TEXT);
        char size[24];
        size_text(size, e->size);
        draw_text(c, LIST_W - 8 - (int)strlen(size) * CHAR_WIDTH, y + 2, size, DIM);
    }
    if (count == 0) draw_text(c, 12, BAR + 8, "(empty)", DIM);
    fill_rect(c, LIST_W, BAR, 1, H - BAR, 0xD5D9E2);

    /* The preview. */
    if (selected >= 0) {
        draw_text(c, LIST_W + 12, BAR + 8, entries[selected].name, TEXT);
        u64 modified = 0;
        if (file_stat(entries[selected].name, NULL, &modified) >= 0 && modified) {
            char when[20];
            format_time(modified, when);
            draw_text(c, LIST_W + 12, BAR + 26, when, DIM);
        }
        for (int i = 0; i < preview_lines; i++) draw_text(c, LIST_W + 12, BAR + 50 + i * 16, preview[i], TEXT);
    }
    if (status[0]) draw_text(c, LIST_W + 12, H - 22, status, PROGRAM);
    gui_update(win, 0, 0, W, H);

    char title[40] = "Files: ";
    strcpy(title + 7, path[1] ? path : "/");
    title[39] = 0;
    gui_set_title(win, title);
}

static void go(const char *where) {
    if (chdir(where) == 0) load();
}

/* Open the selected entry: go into a folder, start a program. */
static void open_selected(void) {
    if (selected < 0) return;
    Entry *e = &entries[selected];
    if (e->size == -2) {
        go(e->name);
    } else if (ends_with(e->name, ".exe")) {
        /* A text program gets a terminal window of its own. */
        char name[64];
        strcpy(name, e->name);
        int graphical = gui_is_graphical(name);
        strcpy(status, gui_launch(name) < 0 ? "could not start " : graphical ? "started " : "opened in a terminal: ");
        strcpy(status + strlen(status), name);
    }
}

static void select_entry(int i) {
    if (i < 0 || i >= count) return;
    selected = i;
    if (selected < scroll) scroll = selected;
    if (selected >= scroll + visible_rows()) scroll = selected - visible_rows() + 1;
    show(&entries[selected]);
}

int main(int argc, char **argv) {
    GUI_PROGRAM();
    if (argc > 1) chdir(argv[1]);
    if (gui_connect() < 0) {
        printf("files: the desktop is not running (start it with: desktop)\n");
        return 1;
    }
    win = gui_window_new(W, H, "Files");
    if (!win) return 1;
    load();
    draw();
    int last_buttons = 0;
    gui_message m;
    for (;;) {
        int got = gui_next(&m, -1);
        if (got < 0) return 0;
        if (got == 0) continue;
        if (m.type == MSG_CLOSE) break;
        if (m.type == MSG_KEY) {
            int k = m.a;
            if (k == 'q' || k == KEY_ESCAPE) break;
            if (k == KEY_DOWN) select_entry(selected + 1);
            else if (k == KEY_UP) select_entry(selected > 0 ? selected - 1 : 0);
            else if (k == '\n') open_selected();
            else if (k == 8) go("..");
            draw();
        } else if (m.type == MSG_MOUSE) {
            int pressed = m.c & ~last_buttons;
            last_buttons = m.c;
            if (!(pressed & MOUSE_LEFT)) continue;
            if (m.b < BAR) {
                if (m.a < 60) go("..");
            } else if (m.a < LIST_W) {
                int i = scroll + (m.b - BAR - 2) / ROW;
                if (i < count) {
                    /* A folder opens at once; a file is shown first, and a
                       second click on a program starts it. */
                    int again = i == selected;
                    select_entry(i);
                    if (entries[i].size == -2 || again) open_selected();
                }
            }
            draw();
        }
    }
    gui_window_close(win);
    return 0;
}
