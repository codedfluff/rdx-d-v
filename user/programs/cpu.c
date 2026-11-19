/* cpu - what the processor is. The kernel keeps this as the file /proc/cpu. */
#include "rdx.h"

int main(void) { return print_file("/proc/cpu"); }
