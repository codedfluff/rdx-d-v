/* cat - print files one after another, or standard input if none given. */
#include "rdx.h"

int main(int argc, char **argv) {
    int status = 0;
    for (int i = 1; i < argc || i == 1; i++) {
        const char *name = argc > 1 ? argv[i] : NULL;
        size_t len;
        char *data = read_all(name, &len);
        if (!data) {
            printf("cat: %s: no such file\n", name);
            status = 1;
            continue;
        }
        flush();
        fd_write(1, data, len);
        free(data);
    }
    return status;
}
