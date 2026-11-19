/* uniq - drop repeated neighbouring lines (sort first to drop all repeats).
   -c shows how many times each line occurred. */
#include "rdx.h"

int main(int argc, char **argv) {
    int show_counts = 0;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) show_counts = 1;
        else name = argv[i];
    }
    int count;
    char *data = read_all(name, NULL);
    if (!data) {
        printf("uniq: %s: no such file\n", name);
        return 1;
    }
    char **lines = split_lines(data, &count);
    for (int i = 0; i < count;) {
        int j = i + 1;
        while (j < count && strcmp(lines[i], lines[j]) == 0) j++;
        if (show_counts) printf("%4d %s\n", j - i, lines[i]);
        else puts(lines[i]);
        i = j;
    }
    return 0;
}
