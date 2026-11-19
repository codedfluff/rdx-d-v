/* spawn - start kernel worker threads, each of which prints five ticks a
   second apart. A test that the kernel's own threads share the processors
   fairly with everything else. Usage: spawn [count] */
#include "rdx.h"

int main(int argc, char **argv) {
    int count = argc > 1 ? atoi(argv[1]) : 1;
    if (count < 1) count = 1;
    if (count > 16) count = 16;
    for (int i = 0; i < count; i++) printf("spawned task %d\n", kernel_debug(0));
    return 0;
}
