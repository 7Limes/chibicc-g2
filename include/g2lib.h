#ifndef __G2LIB_H
#define __G2LIB_H

// The g2 standard library, translated from the Dry standard library.
// g2.h includes it unless G2_NO_STDLIB is defined.
//
// Every function is static, so functions that a program doesn't call
// are not emitted. Strings are length-prefixed: s[0] is the length and
// s[1] .. s[s[0]] are the characters, the same layout that the
// assembler's `string` load type produces.

//
// Math
//

static int min(int a, int b) {
  return a < b ? a : b;
}

static int max(int a, int b) {
  return a > b ? a : b;
}

// A multiplicative linear congruential generator. Seed it with a
// non-zero value; a seed of 0 always produces 0.
#define __G2_RAND_MODULUS 2147483647
#define __G2_RAND_MULTIPLIER 1583458089

static int __g2_rand_value;

static void seed_rand(int seed) {
  __g2_rand_value = seed;
}

// Returns the next random number, in [0, 2147483646].
static int next_rand(void) {
  __g2_rand_value = __g2_rand_value * __G2_RAND_MULTIPLIER % __G2_RAND_MODULUS;
  return __g2_rand_value;
}

//
// Color
//

// Sets the current color from a 0xRRGGBB value.
static void hexcolor(int c) {
  int b = c % 256;
  c /= 256;
  int g = c % 256;
  color(c / 256, g, b);
}

//
// Strings
//

// Writes the decimal representation of x to dest as a length-prefixed
// string. dest needs room for 12 slots.
static void int_to_str(int *dest, int x) {
  int digits = x == 0;
  int negative = x < 0;
  if (negative)
    x = -x;
  for (int t = x; t > 0; t /= 10)
    digits++;

  if (negative) {
    dest[0] = digits + 1;
    dest++;
    dest[0] = '-';
  } else {
    dest[0] = digits;
  }

  for (int *p = dest + digits; p > dest; p--) {
    *p = x % 10 + '0';
    x /= 10;
  }
}

//
// Graphics
//

// Fills a width x height rectangle whose top-left corner is (x, y).
static void rect(int x, int y, int width, int height) {
  for (int py = y; py < y + height; py++)
    for (int px = x; px < x + width; px++)
      pix(px, py);
}

// Draws a line from (x0, y0) to (x1, y1) with Bresenham's algorithm.
static void line(int x0, int y0, int x1, int y1) {
  int dx = x1 > x0 ? x1 - x0 : x0 - x1;
  int dy = y1 > y0 ? y0 - y1 : y1 - y0;
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx + dy;
  int x = x0, y = y0;

  for (;;) {
    pix(x, y);
    if (x == x1 && y == y1)
      break;

    int e2 = err * 2;
    if (e2 >= dy) {
      if (x == x1)
        break;
      err += dy;
      x += sx;
    }
    if (e2 <= dx) {
      if (y == y1)
        break;
      err += dx;
      y += sy;
    }
  }
}

// The first character in a font. Glyphs start at the space character.
#define __G2_FONT_START_CHAR 32

// Draws a length-prefixed string with its top-left corner at (x, y).
//
// A font is a sequence of size x size pixel glyphs, packed 32 pixels per
// slot (as loaded with G2_LOAD(name, pack, file, ...)). Within each slot,
// the lowest bit comes first. Pixels fill a row from right to left, and
// rows go from top to bottom. Characters are separated by one pixel.
static void draw_text(const int *str, const int *font, int size, int x, int y) {
  int glyph_slots = size * size / 32;
  const int *end = str + 1 + str[0];

  for (const int *p = str + 1; p < end; p++) {
    int gx = size - 1;
    int gy = 0;
    const int *glyph = font + (*p - __G2_FONT_START_CHAR) * glyph_slots;

    for (const int *g = glyph; g < glyph + glyph_slots; g++) {
      int bits = *g;
      for (int i = 0; i < 32; i++) {
        if (bits % 2)
          pix(x + gx, y + gy);
        gx--;
        if (gx < 0)
          gy++;
        gx %= size;
        bits /= 2;
      }
    }

    x += size + 1;
  }
}

#endif
