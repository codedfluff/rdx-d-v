/* shutdown - power the machine off. */
#include "rdx.h"

int main(void) {
    shutdown();
    return 1; /* only reached if the kernel refused */
}
