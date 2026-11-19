/* touch - create empty files that do not exist yet. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: touch <file>...\n");
        return 1;
    }
    for (int i = 1; i < argc; i++) {
        if (file_read(argv[i], NULL, 0) < 0) file_write(argv[i], "", 0);
    }
    return 0;
}
