/* seq - print a run of numbers: seq 5, or seq 3 9. */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        printf("usage: seq [first] <last>\n");
        return 1;
    }
    int first = argc == 3 ? atoi(argv[1]) : 1;
    int last = atoi(argv[argc - 1]);
    for (int n = first; n <= last; n++) printf("%d\n", n);
    return 0;
}
