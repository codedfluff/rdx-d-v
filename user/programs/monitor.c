/* monitor - a window showing what the machine is doing: a graph of how
   busy the processors have been, memory in use, and the busiest tasks.
   Updated every second. */
#include "gui.h"

#define W 470
#define H 360
#define HISTORY 90
#define MAX_TASKS 256

#define BG 0x10141C
#define PANEL 0x1A2030
#define TEXT 0xE6E9EF
#define DIM 0x8A92A6
#define ACCENT 0x57C7FF

static gui_window *win;
static proc_info now[MAX_TASKS], before[MAX_TASKS];
static int now_count, before_count;
static int history[HISTORY]; /* busy percentage, oldest first */

static u64 used_since(const proc_info *p) {
    for (int i = 0; i < before_count; i++) {
        if (before[i].tid == p->tid) return p->cpu_ms - before[i].cpu_ms;
    }
    return p->cpu_ms;
}

static void number(char *out, u64 v) {
    char digits[24];
    int n = 0;
    do digits[n++] = (char)('0' + v % 10);
    while (v /= 10);
    while (n) *out++ = digits[--n];
    *out = 0;
}

/* Append to a line being built. */
static void add(char *line, const char *text) { strcpy(line + strlen(line), text); }

static void add_number(char *line, u64 v) {
    char digits[24];
    number(digits, v);
    add(line, digits);
}

static void draw(u64 elapsed_ms) {
    sys_info sys;
    sysinfo(&sys);
    canvas *c = &win->pixels;
    fill_rect(c, 0, 0, W, H, BG);

    /* How busy, over the last second: everything but the idle tasks. */
    u64 busy = 0;
    for (int i = 0; i < now_count; i++) {
        if (memcmp(now[i].name, "idle/", 5) != 0) busy += used_since(&now[i]);
    }
    u64 capacity = elapsed_ms * (sys.cpus ? sys.cpus : 1);
    int percent = capacity ? (int)(busy * 100 / capacity) : 0;
    if (percent > 100) percent = 100;
    memmove(history, history + 1, sizeof(history) - sizeof(history[0]));
    history[HISTORY - 1] = percent;

    char line[96] = "Processors: ";
    add_number(line, (u64)percent);
    add(line, "% busy on ");
    add_number(line, sys.cpus);
    add(line, " core(s)");
    draw_text(c, 12, 10, line, TEXT);
    int gx = 12, gy = 32, gw = W - 24, gh = 100;
    fill_rect(c, gx, gy, gw, gh, PANEL);
    for (int i = 0; i < HISTORY; i++) {
        int bar = history[i] * (gh - 2) / 100;
        int x0 = gx + 1 + i * (gw - 2) / HISTORY, x1 = gx + 1 + (i + 1) * (gw - 2) / HISTORY;
        fill_rect(c, x0, gy + gh - 1 - bar, x1 - x0, bar, ACCENT);
    }
    draw_rect(c, gx, gy, gw, gh, 0x2E3650);

    /* Memory. */
    strcpy(line, "Memory: ");
    add_number(line, sys.mem_used >> 20);
    add(line, " of ");
    add_number(line, sys.mem_total >> 20);
    add(line, " MiB in use");
    draw_text(c, 12, 146, line, TEXT);
    fill_rect(c, 12, 166, W - 24, 14, PANEL);
    int used = sys.mem_total ? (int)((u64)(W - 26) * sys.mem_used / sys.mem_total) : 0;
    fill_rect(c, 13, 167, used, 12, 0x5AF78E);

    /* Uptime and counts. */
    u64 s = sys.uptime_ms / 1000;
    strcpy(line, "Up ");
    add_number(line, s / 3600);
    add(line, "h ");
    add_number(line, s / 60 % 60);
    add(line, "m ");
    add_number(line, s % 60);
    add(line, "s     ");
    add_number(line, (u64)now_count);
    add(line, " tasks     RDX ");
    add(line, sys.version);
    draw_text(c, 12, 190, line, DIM);

    /* The busiest tasks. */
    draw_text(c, 12, 216, "  PID  CPU%  NAME", ACCENT);
    int shown[8], shown_count = 0;
    for (int k = 0; k < 8; k++) {
        int best = -1;
        u64 best_used = 0;
        for (int i = 0; i < now_count; i++) {
            if (memcmp(now[i].name, "idle/", 5) == 0) continue;
            int taken = 0;
            for (int j = 0; j < shown_count; j++) taken |= shown[j] == i;
            u64 u = used_since(&now[i]);
            if (!taken && (best < 0 || u > best_used)) {
                best = i;
                best_used = u;
            }
        }
        if (best < 0) break;
        shown[shown_count++] = best;
        u64 tenths = elapsed_ms ? best_used * 1000 / elapsed_ms : 0;
        if (tenths > 1000) tenths = 1000;
        char pid[12], pct[12];
        number(pid, now[best].pid);
        number(pct, tenths / 10);
        strcpy(line, "     ");
        strcpy(line + 5 - (strlen(pid) < 5 ? strlen(pid) : 5), pid);
        add(line, "  ");
        for (size_t pad = strlen(pct); pad < 3; pad++) add(line, " ");
        add(line, pct);
        add(line, ".");
        char tenth[2] = {(char)('0' + tenths % 10), 0};
        add(line, tenth);
        add(line, "  ");
        add(line, now[best].name);
        draw_text(c, 12, 236 + k * 18, line, TEXT);
    }
    gui_update(win, 0, 0, W, H);
}

static void sample(void) {
    now_count = 0;
    while (now_count < MAX_TASKS && proc_list(now_count, &now[now_count]) == 0) now_count++;
}

int main(void) {
    GUI_PROGRAM();
    if (gui_connect() < 0) {
        printf("monitor: the desktop is not running (start it with: desktop)\n");
        return 1;
    }
    win = gui_window_new(W, H, "System monitor");
    if (!win) return 1;
    sample();
    u64 last = uptime_ms();
    for (;;) {
        memcpy(before, now, sizeof(now));
        before_count = now_count;
        gui_message m;
        int got;
        /* Wait out the second, answering the desktop meanwhile. */
        while ((got = gui_next(&m, (int)(last + 1000 > uptime_ms() ? last + 1000 - uptime_ms() : 0))) > 0) {
            if (m.type == MSG_CLOSE || (m.type == MSG_KEY && (m.a == 'q' || m.a == KEY_ESCAPE))) {
                gui_window_close(win);
                return 0;
            }
        }
        if (got < 0) return 0; /* the desktop has gone */
        sample();
        u64 t = uptime_ms();
        draw(t - last);
        last = t;
    }
}
