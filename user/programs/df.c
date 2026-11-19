/* df - how much room the RDX disk has, and how much of it is in use. */
#include "rdx.h"

int main(void) {
    sys_info s;
    sysinfo(&s);
    printf("%llu file(s) holding %llu KiB in memory\n", s.file_count, (s.file_bytes + 1023) / 1024);
    if (!s.disk_capacity) {
        printf("no RDX disk attached: nothing is being saved\n");
        return 0;
    }
    u64 free_bytes = s.disk_capacity > s.disk_used ? s.disk_capacity - s.disk_used : 0;
    printf("disk: %llu KiB used of %llu KiB (%llu%%), %llu KiB free\n", (s.disk_used + 1023) / 1024,
           s.disk_capacity / 1024, s.disk_used * 100 / s.disk_capacity, free_bytes / 1024);
    return 0;
}
