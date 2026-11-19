/* rmdir - remove empty directories. (rm -r removes full ones.) */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: rmdir <directory>...\n");
        return 1;
    }
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        if (rmdir(argv[i]) != 0) {
            printf("rmdir: %s: %s\n", argv[i], last_error());
            failed = 1;
        }
    }
    return failed;
}
