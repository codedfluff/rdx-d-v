/* wc - count lines, words and bytes of a file or of standard input.
   -l, -w or -c prints just that one number. */
#include "rdx.h"

int main(int argc, char **argv) {
    char only = 0;
    const char *name = NULL;
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-' && argv[i][1]) only = argv[i][1];
        else name = argv[i];
    }
    size_t len;
    char *data = read_all(name, &len);
    if (!data) {
        printf("wc: %s: no such file\n", name);
        return 1;
    }
    i64 lines = 0, words = 0;
    int in_word = 0;
    for (size_t i = 0; i < len; i++) {
        char c = data[i];
        if (c == '\n') lines++;
        int space = c == ' ' || c == '\n' || c == '\t' || c == '\r';
        if (!space && !in_word) words++;
        in_word = !space;
    }
    if (only == 'l') printf("%lld\n", lines);
    else if (only == 'w') printf("%lld\n", words);
    else if (only == 'c') printf("%lld\n", (i64)len);
    else printf("%lld lines, %lld words, %lld bytes\n", lines, words, (i64)len);
    return 0;
}
