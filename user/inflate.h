/* inflate.h - decompressing "deflate" data: gzip and zlib streams, as
   used by compressed web pages and PNG images. */
#pragma once
#include "rdx.h"

/* Each returns 0 and a malloc'd result (*out, *out_len; NUL-terminated
   for convenience) or -1 if the data is damaged or not what it claims.
   Output is limited to `max_out` bytes (0: 64 MiB), against "zip bombs". */
int inflate_raw(const unsigned char *in, size_t len, unsigned char **out, size_t *out_len, size_t max_out);
int zlib_decompress(const unsigned char *in, size_t len, unsigned char **out, size_t *out_len, size_t max_out);
int gzip_decompress(const unsigned char *in, size_t len, unsigned char **out, size_t *out_len, size_t max_out);
