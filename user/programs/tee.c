/* tee - copy standard input to standard output and also into a file. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: tee <file>\n");
        return 1;
    }
    size_t len;
    char *data = read_all(NULL, &len);
    file_write(argv[1], data, len);
    fd_write(1, data, len);
    return 0;
}
