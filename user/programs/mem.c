/* mem - memory in use. The kernel keeps this as the file /proc/mem. */
#include "rdx.h"

int main(void) { return print_file("/proc/mem"); }
