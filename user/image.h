/* image.h - reading pictures: PNG, JPEG (baseline and progressive) and
   GIF (the first frame). */
#pragma once
#include "rdx.h"

typedef struct {
    int width, height;
    unsigned *pixels; /* 0xAARRGGBB, row after row; AA 255 = opaque, 0 = transparent */
} image;

/* Decode a picture file held in memory. Returns 0, or -1 with the reason
   in `error` (at least 96 bytes). Pictures are limited to 16 million
   pixels. */
int image_decode(const unsigned char *data, size_t len, image *out, char *error);
void image_free(image *img);
/* Draw it scaled to w x h at (x, y), letting the background show through
   transparent parts. Clipped to the canvas. */
void image_draw(canvas *c, const image *img, int x, int y, int w, int h);
/* Is this a picture file (by its first bytes)? */
int image_sniff(const unsigned char *data, size_t len);
