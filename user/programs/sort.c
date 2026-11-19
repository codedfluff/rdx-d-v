/* sort - sort the lines of a file or of standard input.
   -r reverses, -n compares as numbers. */
#include "rdx.h"

static int numeric, reverse;

static int before(const char *a, const char *b) {
    int c = numeric ? atoi(a) - atoi(b) : strcmp(a, b);
    return reverse ? c > 0 : c < 0;
}

/* Merge sort: stable, and fast on any input. */
static void merge_sort(char **lines, char **scratch, int n) {
    if (n < 2) return;
    int half = n / 2;
    merge_sort(lines, scratch, half);
    merge_sort(lines + half, scratch, n - half);
    int i = 0, j = half, k = 0;
    while (i < half && j < n) scratch[k++] = before(lines[j], lines[i]) ? lines[j++] : lines[i++];
    while (i < half) scratch[k++] = lines[i++];
    while (j < n) scratch[k++] = lines[j++];
    memcpy(lines, scratch, sizeof(char *) * (size_t)n);
}

int main(int argc, char **argv) {
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-r") == 0) reverse = 1;
        else if (strcmp(argv[i], "-n") == 0) numeric = 1;
        else if (strcmp(argv[i], "-rn") == 0 || strcmp(argv[i], "-nr") == 0) reverse = numeric = 1;
        else name = argv[i];
    }
    int count;
    char *data = read_all(name, NULL);
    if (!data) {
        printf("sort: %s: no such file\n", name);
        return 1;
    }
    char **lines = split_lines(data, &count);
    merge_sort(lines, malloc(sizeof(char *) * (size_t)(count + 1)), count);
    for (int i = 0; i < count; i++) puts(lines[i]);
    return 0;
}
