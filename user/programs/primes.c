/* primes.exe - count primes with a sieve. Usage: primes [limit]
   Exercises the heap (malloc -> sbrk) and gets preempted by the timer. */
#include "rdx.h"

int main(int argc, char **argv) {
    int limit = argc > 1 ? atoi(argv[1]) : 2000000;
    if (limit < 2 || limit > 50000000) {
        printf("limit must be between 2 and 50000000\n");
        return 1;
    }

    u64 start = uptime_ms();
    unsigned char *composite = malloc((size_t)limit + 1);
    if (!composite) {
        printf("out of memory\n");
        return 1;
    }
    memset(composite, 0, (size_t)limit + 1);

    int count = 0, largest = 0;
    for (int n = 2; n <= limit; n++) {
        if (composite[n]) continue;
        count++;
        largest = n;
        for (u64 m = (u64)n * n; m <= (u64)limit; m += n) composite[m] = 1;
    }

    printf("%d primes below %d (largest: %d), found in %llu ms\n", count, limit, largest, uptime_ms() - start);
    return 0;
}
