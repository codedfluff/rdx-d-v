/* net - the network adapter, its address and open sockets. The kernel keeps this as the file /proc/net. */
#include "rdx.h"

int main(void) { return print_file("/proc/net"); }
