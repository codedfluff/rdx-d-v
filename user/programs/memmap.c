/* memmap - the usable regions of physical memory. The kernel keeps this as the file /proc/memmap. */
#include "rdx.h"

int main(void) { return print_file("/proc/memmap"); }
