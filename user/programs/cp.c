/* cp - copy a file. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: cp <file> <new name>\n");
        return 1;
    }
    size_t len;
    char *data = read_all(argv[1], &len);
    if (!data) {
        printf("cp: %s: no such file\n", argv[1]);
        return 1;
    }
    if (strcmp(argv[1], argv[2]) == 0) {
        printf("cp: the two names are the same\n");
        return 1;
    }
    return file_write(argv[2], data, len) == 0 ? 0 : 1;
}
