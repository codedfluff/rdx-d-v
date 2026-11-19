/* ps - list the tasks the kernel is running: programs and its own threads. */
#include "rdx.h"

int main(void) {
    static const char *states[] = TASK_STATES;
    proc_info p;
    printf(" PID  TID  STATE     CORE  RING  CPU(s)  NAME\n");
    for (int i = 0; proc_list(i, &p) == 0; i++) {
        const char *state = states[p.state > 4 ? 4 : p.state];
        printf("%4u %4u  %s", p.pid, p.tid, state);
        for (size_t pad = strlen(state); pad < 8; pad++) putchar(' ');
        if (p.cpu >= 0) printf("  %4d", p.cpu);
        else printf("     -");
        printf("  %4d  %3llu.%02llu  %s\n", p.user ? 3 : 0, p.cpu_ms / 1000, p.cpu_ms % 1000 / 10, p.name);
    }
    return 0;
}
