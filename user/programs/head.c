/* head - the first lines (10, or -n N) of a file or of standard input. */
#include "rdx.h"

int main(int argc, char **argv) {
    int want = 10;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) want = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') want = atoi(argv[i] + 1);
        else name = argv[i];
    }
    if (name) {
        int count;
        char *data = read_all(name, NULL);
        if (!data) {
            printf("head: %s: no such file\n", name);
            return 1;
        }
        char **lines = split_lines(data, &count);
        for (int i = 0; i < count && i < want; i++) puts(lines[i]);
        return 0;
    }
    /* From a pipe, stop reading as soon as we have enough: the program
       feeding us may never finish on its own. */
    int printed = 0, c;
    while (printed < want && (c = getchar()) >= 0) {
        putchar(c);
        if (c == '\n') printed++;
    }
    return 0;
}
