/* ptyrun - run a program inside a pseudo-terminal, and be its terminal.
   Usage: ptyrun [command]     (default: sh)

   Keys you type are passed in; what it prints is passed out to the screen,
   including its colours and cursor movement, which arrive as ANSI escape
   sequences. Ctrl+C goes to whatever it is running. This is exactly the
   job a terminal window does, minus the window: a test of the machinery,
   and a way to see it work. */
#include "rdx.h"

static volatile int interrupts;

/* Ctrl+C on the console reaches us (we are the shell's foreground
   program). Pass it on into the pseudo-terminal instead of stopping. */
static void on_interrupt(int number) {
    (void)number;
    interrupts++;
}

int main(int argc, char **argv) {
    (void)argv;
    const char *command = argc > 1 ? command_tail() : "sh";
    int ends[2];
    if (pty_open(ends) != 0) {
        printf("ptyrun: cannot make a pseudo-terminal\n");
        return 1;
    }
    int master = ends[0];
    int cols, rows;
    screen_size(&cols, &rows);
    pty_resize(master, cols, rows);
    int child = spawn_fds(command, ends[1], ends[1], ends[1]);
    close(ends[1]); /* only the child holds the program end now */
    if (child < 0) {
        printf("ptyrun: cannot run %s: %s\n", command, last_error());
        return 1;
    }
    signal(SIGINT, on_interrupt);
    printf("[ptyrun: %s is running in a pseudo-terminal]\n", command);

    char buf[4096];
    for (;;) {
        while (interrupts > 0) {
            interrupts--;
            fd_write(master, "\x03", 1);
        }
        pollfd watch[2] = {{0, POLLIN, 0}, {master, POLLIN, 0}};
        int ready = poll(watch, 2, -1);
        if (ready == EINTR) continue;
        if (ready < 0) break;
        if (watch[1].revents & (POLLIN | POLLHUP)) {
            i64 n = read(master, buf, sizeof(buf));
            if (n <= 0) break; /* everything there has finished */
            fd_write(1, buf, (size_t)n);
        }
        if (watch[0].revents & POLLIN) {
            int c;
            while ((c = pollchar()) >= 0) {
                char key = (char)c;
                fd_write(master, &key, 1);
            }
        }
    }
    close(master);
    int status = wait(child);
    printf("[ptyrun: %s finished with code %d]\n", command, status);
    return 0;
}
