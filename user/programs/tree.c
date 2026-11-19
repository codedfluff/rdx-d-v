/* tree - print the directory tree below the current (or given) directory.
   Usage: tree [directory] */
#include "rdx.h"

static int files_seen, dirs_seen;

static void walk(const char *prefix) {
    /* Collect this directory's entries before descending (chdir would
       otherwise disturb a running file_list scan). */
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

    for (int i = 0; i < count; i++) {
        int last = i == count - 1;
        printf("%s%s%s%s\n", prefix, last ? "`-- " : "|-- ", names[i], is_dir[i] ? "/" : "");
        if (is_dir[i]) {
            dirs_seen++;
            if (chdir(names[i]) == 0) {
                char child[256];
                int n = 0;
                for (const char *p = prefix; *p; p++) child[n++] = *p;
                for (const char *p = last ? "    " : "|   "; *p; p++) child[n++] = *p;
                child[n] = 0;
                walk(child);
                chdir("..");
            }
        } else {
            files_seen++;
        }
    }
}

int main(int argc, char **argv) {
    if (argc > 1 && chdir(argv[1]) != 0) {
        printf("tree: %s: no such directory\n", argv[1]);
        return 1;
    }
    printf("%s\n", argc > 1 ? argv[1] : ".");
    walk("");
    printf("\n%d director%s, %d file%s\n", dirs_seen, dirs_seen == 1 ? "y" : "ies", files_seen,
           files_seen == 1 ? "" : "s");
    return 0;
}
