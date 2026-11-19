/* echo - print its arguments. -n leaves off the newline. */
#include "rdx.h"

int main(int argc, char **argv) {
    int first = 1, newline = 1;
    if (argc > 1 && strcmp(argv[1], "-n") == 0) {
        newline = 0;
        first = 2;
    }
    for (int i = first; i < argc; i++) printf(i > first ? " %s" : "%s", argv[i]);
    if (newline) putchar('\n');
    return 0;
}
