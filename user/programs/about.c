/* about - what this system is. The kernel keeps this as the file /proc/about. */
#include "rdx.h"

int main(void) { return print_file("/proc/about"); }
