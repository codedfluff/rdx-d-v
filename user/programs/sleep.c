/* sleep - do nothing for a while. Usage: sleep <milliseconds> */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 2 || argv[1][0] < '0' || argv[1][0] > '9') {
        printf("usage: sleep <milliseconds>\n");
        return 1;
    }
    sleep_ms((u64)atoi(argv[1]));
    return 0;
}
