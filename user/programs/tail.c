/* tail - the last lines (10, or -n N) of a file or of standard input. */
#include "rdx.h"

int main(int argc, char **argv) {
    int want = 10;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-n") == 0 && i + 1 < argc) want = atoi(argv[++i]);
        else if (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9') want = atoi(argv[i] + 1);
        else name = argv[i];
    }
    int count;
    char *data = read_all(name, NULL);
    if (!data) {
        printf("tail: %s: no such file\n", name);
        return 1;
    }
    char **lines = split_lines(data, &count);
    for (int i = count > want ? count - want : 0; i < count; i++) puts(lines[i]);
    return 0;
}
