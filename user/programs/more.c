/* more - show a file (or standard input) one screen at a time.
   Space or Enter: next page.  q: quit. */
#include "rdx.h"

int main(int argc, char **argv) {
    int count, cols, rows;
    char *data = read_all(argc > 1 ? argv[1] : NULL, NULL);
    if (!data) {
        printf("more: %s: no such file\n", argv[1]);
        return 1;
    }
    char **lines = split_lines(data, &count);
    screen_size(&cols, &rows);
    for (int i = 0; i < count; i++) {
        puts(lines[i]);
        if ((i + 1) % (rows - 1) == 0 && i + 1 < count) {
            screen_color(0x57C7FF);
            printf("-- more (%d of %d lines) --", i + 1, count);
            flush();
            screen_color(COLOR_DEFAULT);
            /* Keys come from the keyboard even when our input is a pipe. */
            int key = console_key();
            printf("\r");
            flush();
            screen_clear_eol();
            if (key == 'q' || key == KEY_ESCAPE) break;
        }
    }
    return 0;
}
