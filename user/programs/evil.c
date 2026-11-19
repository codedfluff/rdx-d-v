/* evil.exe - tries to scribble on kernel memory. The CPU refuses, because
   this runs in ring 3, and the kernel kills the process. */
#include "rdx.h"

int main(void) {
    printf("Attempting to overwrite kernel memory at 0x200000...\n");
    volatile u64 *kernel = (volatile u64 *)0x200000;
    *kernel = 0xDEADBEEF;
    printf("If you can read this, memory protection is broken!\n");
    return 0;
}
