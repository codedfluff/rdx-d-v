/* panic - make the kernel panic on purpose, to see what that looks like.
   The machine stops; you will need to restart it. */
#include "rdx.h"

int main(void) {
    kernel_debug(2);
    return 1;
}
