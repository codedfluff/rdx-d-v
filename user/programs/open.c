/* open - open something on the desktop, the right way for what it is.
   Usage: open <program> [arguments]   a text program gets its own terminal
                                       window ("Terminal - name.exe"); a
                                       graphical one is just started
          open <page.html>             shows it in the browser
          open <folder>                shows it in the file manager
   Returns at once: what it opened carries on by itself. */
#include "gui.h"

static int ends_with(const char *s, const char *tail) {
    size_t n = strlen(s), m = strlen(tail);
    return n >= m && strncasecmp(s + n - m, tail, m) == 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: open <program or file> [arguments]\n");
        return 1;
    }
    if (gui_connect() < 0) {
        printf("open: the desktop is not running (start it with: desktop)\n");
        return 1;
    }
    char command[256];
    const char *target = argv[1];
    if (ends_with(target, ".html") || ends_with(target, ".htm") || strncasecmp(target, "http://", 7) == 0) {
        strcpy(command, "browser ");
    } else if (file_stat(target, NULL, NULL) == 1) {
        strcpy(command, "files ");
    } else {
        command[0] = 0;
    }
    size_t n = strlen(command);
    const char *rest = command_tail();
    for (size_t i = 0; rest[i] && n < sizeof(command) - 1; i++) command[n++] = rest[i];
    command[n] = 0;
    int pid = gui_launch(command);
    if (pid < 0) {
        printf("open: %s: no such program or file\n", target);
        return 1;
    }
    return 0;
}
