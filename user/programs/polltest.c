/* polltest - checks that poll() behaves: time limits, waking on input from
   another program, hang-ups, and watching many descriptors at once.
   Prints one line per check and "POLL OK" if they all passed. */
#include "rdx.h"

static int failures;

static void check(const char *what, int passed) {
    printf("%s  %s\n", passed ? "ok  " : "FAIL", what);
    if (!passed) failures++;
}

int main(void) {
    int a[2], b[2];
    pipe(a);
    pipe(b);

    /* Nothing to read: it must wait out the time limit, then return 0. */
    pollfd p[2] = {{a[0], POLLIN, 0}, {b[0], POLLIN, 0}};
    u64 started = uptime_ms();
    int n = poll(p, 2, 300);
    u64 took = uptime_ms() - started;
    check("an idle poll times out after its limit", n == 0 && took >= 290 && took < 600);

    /* Something to read on the second: only that one is reported. */
    fd_write(b[1], "x", 1);
    n = poll(p, 2, 1000);
    check("data on one pipe is reported for that pipe only",
          n == 1 && !(p[0].revents & POLLIN) && (p[1].revents & POLLIN));
    char c;
    read(b[0], &c, 1);

    /* Writable: an empty pipe has room. */
    pollfd w = {a[1], POLLOUT, 0};
    check("an empty pipe can be written", poll(&w, 1, 0) == 1 && (w.revents & POLLOUT));

    /* Another program writes after a delay: poll wakes when it does, not
       at the time limit. */
    int child = spawn("sleep 200", -1, -1);
    int helper = spawn("echo late", -1, a[1]);
    started = uptime_ms();
    n = poll(p, 2, 5000);
    took = uptime_ms() - started;
    check("poll wakes as soon as another program writes", n == 1 && (p[0].revents & POLLIN) && took < 2000);
    wait(helper);
    wait(child);
    char line[16];
    read(a[0], line, sizeof(line));

    /* The writer closes: the reader hears about it. */
    close(b[1]);
    n = poll(p, 2, 1000);
    check("closing the other end is reported as a hang-up", n >= 1 && (p[1].revents & POLLHUP));

    /* A slot that is not open, and one the caller is skipping. */
    pollfd odd[2] = {{57, POLLIN, 0}, {-1, POLLIN, 0}};
    n = poll(odd, 2, 0);
    check("a closed descriptor is flagged, a negative one skipped",
          n == 1 && (odd[0].revents & POLLNVAL) && odd[1].revents == 0);

    /* A pseudo-terminal: what the program writes appears at the terminal end. */
    int t[2];
    check("a pseudo-terminal can be opened", pty_open(t) == 0);
    pollfd tp = {t[0], POLLIN, 0};
    check("nothing to read from a new terminal", poll(&tp, 1, 0) == 0);
    fd_write(t[1], "hi", 2);
    check("the program's output reaches the terminal end", poll(&tp, 1, 1000) == 1 && (tp.revents & POLLIN));

    printf(failures ? "POLL FAILED\n" : "POLL OK\n");
    return failures != 0;
}
