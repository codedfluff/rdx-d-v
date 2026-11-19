/* imgview - look at a picture (PNG, JPEG or GIF) in a window.
   Usage: imgview <file>
          imgview --info <file>   print its size and some pixels (for scripts)
   In the window: + and - zoom, 0 fits it to the window, q closes. */
#include "gui.h"
#include "image.h"

#define MAX_W 1000
#define MAX_H 640

int main(int argc, char **argv) {
    GUI_PROGRAM();
    int info = argc > 2 && strcmp(argv[1], "--info") == 0;
    const char *name = info ? argv[2] : argc > 1 ? argv[1] : NULL;
    if (!name) {
        printf("usage: imgview <picture file>\n");
        return 1;
    }
    size_t len;
    char *data = read_all(name, &len);
    if (!data) {
        printf("imgview: %s: no such file\n", name);
        return 1;
    }
    image img;
    char error[96];
    u64 started = uptime_ms();
    if (image_decode((unsigned char *)data, len, &img, error) != 0) {
        printf("imgview: %s: %s\n", name, error);
        return 1;
    }
    u64 took = uptime_ms() - started;
    free(data);

    if (info) {
        /* Size, then pixels at a few fixed places, for comparing decoders. */
        printf("%s: %dx%d, decoded in %llu ms\n", name, img.width, img.height, took);
        int points[5][2] = {{0, 0}, {img.width - 1, 0}, {img.width / 2, img.height / 2}, {0, img.height - 1},
                            {img.width - 1, img.height - 1}};
        for (int i = 0; i < 5; i++) {
            unsigned p = img.pixels[(size_t)points[i][1] * (size_t)img.width + points[i][0]];
            printf("pixel %d,%d = %02x %02x %02x %02x\n", points[i][0], points[i][1], p >> 16 & 255, p >> 8 & 255,
                   p & 255, p >> 24);
        }
        /* Every pixel, two ways: an exact checksum (fully transparent
           pixels count as 0, their colour being meaningless), and the
           average colour of each cell of a 4x4 grid. */
        unsigned sum = 2166136261u;
        u64 cells[16][3];
        int counts[16];
        memset(cells, 0, sizeof(cells));
        memset(counts, 0, sizeof(counts));
        for (int y = 0; y < img.height; y++)
            for (int x = 0; x < img.width; x++) {
                unsigned p = img.pixels[(size_t)y * (size_t)img.width + x];
                if ((p >> 24) == 0) p = 0;
                for (int k = 0; k < 4; k++) sum = (sum ^ (p >> (8 * k) & 255)) * 16777619u;
                int cell = (y * 4 / img.height) * 4 + x * 4 / img.width;
                cells[cell][0] += p >> 16 & 255;
                cells[cell][1] += p >> 8 & 255;
                cells[cell][2] += p & 255;
                counts[cell]++;
            }
        printf("checksum %08x\ngrid", sum);
        for (int i = 0; i < 16; i++) {
            int n = counts[i] ? counts[i] : 1;
            printf(" %02x%02x%02x", (unsigned)(cells[i][0] / (u64)n), (unsigned)(cells[i][1] / (u64)n),
                   (unsigned)(cells[i][2] / (u64)n));
        }
        printf("\n");
        return 0;
    }

    if (gui_connect() < 0) {
        printf("imgview: the desktop is not running (start it with: desktop)\n");
        return 1;
    }
    /* Fit it, keeping its shape. */
    int zoom100 = 100;
    if (img.width > MAX_W || img.height > MAX_H) {
        int zw = MAX_W * 100 / img.width, zh = MAX_H * 100 / img.height;
        zoom100 = zw < zh ? zw : zh;
    }
    int fit = zoom100;
    int ww = img.width * zoom100 / 100, wh = img.height * zoom100 / 100;
    if (ww < 240) ww = 240;
    if (wh < 120) wh = 120;
    if (ww > MAX_W) ww = MAX_W;
    if (wh > MAX_H) wh = MAX_H;
    char title[40] = "Image - ";
    const char *base = name;
    for (const char *p = name; *p; p++) {
        if (*p == '/') base = p + 1;
    }
    for (size_t i = 0, n = strlen(title); base[i] && n < sizeof(title) - 1; i++) title[n++] = base[i], title[n] = 0;
    gui_window *win = gui_window_new(ww, wh, title);
    if (!win) return 1;
    for (;;) {
        /* A checkerboard behind, so transparent parts can be seen. */
        for (int y = 0; y < wh; y += 8)
            for (int x = 0; x < ww; x += 8) fill_rect(&win->pixels, x, y, 8, 8, ((x ^ y) & 8) ? 0xD8D8D8 : 0xF4F4F4);
        int w = img.width * zoom100 / 100, h = img.height * zoom100 / 100;
        if (w < 1) w = 1;
        if (h < 1) h = 1;
        image_draw(&win->pixels, &img, (ww - w) / 2, (wh - h) / 2, w, h);
        gui_update(win, 0, 0, ww, wh);
        gui_message m;
        int got = gui_next(&m, -1);
        if (got < 0) break;
        if (m.type == MSG_CLOSE) break;
        if (m.type == MSG_KEY) {
            if (m.a == 'q' || m.a == KEY_ESCAPE) break;
            if (m.a == '+' || m.a == '=') zoom100 = zoom100 * 5 / 4 + 1;
            if (m.a == '-' && zoom100 > 2) zoom100 = zoom100 * 4 / 5;
            if (m.a == '0') zoom100 = fit;
        }
    }
    gui_window_close(win);
    return 0;
}
