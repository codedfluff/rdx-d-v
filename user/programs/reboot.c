/* reboot - restart the machine. */
#include "rdx.h"

int main(void) {
    reboot();
    return 1; /* only reached if the kernel refused */
}
