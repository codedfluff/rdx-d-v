/* grep - print the lines that contain some text.
   Usage: grep [-i] [-v] [-n] [-c] <text> [file]
     -i ignore case   -v lines that do NOT match
     -n show line numbers   -c just count the matches */
#include "rdx.h"

static char lower(char c) { return c >= 'A' && c <= 'Z' ? (char)(c + 32) : c; }

static int matches(const char *line, const char *text, int ignore_case) {
    for (; *line; line++) {
        const char *a = line, *b = text;
        while (*b && (ignore_case ? lower(*a) == lower(*b) : *a == *b)) {
            a++;
            b++;
        }
        if (!*b) return 1;
    }
    return !*text;
}

int main(int argc, char **argv) {
    int ignore_case = 0, invert = 0, numbers = 0, count_only = 0;
    const char *text = NULL, *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1] && !text) {
            for (const char *f = argv[i] + 1; *f; f++) {
                if (*f == 'i') ignore_case = 1;
                else if (*f == 'v') invert = 1;
                else if (*f == 'n') numbers = 1;
                else if (*f == 'c') count_only = 1;
            }
        } else if (!text) {
            text = argv[i];
        } else {
            name = argv[i];
        }
    }
    if (!text) {
        printf("usage: grep [-i] [-v] [-n] [-c] <text> [file]\n");
        return 2;
    }
    int count, found = 0;
    char *data = read_all(name, NULL);
    if (!data) {
        printf("grep: %s: no such file\n", name);
        return 2;
    }
    char **lines = split_lines(data, &count);
    for (int i = 0; i < count; i++) {
        if (matches(lines[i], text, ignore_case) == invert) continue;
        found++;
        if (count_only) continue;
        if (numbers) printf("%d:", i + 1);
        puts(lines[i]);
    }
    if (count_only) printf("%d\n", found);
    return 0;
}
