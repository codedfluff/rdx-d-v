/* mkdir - make directories. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: mkdir <directory>...\n");
        return 1;
    }
    int failed = 0;
    for (int i = 1; i < argc; i++) {
        if (mkdir(argv[i]) != 0) {
            printf("mkdir: %s: %s\n", argv[i], last_error());
            failed = 1;
        }
    }
    return failed;
}
