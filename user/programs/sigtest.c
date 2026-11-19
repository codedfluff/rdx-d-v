/* sigtest - shows a program handling signals instead of being ended by them.
   Run it and press Ctrl+C: it reports each one and exits tidily on the
   third. `kill <pid>` from elsewhere makes it exit tidily at once.
   Usage: sigtest [wait]
   With "wait" it blocks in read() instead of sleeping, to show that a
   signal reaches a program that is waiting for input: read() returns EINTR
   and the program reads again (type a line and it is echoed back). */
#include "rdx.h"

static volatile int interrupts, asked_to_stop;

static void on_signal(int number) {
    if (number == SIGINT) {
        interrupts++;
        printf("caught interrupt %d of 3\n", interrupts);
    } else {
        asked_to_stop = 1;
        printf("caught signal %d\n", number);
    }
}

int main(int argc, char **argv) {
    int waiting = argc > 1 && strcmp(argv[1], "wait") == 0;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    printf("sigtest %d ready\n", getpid());
    while (interrupts < 3 && !asked_to_stop) {
        if (waiting) {
            /* If a signal arrives while this waits, the handler runs and
               read() then returns EINTR: go round and look at the flags. */
            char line[64];
            i64 n = read(0, line, sizeof(line) - 1);
            if (n == EINTR) continue;
            if (n <= 0) break;
            line[n] = 0;
            printf("read: %s", line);
        } else {
            sleep_ms(50);
        }
    }
    printf("sigtest finished cleanly\n");
    return 0;
}
