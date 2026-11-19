/* pingpong - measures how quickly the kernel hands control between two
   programs that are waiting on each other. It bounces one byte back and
   forth through a pair of pipes with a copy of itself; every bounce needs
   the kernel to wake the other side.

   When a waiting program is woken the moment its data arrives, a round trip
   takes microseconds. (If waiting meant looking again at every timer tick,
   each round trip would take two ticks: 20 milliseconds.)
   Usage: pingpong [round trips] */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc > 1 && strcmp(argv[1], "--echo") == 0) {
        /* The other side: send back every byte that arrives. */
        char c;
        while (read(0, &c, 1) == 1) fd_write(1, &c, 1);
        return 0;
    }
    int rounds = argc > 1 ? atoi(argv[1]) : 1000;
    int to_child[2], from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0) {
        printf("pingpong: cannot make pipes\n");
        return 1;
    }
    int child = spawn("pingpong --echo", to_child[0], from_child[1]);
    if (child < 0) {
        printf("pingpong: cannot start the other side\n");
        return 1;
    }
    close(to_child[0]);
    close(from_child[1]);

    u64 started = uptime_ms();
    char c = 'x';
    for (int i = 0; i < rounds; i++) {
        if (fd_write(to_child[1], &c, 1) != 1 || read(from_child[0], &c, 1) != 1) {
            printf("pingpong: the other side went away\n");
            return 1;
        }
    }
    u64 took = uptime_ms() - started;
    close(to_child[1]); /* the echo side sees end of input and exits */
    wait(child);
    printf("%d round trips in %llu ms", rounds, took);
    if (rounds > 0) printf(" (%llu microseconds each)", took * 1000 / (u64)rounds);
    printf("\n");
    return 0;
}
