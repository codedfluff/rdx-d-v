/* du - add up the space used by a directory and everything below it.
   Usage: du [directory]
   Prints the total for each directory, deepest first, ending with the
   grand total. */
#include "rdx.h"

#define MAX_ENTRIES 128

/* Total bytes under the current directory, which is called `path`. */
static i64 walk(const char *path) {
    /* Collect the entries first: chdir would disturb a running listing. */
    char (*names)[64] = malloc(MAX_ENTRIES * 64);
    i64 *sizes = malloc(MAX_ENTRIES * sizeof(i64));
    if (!names || !sizes) return 0;
    int count = 0;
    char name[64];
    i64 size, total = 0;
    for (int i = 0; count < MAX_ENTRIES && (size = file_list(i, name, sizeof(name))) != -1; i++) {
        strcpy(names[count], name);
        sizes[count++] = size;
    }
    for (int i = 0; i < count; i++) {
        if (sizes[i] != -2) {
            total += sizes[i];
            continue;
        }
        char child[256];
        size_t n = strlen(path);
        if (n + strlen(names[i]) + 2 > sizeof(child) || chdir(names[i]) != 0) continue;
        strcpy(child, path);
        if (n && child[n - 1] != '/') child[n++] = '/';
        strcpy(child + n, names[i]);
        total += walk(child);
        chdir("..");
    }
    free(names);
    free(sizes);
    printf("%8lld  %s\n", total, path);
    return total;
}

int main(int argc, char **argv) {
    const char *start = argc > 1 ? argv[1] : ".";
    if (chdir(start) != 0) {
        printf("du: %s: no such directory\n", start);
        return 1;
    }
    walk(start);
    return 0;
}
