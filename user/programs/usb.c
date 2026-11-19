/* usb - USB controllers, keyboards and drives. The kernel keeps this as the file /proc/usb. */
#include "rdx.h"

int main(void) { return print_file("/proc/usb"); }
