/* desktop - the RDX window server: windows, a taskbar and a mouse pointer.

   It takes over the display (the text console comes back when it exits)
   and listens for programs on the local connection name "desktop" (see
   gui.h). Each window's pixels live in shared memory: the program draws,
   says which part changed, and the desktop copies it onto the screen
   together with the title bars, the taskbar and the pointer.

   Using it:
     * the taskbar starts programs (Terminal, Files, Monitor, Browser) and lists the
       open windows; Log out closes everything and returns to the console;
     * drag a window by its title bar; the x closes it; clicking a window
       brings it to the front and gives it the keyboard;
     * with no window selected (click the background), keys work as
       shortcuts: t terminal, f files, m monitor, b browser, q log out.

   Usage: desktop [program...]    starts those programs once it is up */
#include "gui.h"

#define MAX_WINDOWS 32
#define MAX_CLIENTS 24
#define TITLE_H 24
#define BORDER 1
#define TASKBAR_H 34

#define COLOR_TITLE 0x2B3245
#define COLOR_TITLE_FOCUS 0x3D5A99
#define COLOR_BORDER 0x151922
#define COLOR_TASKBAR 0x161A24
#define COLOR_BUTTON 0x262C3B
#define COLOR_BUTTON_ACTIVE 0x3D5A99
#define COLOR_TEXT 0xE6E9EF
#define COLOR_DIM 0x8A92A6

typedef struct {
    int used;
    unsigned id;
    int client;   /* which connection owns it */
    int x, y;     /* top left of the content area, on the screen */
    int w, h;     /* content size */
    char title[40];
    char shm[40];
    canvas content;
} Window;

typedef struct {
    int fd; /* -1: slot free */
    unsigned char buf[sizeof(gui_message)];
    int have;
} Client;

static Window windows[MAX_WINDOWS];
static int order[MAX_WINDOWS]; /* window slots, bottom first */
static int window_count;
static int focused = -1;
static unsigned next_id = 1;

static Client clients[MAX_CLIENTS];

static display_info screen;
static canvas frame;     /* the whole screen, as it should look (minus the pointer) */
static canvas wallpaper; /* the background, drawn once */
static int pointer_x, pointer_y, buttons;
static int dragging = -1, drag_dx, drag_dy; /* window being moved by its title bar */
static int mouse_target = -1;               /* window that got a button press, until release */
static int quit;

/* The part of the screen that needs drawing again. */
static int dirty_x0, dirty_y0, dirty_x1, dirty_y1, dirty_any;

static void touched(int x, int y, int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (!dirty_any) {
        dirty_x0 = x;
        dirty_y0 = y;
        dirty_x1 = x + w;
        dirty_y1 = y + h;
        dirty_any = 1;
        return;
    }
    if (x < dirty_x0) dirty_x0 = x;
    if (y < dirty_y0) dirty_y0 = y;
    if (x + w > dirty_x1) dirty_x1 = x + w;
    if (y + h > dirty_y1) dirty_y1 = y + h;
}

/* ---------------------------------------------------------------- geometry */

static void outer_rect(const Window *w, int *x, int *y, int *width, int *height) {
    *x = w->x - BORDER;
    *y = w->y - TITLE_H - BORDER;
    *width = w->w + 2 * BORDER;
    *height = w->h + TITLE_H + 2 * BORDER;
}

static void touch_window(const Window *w) {
    int x, y, width, height;
    outer_rect(w, &x, &y, &width, &height);
    touched(x, y, width, height);
}

static int inside(int px, int py, int x, int y, int w, int h) {
    return px >= x && py >= y && px < x + w && py < y + h;
}

/* The close box at the right of a window's title bar. */
static void close_box(const Window *w, int *x, int *y, int *size) {
    *size = TITLE_H - 8;
    *x = w->x + w->w - *size - 4;
    *y = w->y - TITLE_H + 4;
}

/* Topmost window under a point, or -1. */
static int window_at(int px, int py) {
    for (int i = window_count - 1; i >= 0; i--) {
        int x, y, w, h;
        outer_rect(&windows[order[i]], &x, &y, &w, &h);
        if (inside(px, py, x, y, w, h)) return order[i];
    }
    return -1;
}

/* ---------------------------------------------------------------- talking to programs */

static void send_to(int client, unsigned type, unsigned window, int a, int b, int c, const char *text) {
    if (client < 0 || clients[client].fd < 0) return;
    gui_message m;
    memset(&m, 0, sizeof(m));
    m.type = type;
    m.window = window;
    m.a = a;
    m.b = b;
    m.c = c;
    if (text) {
        for (int i = 0; text[i] && i < 39; i++) m.text[i] = text[i];
    }
    /* A program that has stopped reading must not stall the desktop: if
       its connection is full, the message is dropped. */
    pollfd p = {clients[client].fd, POLLOUT, 0};
    if (poll(&p, 1, 0) == 1 && (p.revents & POLLOUT)) fd_write(clients[client].fd, &m, sizeof(m));
}

static void set_focus(int slot) {
    if (slot == focused) return;
    if (focused >= 0 && windows[focused].used) {
        send_to(windows[focused].client, MSG_FOCUS, windows[focused].id, 0, 0, 0, NULL);
        touch_window(&windows[focused]);
    }
    focused = slot;
    if (slot >= 0) {
        send_to(windows[slot].client, MSG_FOCUS, windows[slot].id, 1, 0, 0, NULL);
        touch_window(&windows[slot]);
    }
    touched(0, screen.height - TASKBAR_H, screen.width, TASKBAR_H);
}

static void raise_window(int slot) {
    int at = -1;
    for (int i = 0; i < window_count; i++) {
        if (order[i] == slot) at = i;
    }
    if (at < 0) return;
    for (int i = at; i < window_count - 1; i++) order[i] = order[i + 1];
    order[window_count - 1] = slot;
    touch_window(&windows[slot]);
    set_focus(slot);
}

static void remove_window(int slot) {
    Window *w = &windows[slot];
    if (!w->used) return;
    touch_window(w);
    shm_close(w->content.pixels);
    shm_unlink(w->shm);
    w->used = 0;
    int j = 0;
    for (int i = 0; i < window_count; i++) {
        if (order[i] != slot) order[j++] = order[i];
    }
    window_count = j;
    if (dragging == slot) dragging = -1;
    if (mouse_target == slot) mouse_target = -1;
    if (focused == slot) {
        focused = -1;
        if (window_count) set_focus(order[window_count - 1]);
    }
    touched(0, screen.height - TASKBAR_H, screen.width, TASKBAR_H);
}

static int find_window(int client, unsigned id) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].id == id && windows[i].client == client) return i;
    }
    return -1;
}

static void create_window(int client, const gui_message *m) {
    int slot = -1;
    for (int i = 0; i < MAX_WINDOWS && slot < 0; i++) {
        if (!windows[i].used) slot = i;
    }
    if (slot < 0) return; /* no room: the program's gui_window_new will wait in vain... */
    Window *w = &windows[slot];
    memset(w, 0, sizeof(*w));
    w->id = next_id++;
    w->client = client;
    w->w = m->a < 40 ? 40 : m->a > screen.width - 2 * BORDER ? screen.width - 2 * BORDER : m->a;
    w->h = m->b < 20 ? 20 : m->b > screen.height - TASKBAR_H - TITLE_H - 2 * BORDER
                                 ? screen.height - TASKBAR_H - TITLE_H - 2 * BORDER
                                 : m->b;
    memcpy(w->title, m->text, sizeof(w->title));
    w->title[39] = 0;
    /* A name unique to this window for its pixels. */
    strcpy(w->shm, "desktop-window-");
    char digits[12];
    int n = 0;
    unsigned v = w->id;
    do digits[n++] = (char)('0' + v % 10);
    while (v /= 10);
    size_t at = strlen(w->shm);
    while (n) w->shm[at++] = digits[--n];
    w->shm[at] = 0;
    unsigned *pixels = shm_open(w->shm, (size_t)w->w * (size_t)w->h * 4);
    if (!pixels) return;
    w->content.pixels = pixels;
    w->content.width = w->content.stride = w->w;
    w->content.height = w->h;
    fill_rect(&w->content, 0, 0, w->w, w->h, 0xFFFFFF);
    /* Cascade new windows down and to the right, within the screen. */
    int step = (int)(w->id % 8);
    w->x = 80 + step * 32;
    w->y = 60 + step * 28 + TITLE_H;
    if (w->x + w->w + BORDER > screen.width) w->x = screen.width - w->w - BORDER;
    if (w->y + w->h + BORDER > screen.height - TASKBAR_H) w->y = screen.height - TASKBAR_H - w->h - BORDER;
    if (w->x < BORDER) w->x = BORDER;
    if (w->y < TITLE_H + BORDER) w->y = TITLE_H + BORDER;
    w->used = 1;
    order[window_count++] = slot;
    send_to(client, MSG_CREATED, w->id, w->w, w->h, 0, w->shm);
    raise_window(slot);
}

static void handle_message(int client, const gui_message *m) {
    int slot = m->type == MSG_CREATE ? -1 : find_window(client, m->window);
    switch (m->type) {
    case MSG_CREATE:
        create_window(client, m);
        break;
    case MSG_UPDATE:
        if (slot >= 0) touched(windows[slot].x + m->a, windows[slot].y + m->b, m->c, m->d);
        break;
    case MSG_TITLE:
        if (slot >= 0) {
            memcpy(windows[slot].title, m->text, sizeof(windows[slot].title));
            windows[slot].title[39] = 0;
            touch_window(&windows[slot]);
            touched(0, screen.height - TASKBAR_H, screen.width, TASKBAR_H);
        }
        break;
    case MSG_DESTROY:
        if (slot >= 0) remove_window(slot);
        break;
    }
}

static void drop_client(int client) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (windows[i].used && windows[i].client == client) remove_window(i);
    }
    close(clients[client].fd);
    clients[client].fd = -1;
}

static void read_client(int client) {
    Client *c = &clients[client];
    i64 n = read(c->fd, c->buf + c->have, sizeof(c->buf) - (size_t)c->have);
    if (n == EINTR) return;
    if (n <= 0) {
        drop_client(client);
        return;
    }
    c->have += (int)n;
    if (c->have == (int)sizeof(c->buf)) {
        gui_message m;
        memcpy(&m, c->buf, sizeof(m));
        c->have = 0;
        handle_message(client, &m);
    }
}

/* ---------------------------------------------------------------- drawing */

/* Text drawn bigger: each pixel of the font becomes a scale x scale block. */
static void draw_big(canvas *c, int x, int y, const char *text, int scale, unsigned color) {
    for (; *text; text++, x += CHAR_WIDTH * scale) {
        canvas glyph = canvas_new(CHAR_WIDTH, CHAR_HEIGHT);
        if (!glyph.pixels) return;
        fill_rect(&glyph, 0, 0, CHAR_WIDTH, CHAR_HEIGHT, 0);
        draw_char(&glyph, 0, 0, *text, 1);
        for (int gy = 0; gy < CHAR_HEIGHT; gy++)
            for (int gx = 0; gx < CHAR_WIDTH; gx++)
                if (glyph.pixels[gy * CHAR_WIDTH + gx]) fill_rect(c, x + gx * scale, y + gy * scale, scale, scale, color);
        free(glyph.pixels);
    }
}

static void make_wallpaper(void) {
    wallpaper = canvas_new(screen.width, screen.height);
    for (int y = 0; y < screen.height; y++) {
        /* A deep blue fading towards violet. */
        int t = y * 255 / screen.height;
        unsigned r = (unsigned)(18 + t * 40 / 255), g = (unsigned)(24 + t * 10 / 255), b = (unsigned)(48 + t * 50 / 255);
        fill_rect(&wallpaper, 0, y, screen.width, 1, r << 16 | g << 8 | b);
    }
    int scale = screen.width >= 1000 ? 10 : 6;
    int w = 3 * CHAR_WIDTH * scale;
    draw_big(&wallpaper, (screen.width - w) / 2, screen.height / 2 - CHAR_HEIGHT * scale / 2 - 40, "RDX", scale,
             0x2E3A5C);
    const char *hint = "Click the background, then: t terminal   f files   m monitor   b browser   q log out";
    draw_text(&wallpaper, (screen.width - (int)strlen(hint) * CHAR_WIDTH) / 2,
              screen.height / 2 + CHAR_HEIGHT * scale / 2 - 10, hint, 0x7C86A8);
}

static void draw_window(canvas *v, const Window *w, int ox, int oy) {
    int x, y, width, height;
    outer_rect(w, &x, &y, &width, &height);
    int active = order[window_count - 1] == (int)(w - windows) && focused == (int)(w - windows);
    fill_rect(v, x + ox, y + oy, width, height, COLOR_BORDER);
    fill_rect(v, w->x + ox, w->y - TITLE_H + oy, w->w, TITLE_H, active ? COLOR_TITLE_FOCUS : COLOR_TITLE);
    draw_text(v, w->x + 8 + ox, w->y - TITLE_H + 4 + oy, w->title, active ? COLOR_TEXT : COLOR_DIM);
    int bx, by, size;
    close_box(w, &bx, &by, &size);
    fill_rect(v, bx + ox, by + oy, size, size, active ? 0xC0392B : 0x4A5268);
    draw_char(v, bx + (size - CHAR_WIDTH) / 2 + ox, by + (size - CHAR_HEIGHT) / 2 + oy, 'x', COLOR_TEXT);
    blit(v, w->x + ox, w->y + oy, &w->content, 0, 0, w->w, w->h);
}

/* Taskbar buttons: the launchers, then one per window. */
static const char *LAUNCHERS[] = {"Terminal", "Files", "Monitor", "Browser", "Log out"};
static const char *LAUNCH_PROGRAMS[] = {"terminal", "files", "monitor", "browser", NULL};
#define LAUNCHER_COUNT 5
#define BUTTON_W 104
#define WINDOW_BUTTON_W 150

static void draw_taskbar(canvas *v, int ox, int oy) {
    int y0 = screen.height - TASKBAR_H;
    fill_rect(v, ox, y0 + oy, screen.width, TASKBAR_H, COLOR_TASKBAR);
    fill_rect(v, ox, y0 + oy, screen.width, 1, 0x2E3445);
    int x = 6;
    for (int i = 0; i < LAUNCHER_COUNT; i++, x += BUTTON_W + 6) {
        fill_rect(v, x + ox, y0 + 5 + oy, BUTTON_W, TASKBAR_H - 10, i == LAUNCHER_COUNT - 1 ? 0x5A2A35 : COLOR_BUTTON);
        int tw = (int)strlen(LAUNCHERS[i]) * CHAR_WIDTH;
        draw_text(v, x + (BUTTON_W - tw) / 2 + ox, y0 + 9 + oy, LAUNCHERS[i], COLOR_TEXT);
    }
    x += 12;
    for (int i = 0; i < window_count && x + WINDOW_BUTTON_W < screen.width - 90; i++, x += WINDOW_BUTTON_W + 4) {
        const Window *w = &windows[order[i]];
        fill_rect(v, x + ox, y0 + 5 + oy, WINDOW_BUTTON_W, TASKBAR_H - 10,
                  order[i] == focused ? COLOR_BUTTON_ACTIVE : COLOR_BUTTON);
        char label[19];
        int n = 0;
        for (; w->title[n] && n < 17; n++) label[n] = w->title[n];
        label[n] = 0;
        draw_text(v, x + 8 + ox, y0 + 9 + oy, label, COLOR_TEXT);
    }
    /* The time, from the hardware clock. */
    char now[20];
    format_time(time_now(), now);
    draw_text(v, screen.width - 8 * CHAR_WIDTH - 10 + ox, y0 + 9 + oy, now + 11, COLOR_TEXT);
}

/* Which taskbar button is at x: launcher index (0..), or a window slot
   through *slot. Returns -1 for none. */
static int taskbar_hit(int px, int *slot) {
    *slot = -1;
    int x = 6;
    for (int i = 0; i < LAUNCHER_COUNT; i++, x += BUTTON_W + 6) {
        if (px >= x && px < x + BUTTON_W) return i;
    }
    x += 12;
    for (int i = 0; i < window_count && x + WINDOW_BUTTON_W < screen.width - 90; i++, x += WINDOW_BUTTON_W + 4) {
        if (px >= x && px < x + WINDOW_BUTTON_W) {
            *slot = order[i];
            return -1;
        }
    }
    return -1;
}

/* Recompose the dirty part of the screen and copy it out. */
static void redraw(void) {
    if (!dirty_any) return;
    int x0 = dirty_x0 < 0 ? 0 : dirty_x0, y0 = dirty_y0 < 0 ? 0 : dirty_y0;
    int x1 = dirty_x1 > screen.width ? screen.width : dirty_x1;
    int y1 = dirty_y1 > screen.height ? screen.height : dirty_y1;
    dirty_any = 0;
    if (x1 <= x0 || y1 <= y0) return;
    /* A view of just the dirty part, so all drawing is clipped to it. */
    canvas v = {frame.pixels + (size_t)y0 * (size_t)frame.stride + x0, x1 - x0, y1 - y0, frame.stride};
    blit(&v, 0, 0, &wallpaper, x0, y0, x1 - x0, y1 - y0);
    for (int i = 0; i < window_count; i++) draw_window(&v, &windows[order[i]], -x0, -y0);
    draw_taskbar(&v, -x0, -y0);
    present(&screen, &frame, x0, y0, x1 - x0, y1 - y0);
}

/* The pointer, drawn straight onto the screen over everything. */
static const char *ARROW[] = {
    "1",        "11",        "121",        "1221",        "12221",   "122221",  "1222221",   "12222221",   "122222221",
    "1222222221", "12222211111", "1221221", "121 1221", "11  1221", "1    1221", "     1221", "      11",
};
#define ARROW_ROWS 17
#define ARROW_COLS 11

static void draw_pointer(void) {
    for (int row = 0; row < ARROW_ROWS; row++) {
        for (int col = 0; ARROW[row][col]; col++) {
            char p = ARROW[row][col];
            int x = pointer_x + col, y = pointer_y + row;
            if (p == ' ' || x >= screen.width || y >= screen.height) continue;
            screen.pixels[(size_t)y * (size_t)screen.stride + x] = p == '1' ? 0x000000 : 0xFFFFFF;
        }
    }
}

/* ---------------------------------------------------------------- input */

/* Text programs get a terminal window of their own (see gui_launch). */
static void launch(const char *program) {
    if (gui_launch(program) < 0) printf("desktop: cannot start %s\n", program);
}

static void taskbar_click(int px) {
    int slot;
    int launcher = taskbar_hit(px, &slot);
    if (slot >= 0) {
        raise_window(slot);
    } else if (launcher >= 0) {
        if (LAUNCH_PROGRAMS[launcher]) launch(LAUNCH_PROGRAMS[launcher]);
        else quit = 1;
    }
}

static void mouse_event(const input_event *e) {
    int x = e->x, y = e->y, now = (int)e->code;
    int pressed = now & ~buttons, released = buttons & ~now;
    touched(pointer_x, pointer_y, ARROW_COLS, ARROW_ROWS);
    pointer_x = x;
    pointer_y = y;
    touched(pointer_x, pointer_y, ARROW_COLS, ARROW_ROWS);

    if (dragging >= 0) {
        Window *w = &windows[dragging];
        touch_window(w);
        w->x = x - drag_dx;
        w->y = y - drag_dy;
        /* Keep the title bar where it can be grabbed again. */
        if (w->y < TITLE_H + BORDER) w->y = TITLE_H + BORDER;
        if (w->y > screen.height - TASKBAR_H) w->y = screen.height - TASKBAR_H;
        if (w->x + w->w < 60) w->x = 60 - w->w;
        if (w->x > screen.width - 60) w->x = screen.width - 60;
        touch_window(w);
        if (released & MOUSE_LEFT) dragging = -1;
    } else if (pressed) {
        if (y >= screen.height - TASKBAR_H) {
            if (pressed & MOUSE_LEFT) taskbar_click(x);
        } else {
            int slot = window_at(x, y);
            if (slot < 0) {
                set_focus(-1); /* the background: keys become shortcuts */
            } else {
                Window *w = &windows[slot];
                raise_window(slot);
                int bx, by, size;
                close_box(w, &bx, &by, &size);
                if (inside(x, y, bx, by, size, size)) {
                    send_to(w->client, MSG_CLOSE, w->id, 0, 0, 0, NULL);
                } else if (y < w->y) {
                    if (pressed & MOUSE_LEFT) {
                        dragging = slot;
                        drag_dx = x - w->x;
                        drag_dy = y - w->y;
                    }
                } else {
                    mouse_target = slot;
                    send_to(w->client, MSG_MOUSE, w->id, x - w->x, y - w->y, now, NULL);
                }
            }
        }
    } else if (mouse_target >= 0) {
        /* Moves and the release go to the window the press went to. */
        Window *w = &windows[mouse_target];
        send_to(w->client, MSG_MOUSE, w->id, x - w->x, y - w->y, now, NULL);
        if (!now) mouse_target = -1;
    }
    buttons = now;
}

static void key_event(int key) {
    if (focused >= 0) {
        send_to(windows[focused].client, MSG_KEY, windows[focused].id, key, 0, 0, NULL);
        return;
    }
    switch (key) {
    case 't': launch("terminal"); break;
    case 'f': launch("files"); break;
    case 'm': launch("monitor"); break;
    case 'b': launch("browser"); break;
    case 'q': quit = 1; break;
    }
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv) {
    int listener = local_listen(GUI_SERVICE);
    if (listener < 0) {
        printf("desktop: %s\n", last_error());
        return 1;
    }
    int events = display_open(&screen);
    if (events < 0) {
        printf("desktop: cannot use the display: %s\n", last_error());
        return 1;
    }
    frame = canvas_new(screen.width, screen.height);
    make_wallpaper();
    if (!frame.pixels || !wallpaper.pixels) {
        close(events);
        printf("desktop: not enough memory\n");
        return 1;
    }
    for (int i = 0; i < MAX_CLIENTS; i++) clients[i].fd = -1;
    pointer_x = screen.mouse_x;
    pointer_y = screen.mouse_y;
    touched(0, 0, screen.width, screen.height);
    printf("desktop: running on a %dx%d screen\n", screen.width, screen.height);
    for (int i = 1; i < argc; i++) launch(argv[i]);

    u64 last_second = time_now();
    input_event batch[64];
    while (!quit) {
        redraw();
        draw_pointer();

        pollfd watch[2 + MAX_CLIENTS];
        int who[2 + MAX_CLIENTS];
        int n = 0;
        watch[n] = (pollfd){events, POLLIN, 0};
        who[n++] = -1;
        watch[n] = (pollfd){listener, POLLIN, 0};
        who[n++] = -2;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (clients[i].fd >= 0) {
                watch[n] = (pollfd){clients[i].fd, POLLIN, 0};
                who[n++] = i;
            }
        }
        int ready = poll(watch, n, 250);
        if (ready == EINTR) continue;

        /* The clock in the corner. */
        u64 second = time_now();
        if (second != last_second) {
            last_second = second;
            touched(screen.width - 120, screen.height - TASKBAR_H, 120, TASKBAR_H);
        }
        if (ready <= 0) continue;

        for (int i = 0; i < n; i++) {
            if (!watch[i].revents) continue;
            if (who[i] == -1) {
                int count = read_events(events, batch, 64);
                for (int k = 0; k < count; k++) {
                    if (batch[k].kind == EVENT_MOUSE) mouse_event(&batch[k]);
                    else if (batch[k].kind == EVENT_KEY) key_event((int)batch[k].code);
                }
            } else if (who[i] == -2) {
                int fd = local_accept(listener);
                int slot = -1;
                for (int k = 0; k < MAX_CLIENTS && slot < 0; k++) {
                    if (clients[k].fd < 0) slot = k;
                }
                if (fd >= 0 && slot >= 0) {
                    clients[slot].fd = fd;
                    clients[slot].have = 0;
                } else if (fd >= 0) {
                    close(fd); /* too many programs */
                }
            } else if (clients[who[i]].fd >= 0) {
                read_client(who[i]);
            }
        }
    }

    /* Log out: every program loses its connection (and so its windows and
       usually itself), and the console gets the screen back. */
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (clients[i].fd >= 0) drop_client(i);
    }
    close(listener);
    close(events);
    printf("desktop: logged out\n");
    return 0;
}
