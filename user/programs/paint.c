/* paint - draw with the mouse. RDX's first graphical program.

   It takes over the whole screen (the text console comes back when it
   exits). Hold the left button and drag to draw; the right button rubs out.
   Keys: 1-8 pick a colour, + and - change the brush size, c clears the
   picture, s saves it as paint.ppm, q or Esc quits.

   How it draws: everything goes into a picture in memory first, then only
   the part that changed is copied to the screen, with the mouse pointer
   drawn on top. */
#include "rdx.h"

#define TOOLBAR 40
#define PAPER 0xFFFFFF
#define BAR 0x1F2430
#define TEXT 0xD0D4DC

static const unsigned palette[8] = {0x111111, 0xE5484D, 0x30A46C, 0x0090FF, 0xF5D90A, 0x8E4EC6, 0xF76B15, 0x6C7380};

static display_info screen;
static canvas picture; /* what is on the screen, minus the pointer */
static unsigned colour = 0x111111;
static int brush = 4, strokes;

/* Changed area waiting to be copied to the screen. */
static int dirty_x0 = 1 << 30, dirty_y0 = 1 << 30, dirty_x1 = -1, dirty_y1 = -1;

static void touched(int x, int y, int w, int h) {
    if (x < dirty_x0) dirty_x0 = x;
    if (y < dirty_y0) dirty_y0 = y;
    if (x + w > dirty_x1) dirty_x1 = x + w;
    if (y + h > dirty_y1) dirty_y1 = y + h;
}

/* The pointer: an arrow, 1 = outline, 2 = fill. */
static const char *ARROW[] = {
    "1",           "11",          "121",         "1221",        "12221",       "122221",      "1222221",
    "12222221",    "122222221",   "1222222221",  "12222211111", "1221221",     "121 1221",    "11  1221",
    "1    1221",   "     1221",   "      11",
};
#define ARROW_ROWS 17
#define ARROW_COLS 11

static void draw_pointer(int x, int y) {
    canvas s = screen_canvas(&screen);
    for (int row = 0; row < ARROW_ROWS; row++) {
        for (int col = 0; ARROW[row][col]; col++) {
            char p = ARROW[row][col];
            if (p == ' ') continue;
            unsigned v = p == '1' ? 0x000000 : 0xFFFFFF;
            int px = x + col, py = y + row;
            if (px < s.width && py < s.height) s.pixels[(size_t)py * (size_t)s.stride + px] = v;
        }
    }
}

static void draw_toolbar(void) {
    fill_rect(&picture, 0, 0, picture.width, TOOLBAR, BAR);
    for (int i = 0; i < 8; i++) {
        int x = 10 + i * 34;
        fill_rect(&picture, x, 8, 26, 24, palette[i]);
        draw_rect(&picture, x - 2, 6, 30, 28, palette[i] == colour ? 0xFFFFFF : BAR);
        draw_char(&picture, x + 9, 12, (char)('1' + i), palette[i] == 0x111111 ? 0xFFFFFF : 0x000000);
    }
    char info[96] = "RDX Paint   brush ";
    char *p = info + strlen(info);
    if (brush >= 10) *p++ = (char)('0' + brush / 10);
    *p++ = (char)('0' + brush % 10);
    strcpy(p, "   left draws, right rubs out, +/- brush, c clear, s save, q quit");
    draw_text(&picture, 300, 12, info, TEXT);
    touched(0, 0, picture.width, TOOLBAR);
}

static void clear_paper(void) {
    fill_rect(&picture, 0, TOOLBAR, picture.width, picture.height - TOOLBAR, PAPER);
    touched(0, TOOLBAR, picture.width, picture.height - TOOLBAR);
}

static void stroke(int x0, int y0, int x1, int y1, unsigned c, int size) {
    canvas paper = picture;
    /* Never paint over the toolbar. */
    if (y0 < TOOLBAR + size / 2 && y1 < TOOLBAR + size / 2) return;
    draw_line(&paper, x0, y0, x1, y1, size, c);
    fill_rect(&picture, 0, 0, picture.width, TOOLBAR, BAR); /* undo any overlap */
    draw_toolbar();
    int lx = x0 < x1 ? x0 : x1, ly = y0 < y1 ? y0 : y1;
    int hx = x0 < x1 ? x1 : x0, hy = y0 < y1 ? y1 : y0;
    touched(lx - size, ly - size, hx - lx + 2 * size + 1, hy - ly + 2 * size + 1);
}

/* Save the picture (without the toolbar) as a PPM image file. */
static void save(void) {
    int w = picture.width, h = picture.height - TOOLBAR;
    char header[64];
    char *p = header;
    strcpy(p, "P6\n");
    p += 3;
    for (int n = 0, v = w; n < 2; n++, v = h) {
        char digits[12];
        int k = 0;
        do digits[k++] = (char)('0' + v % 10);
        while (v /= 10);
        while (k) *p++ = digits[--k];
        *p++ = n == 0 ? ' ' : '\n';
    }
    strcpy(p, "255\n");
    size_t head = strlen(header), size = head + (size_t)w * (size_t)h * 3;
    unsigned char *data = malloc(size);
    if (!data) return;
    memcpy(data, header, head);
    unsigned char *out = data + head;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            unsigned v = picture.pixels[(size_t)(y + TOOLBAR) * (size_t)picture.stride + x];
            *out++ = (unsigned char)(v >> 16);
            *out++ = (unsigned char)(v >> 8);
            *out++ = (unsigned char)v;
        }
    }
    file_write("paint.ppm", data, size);
    free(data);
}

int main(void) {
    int events = display_open(&screen);
    if (events < 0) {
        printf("paint: cannot use the display: %s\n", last_error());
        return 1;
    }
    picture = canvas_new(screen.width, screen.height);
    if (!picture.pixels) {
        close(events);
        printf("paint: not enough memory for a %dx%d picture\n", screen.width, screen.height);
        return 1;
    }
    clear_paper();
    draw_toolbar();

    int x = screen.mouse_x, y = screen.mouse_y, buttons = 0, quit = 0;
    int moves = 0, clicks = 0;
    input_event batch[64];
    for (;;) {
        /* Copy what changed to the screen, then the pointer over it. */
        if (dirty_x1 >= 0) {
            present(&screen, &picture, dirty_x0, dirty_y0, dirty_x1 - dirty_x0, dirty_y1 - dirty_y0);
            dirty_x0 = dirty_y0 = 1 << 30;
            dirty_x1 = dirty_y1 = -1;
        }
        draw_pointer(x, y);
        if (quit) break;

        int n = read_events(events, batch, 64);
        if (n == EINTR) continue;
        if (n < 0) break;
        /* Where the pointer was must be repainted from the picture. */
        touched(x, y, ARROW_COLS, ARROW_ROWS);
        for (int i = 0; i < n; i++) {
            input_event *e = &batch[i];
            if (e->kind == EVENT_KEY) {
                int k = (int)e->code;
                if (k == 'q' || k == KEY_ESCAPE || k == CTRL('C')) quit = 1;
                else if (k >= '1' && k <= '8') colour = palette[k - '1'];
                else if (k == '+' || k == '=') brush = brush < 40 ? brush + 1 : 40;
                else if (k == '-') brush = brush > 1 ? brush - 1 : 1;
                else if (k == 'c') clear_paper();
                else if (k == 's') save();
                draw_toolbar();
            } else if (e->kind == EVENT_MOUSE) {
                int nx = e->x, ny = e->y, now = (int)e->code;
                moves++;
                if ((now & MOUSE_LEFT) && !(buttons & MOUSE_LEFT)) {
                    clicks++;
                    strokes++;
                    if (ny < TOOLBAR) { /* a click on the toolbar picks a colour */
                        int slot = (nx - 8) / 34;
                        if (slot >= 0 && slot < 8) colour = palette[slot];
                        draw_toolbar();
                    } else {
                        stroke(nx, ny, nx, ny, colour, brush);
                    }
                } else if (now & MOUSE_LEFT) {
                    stroke(x, y, nx, ny, colour, brush);
                } else if (now & MOUSE_RIGHT) {
                    stroke(x, y, nx, ny, PAPER, brush * 4);
                }
                x = nx;
                y = ny;
                buttons = now;
            }
        }
    }
    close(events); /* the console takes the screen back */
    printf("paint: %d stroke(s), %d mouse event(s), %d click(s), pointer ended at %d,%d\n", strokes, moves, clicks,
           x, y);
    return 0;
}
