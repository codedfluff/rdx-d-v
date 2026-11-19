/* disk - the disk that files are saved to. The kernel keeps this as the file /proc/disk. */
#include "rdx.h"

int main(void) { return print_file("/proc/disk"); }
