/* gunzip - decompress a .gz (gzip) or zlib file.
   Usage: gunzip <file> [output]   without an output file, prints it */
#include "inflate.h"

int main(int argc, char **argv) {
    if (argc < 2) {
        printf("usage: gunzip <file.gz> [output file]\n");
        return 1;
    }
    size_t len;
    char *data = read_all(argv[1], &len);
    if (!data) {
        printf("gunzip: %s: no such file\n", argv[1]);
        return 1;
    }
    unsigned char *out;
    size_t out_len;
    const unsigned char *in = (const unsigned char *)data;
    int failed = len >= 2 && in[0] == 0x1f && in[1] == 0x8b ? gzip_decompress(in, len, &out, &out_len, 0)
                                                            : zlib_decompress(in, len, &out, &out_len, 0);
    if (failed) {
        printf("gunzip: %s: not gzip or zlib data, or damaged\n", argv[1]);
        return 1;
    }
    if (argc > 2) {
        if (file_write(argv[2], out, out_len) != 0) {
            printf("gunzip: %s: %s\n", argv[2], last_error());
            return 1;
        }
        printf("%s: %d bytes\n", argv[2], (int)out_len);
    } else {
        flush();
        fd_write(1, out, out_len);
    }
    return 0;
}
