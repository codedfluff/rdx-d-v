/* rm - delete files, or directories with -r. */
#include "rdx.h"

int main(int argc, char **argv) {
    int recursive = 0, status = 0, targets = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) {
            recursive = 1;
            continue;
        }
        targets++;
        if (file_delete(argv[i]) == 0) continue; /* a file */
        /* Not a file: maybe a directory. */
        int removed = recursive ? (int)syscall(42, (i64)argv[i], 1, 0) : rmdir(argv[i]);
        if (removed != 0) {
            printf("rm: %s: no such file%s\n", argv[i], recursive ? "" : " (use -r for a directory)");
            status = 1;
        }
    }
    if (targets == 0) {
        printf("usage: rm [-r] <file>...\n");
        return 1;
    }
    return status;
}
