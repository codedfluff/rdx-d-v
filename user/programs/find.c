/* find - list files whose contents mention some text, searching the current
   directory and everything below it.
   Usage: find <text>      (to search by file name instead, use: ls <part>) */
#include "rdx.h"

static const char *needle;
static int hits;

/* Search the working directory, then descend into its subdirectories.
   `prefix` is the path shown in front of each name. */
static void search(const char *prefix) {
    /* Collect this directory's entries first: descending changes the working
       directory, which would disturb a running file_list scan. */
    char names[256][64];
    int is_dir[256];
    int count = 0;
    char name[64];
    i64 size;
    for (int i = 0; count < 256 && (size = file_list(i, name, sizeof(name))) != -1; i++) {
        strcpy(names[count], name);
        is_dir[count] = size == -2;
        count++;
    }

    size_t want = strlen(needle);
    for (int i = 0; i < count; i++) {
        if (is_dir[i]) continue;
        size_t len;
        char *data = read_all(names[i], &len);
        if (!data) continue;
        for (size_t at = 0; at + want <= len; at++) {
            if (memcmp(data + at, needle, want) == 0) {
                printf("%s%s\n", prefix, names[i]);
                hits++;
                break;
            }
        }
        free(data);
    }
    for (int i = 0; i < count; i++) {
        if (!is_dir[i]) continue;
        char child[256];
        int n = 0;
        for (const char *p = prefix; *p; p++) child[n++] = *p;
        for (const char *p = names[i]; *p; p++) child[n++] = *p;
        child[n++] = '/';
        child[n] = 0;
        if (chdir(names[i]) == 0) {
            search(child);
            chdir("..");
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 2) {
        printf("usage: find <text>\n");
        return 1;
    }
    needle = argv[1];
    search("");
    return hits ? 0 : 1;
}
