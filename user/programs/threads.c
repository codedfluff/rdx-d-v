/* threads.exe - shows threads running in parallel on several CPU cores.
   Usage: threads [thread count]

   1. Counts primes below a limit, first with one thread and then with the
      work split across several. On a multi-core machine the second run is
      faster by roughly the number of cores.
   2. Has all the threads hammer one shared counter, to show that a mutex
      keeps the result exact. */
#include "rdx.h"

#define LIMIT 1500000
#define CHUNK 1000
#define BUMPS 50000

/* The range is cut into chunks of 1000 numbers, dealt out to the threads
   like cards, so each gets the same mix of cheap and expensive numbers. */
typedef struct {
    int first_chunk, stride;
    int found;
} Job;

static int is_prime(int n) {
    if (n < 2) return 0;
    for (int d = 2; d * d <= n; d++) {
        if (n % d == 0) return 0;
    }
    return 1;
}

static void count_primes(void *arg) {
    Job *job = arg;
    int found = 0;
    for (int chunk = job->first_chunk; chunk * CHUNK < LIMIT; chunk += job->stride) {
        for (int n = chunk * CHUNK; n < (chunk + 1) * CHUNK && n < LIMIT; n++) found += is_prime(n);
    }
    job->found = found;
}

static mutex_t counter_lock;
static int shared_counter;

static void bump(void *arg) {
    (void)arg;
    for (int i = 0; i < BUMPS; i++) {
        mutex_lock(&counter_lock);
        shared_counter++;
        mutex_unlock(&counter_lock);
    }
}

int main(int argc, char **argv) {
    int cores = cpu_count();
    int threads = argc > 1 ? atoi(argv[1]) : (cores > 1 ? cores : 2);
    if (threads < 1 || threads > 32) {
        printf("thread count must be between 1 and 32\n");
        return 1;
    }
    printf("%d CPU core(s); using %d threads.\n", cores, threads);

    /* One thread does everything. */
    Job whole = {0, 1, 0};
    u64 start = uptime_ms();
    count_primes(&whole);
    u64 single_ms = uptime_ms() - start;
    printf("1 thread:   %d primes below %d in %llu ms\n", whole.found, LIMIT, single_ms);

    /* The same work shared out between the threads. */
    Job *jobs = malloc(sizeof(Job) * (size_t)threads);
    int *ids = malloc(sizeof(int) * (size_t)threads);
    int total = 0;
    start = uptime_ms();
    for (int t = 0; t < threads; t++) {
        jobs[t].first_chunk = t;
        jobs[t].stride = threads;
        jobs[t].found = 0;
        ids[t] = thread_create(count_primes, &jobs[t]);
        if (ids[t] < 0) {
            printf("could not create thread %d\n", t);
            return 1;
        }
    }
    for (int t = 0; t < threads; t++) {
        thread_join(ids[t]);
        total += jobs[t].found;
    }
    u64 multi_ms = uptime_ms() - start;
    printf("%d threads:  %d primes below %d in %llu ms\n", threads, total, LIMIT, multi_ms);
    if (total != whole.found) {
        printf("MISMATCH: the two counts differ!\n");
        return 1;
    }
    if (multi_ms > 0) {
        printf("speed-up: %llu.%llux\n", single_ms / multi_ms, single_ms * 10 / multi_ms % 10);
    }

    /* Shared data under a lock. */
    for (int t = 0; t < threads; t++) ids[t] = thread_create(bump, NULL);
    for (int t = 0; t < threads; t++) thread_join(ids[t]);
    printf("shared counter: %d (expected %d) %s\n", shared_counter, threads * BUMPS,
           shared_counter == threads * BUMPS ? "OK" : "WRONG");
    return shared_counter == threads * BUMPS ? 0 : 1;
}
