/* uptime - time since boot. The kernel keeps this as the file /proc/uptime. */
#include "rdx.h"

int main(void) { return print_file("/proc/uptime"); }
