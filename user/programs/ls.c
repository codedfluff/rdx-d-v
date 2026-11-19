/* ls - list files. -l adds sizes and when each was last changed. An
   argument names a directory to list, or else filters by part of the name. */
#include "rdx.h"

static int contains(const char *text, const char *part) {
    size_t n = strlen(part);
    for (; *text; text++) {
        if (memcmp(text, part, n) == 0) return 1;
    }
    return n == 0;
}

int main(int argc, char **argv) {
    int details = 0;
    const char *filter = "";
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-l") == 0) {
            details = 1;
        } else if (chdir(argv[i]) == 0) {
            /* It names a directory: list its contents. */
        } else {
            filter = argv[i]; /* otherwise, filter names by this text */
        }
    }
    /* file_list returns a size, -2 for a directory, or -1 past the end. */
    char name[64];
    i64 size, total = 0;
    int count = 0, dirs = 0;
    for (int i = 0; (size = file_list(i, name, sizeof(name))) != -1; i++) {
        if (!contains(name, filter)) continue;
        char when[20] = "-";
        u64 modified = 0;
        if (details && file_stat(name, NULL, &modified) >= 0 && modified) format_time(modified, when);
        if (size == -2) {
            if (details) printf("   <dir>  %19s  %s/\n", when, name);
            else printf("%s/\n", name);
            dirs++;
        } else {
            if (details) printf("%8lld  %19s  %s\n", size, when, name);
            else printf("%s\n", name);
            total += size;
            count++;
        }
    }
    if (details) printf("%d file(s), %d dir(s), %lld bytes\n", count, dirs, total);
    return 0;
}
