/* mv - rename a file. Refuses to replace an existing one. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc != 3) {
        printf("usage: mv <file> <new name>\n");
        return 1;
    }
    size_t len;
    char *data = read_all(argv[1], &len);
    if (!data) {
        printf("mv: %s: no such file\n", argv[1]);
        return 1;
    }
    if (strcmp(argv[1], argv[2]) == 0) {
        printf("mv: the two names are the same\n");
        return 1;
    }
    if (file_read(argv[2], NULL, 0) >= 0) {
        printf("mv: a file with the new name already exists (rm it first)\n");
        return 1;
    }
    if (file_write(argv[2], data, len) != 0) return 1;
    file_delete(argv[1]);
    return 0;
}
