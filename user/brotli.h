/* brotli.h - reading Brotli-compressed data (RFC 7932), as WOFF2 fonts and
   many web servers use it. */
#pragma once
#include "rdx.h"

/* Decompress all of `in` into a new malloc'd buffer (*out, *out_len),
   at most max_out bytes. Returns 0, or -1 if the data is damaged, too big,
   or the dictionary file (/brotli-dict.bin) is missing when needed. */
int brotli_decompress(const unsigned char *in, size_t len, unsigned char **out, size_t *out_len, size_t max_out);

/* Where the 122784-byte static dictionary comes from (default: read from
   /brotli-dict.bin the first time a stream uses it). */
void brotli_set_dictionary(const unsigned char *dict);
