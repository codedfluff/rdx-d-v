/* kill - send a signal to a process.
   Usage: kill [-signal] <pid>...
   The default signal, 15 (terminate), asks the program to stop: it may
   clean up first, or even refuse. -9 (kill) cannot be caught or refused.
   -2 (interrupt) is what Ctrl+C sends. */
#include "rdx.h"

int main(int argc, char **argv) {
    int number = SIGTERM, failed = 0, targets = 0;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            number = atoi(argv[i] + 1);
            if (number < 1 || number > 31) {
                printf("kill: %s is not a signal (use 2, 9 or 15)\n", argv[i]);
                return 1;
            }
            continue;
        }
        targets++;
        if (kill(atoi(argv[i]), number) != 0) {
            printf("kill: no process %s\n", argv[i]);
            failed = 1;
        }
    }
    if (!targets) {
        printf("usage: kill [-signal] <pid>...   (signals: 2 interrupt, 15 terminate, 9 kill)\n");
        return 1;
    }
    return failed;
}
