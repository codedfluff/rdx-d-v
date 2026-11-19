/* hexdump - show the bytes of a file (or standard input) in hexadecimal. */
#include "rdx.h"

int main(int argc, char **argv) {
    size_t len;
    unsigned char *data = (unsigned char *)read_all(argc > 1 ? argv[1] : NULL, &len);
    if (!data) {
        printf("hexdump: %s: no such file\n", argv[1]);
        return 1;
    }
    for (size_t row = 0; row < len; row += 16) {
        printf("%08x  ", (unsigned)row);
        for (size_t i = row; i < row + 16; i++) {
            if (i < len) printf("%02x ", data[i]);
            else printf("   ");
            if (i == row + 7) putchar(' ');
        }
        printf(" |");
        for (size_t i = row; i < row + 16 && i < len; i++) putchar(data[i] >= 32 && data[i] < 127 ? data[i] : '.');
        printf("|\n");
    }
    return 0;
}
