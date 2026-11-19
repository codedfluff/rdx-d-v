/* uname - which system this is. -a adds the details. */
#include "rdx.h"

int main(int argc, char **argv) {
    sys_info s;
    sysinfo(&s);
    if (argc > 1 && strcmp(argv[1], "-a") == 0) {
        printf("RDX %s x86_64, %llu CPU core(s), %llu MiB memory\n", s.version, s.cpus, s.mem_total >> 20);
    } else {
        printf("RDX\n");
    }
    return 0;
}
