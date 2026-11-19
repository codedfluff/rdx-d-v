/* write - create or replace a file with one line of text.
   Usage: write <file> <text...> */
#include "rdx.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: write <file> <text>\n");
        return 1;
    }
    /* The text is everything after the file name, spacing and all. */
    const char *text = command_tail() + strlen(argv[1]);
    while (*text == ' ') text++;
    size_t len = strlen(text);
    char *data = malloc(len + 2);
    if (!data) return 1;
    memcpy(data, text, len);
    data[len] = '\n';
    if (file_write(argv[1], data, len + 1) != 0) {
        printf("write: %s: %s\n", argv[1], last_error());
        return 1;
    }
    return 0;
}
