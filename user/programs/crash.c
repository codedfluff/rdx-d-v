/* crash - start a kernel thread that dereferences a null pointer. A test
   that a fault inside one kernel thread ends that thread, not the system. */
#include "rdx.h"

int main(void) {
    printf("spawned task %d which will dereference a null pointer\n", kernel_debug(1));
    sleep_ms(50); /* let its crash report appear before the prompt */
    return 0;
}
