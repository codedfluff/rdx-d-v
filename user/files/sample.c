/* sample.c - a first program to compile inside RDX.

       c-comp sample.c
       sample 20                                                       */
#include "rdx.h"

/* How many steps does n take to reach 1 in the "3n + 1" game? */
int steps(long long n) {
    int count = 0;
    while (n != 1) {
        if (n % 2 == 0) {
            n = n / 2;
        } else {
            n = n * 3 + 1;
        }
        count++;
    }
    return count;
}

int main(int argc, char **argv) {
    int limit = 10;
    if (argc > 1) limit = atoi(argv[1]);

    printf("Compiled inside RDX by c-comp. Process %d.\n", getpid());
    int best = 1;
    for (int n = 1; n <= limit; n++) {
        int s = steps(n);
        printf("%4d takes %3d steps\n", n, s);
        if (s > steps(best)) best = n;
    }
    printf("Longest: %d\n", best);
    return 0;
}
