/* top - a live view of what the machine is doing: how busy each task has
   been over the last second, and memory in use. Any key quits.
   Usage: top [-n count]   (print that many updates as plain text, then stop) */
#include "rdx.h"

#define MAX_TASKS 256

static proc_info now[MAX_TASKS], before[MAX_TASKS];
static int now_count, before_count;
static int interactive;

static u64 earlier_cpu(const proc_info *p) {
    for (int i = 0; i < before_count; i++) {
        if (before[i].tid == p->tid) return before[i].cpu_ms;
    }
    return 0;
}

/* End a line. On the live screen, first blank whatever the previous update
   left further along it. */
static void end_line(void) {
    if (interactive) screen_clear_eol();
    printf("\n");
}

int main(int argc, char **argv) {
    static const char *states[] = TASK_STATES;
    int rounds = argc > 2 && strcmp(argv[1], "-n") == 0 ? atoi(argv[2]) : -1;
    interactive = rounds < 0;
    sys_info sys;
    u64 before_ms = uptime_ms();
    if (interactive) {
        screen_mirror(0);
        screen_clear();
    }

    /* Round 0 only takes a baseline to measure the first second against. */
    for (int round = 0; rounds < 0 || round <= rounds; round++) {
        now_count = 0;
        while (now_count < MAX_TASKS && proc_list(now_count, &now[now_count]) == 0) now_count++;
        u64 now_ms = uptime_ms();
        u64 elapsed = now_ms > before_ms ? now_ms - before_ms : 1;
        sysinfo(&sys);

        if (round > 0) {
            if (interactive) screen_goto(0, 0);
            printf("RDX %s   up %llu s   %llu cores   %d tasks", sys.version, sys.uptime_ms / 1000, sys.cpus,
                   now_count);
            end_line();
            printf("memory: %llu MiB used of %llu MiB", sys.mem_used >> 20, sys.mem_total >> 20);
            end_line();
            end_line();
            printf("  PID  TID   CPU%%  STATE     NAME");
            end_line();
            for (int i = 0; i < now_count; i++) {
                const proc_info *p = &now[i];
                u64 tenths = (p->cpu_ms - earlier_cpu(p)) * 1000 / elapsed;
                if (tenths > 1000) tenths = 1000;
                const char *state = states[p->state > 4 ? 4 : p->state];
                printf(" %4u %4u  %3llu.%llu  %s", p->pid, p->tid, tenths / 10, tenths % 10, state);
                for (size_t pad = strlen(state); pad < 8; pad++) putchar(' ');
                printf("  %s", p->name);
                end_line();
            }
            /* Blank a few lines in case the list just got shorter. */
            for (int i = 0; interactive && i < 4; i++) end_line();
            flush();
        }
        memcpy(before, now, sizeof(now));
        before_count = now_count;
        before_ms = now_ms;

        if (rounds >= 0 && round == rounds) break;
        /* Wait a second, leaving at once if a key is pressed. */
        for (int waited = 0; waited < 10; waited++) {
            if (interactive && pollchar() >= 0) {
                screen_clear();
                screen_mirror(1);
                return 0;
            }
            sleep_ms(100);
        }
    }
    return 0;
}
