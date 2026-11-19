/* rev - reverse each line of a file or of standard input. */
#include "rdx.h"

int main(int argc, char **argv) {
    int count;
    char *data = read_all(argc > 1 ? argv[1] : NULL, NULL);
    if (!data) {
        printf("rev: %s: no such file\n", argv[1]);
        return 1;
    }
    char **lines = split_lines(data, &count);
    for (int i = 0; i < count; i++) {
        for (int n = (int)strlen(lines[i]); n > 0; n--) putchar(lines[i][n - 1]);
        putchar('\n');
    }
    return 0;
}
