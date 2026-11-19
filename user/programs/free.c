/* free - memory in use. */
#include "rdx.h"

int main(void) {
    sys_info s;
    sysinfo(&s);
    printf("            total       used       free\n");
    printf("memory %8llu K %8llu K %8llu K\n", s.mem_total / 1024, s.mem_used / 1024,
           (s.mem_total - s.mem_used) / 1024);
    printf("kernel %8llu K %8llu K %8llu K   (the heap the kernel keeps for itself)\n", s.heap_size / 1024,
           s.heap_used / 1024, (s.heap_size - s.heap_used) / 1024);
    return 0;
}
