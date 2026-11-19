/* gfxtest - take over the display, draw a test pattern, give it back.
   Usage: gfxtest [crash]
   Shows that drawing works and the screen's colours are the right way
   round. "crash" makes it crash on purpose while it has the display, to
   show that the text console still comes back. */
#include "rdx.h"

int main(int argc, char **argv) {
    display_info screen;
    int events = display_open(&screen);
    if (events < 0) {
        printf("gfxtest: cannot use the display: %s\n", last_error());
        return 1;
    }
    canvas c = canvas_new(screen.width, screen.height);
    if (!c.pixels) return 1;
    /* Colour bars, a grey ramp and some text. */
    static const unsigned bars[] = {0xFFFFFF, 0xFFFF00, 0x00FFFF, 0x00FF00, 0xFF00FF, 0xFF0000, 0x0000FF, 0x000000};
    int bar = screen.width / 8;
    for (int i = 0; i < 8; i++) fill_rect(&c, i * bar, 0, bar, screen.height * 2 / 3, bars[i]);
    for (int x = 0; x < screen.width; x++) {
        unsigned v = (unsigned)(x * 255 / (screen.width - 1));
        fill_rect(&c, x, screen.height * 2 / 3, 1, screen.height / 3, v << 16 | v << 8 | v);
    }
    fill_rect(&c, 40, 40, 360, 40, 0x000000);
    draw_text(&c, 52, 52, "RDX graphics test pattern", 0xFFFFFF);
    present(&screen, &c, 0, 0, screen.width, screen.height);

    /* Check the screen really holds what was drawn (in its own format). */
    unsigned want = bars[5]; /* red */
    unsigned got = screen.pixels[(size_t)(screen.height / 3) * (size_t)screen.stride + 5 * bar + bar / 2];
    if (screen.format == 1) got = (got & 0xFF00) | (got >> 16 & 0xFF) | (got & 0xFF) << 16;
    sleep_ms(300);

    if (argc > 1 && strcmp(argv[1], "crash") == 0) {
        volatile unsigned *nowhere = (unsigned *)0;
        *nowhere = 1; /* the kernel ends us here; the console must come back */
    }
    close(events);
    printf("gfxtest: %dx%d, format %d, pixel check %s\n", screen.width, screen.height, screen.format,
           (got & 0xFFFFFF) == want ? "ok" : "WRONG");
    return 0;
}
