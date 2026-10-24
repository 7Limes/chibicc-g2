#ifndef __G2_H
#define __G2_H

// Default window size. A program sets its own size by defining WIDTH and
// HEIGHT *before* including this header, or with -DWIDTH=N -DHEIGHT=N.
// The final values become the #width and #height of the program.
#ifndef WIDTH
#define WIDTH 100
#endif
#ifndef HEIGHT
#define HEIGHT 100
#endif

// Drawing. Each call compiles to a single instruction.

// g2: Set the current color to (`r`, `g`, `b`)
void color(int r, int g, int b) __attribute__((g2_intrinsic("col")));

// g2: Draw a pixel at (`x`, `y`)
void pix(int x, int y) __attribute__((g2_intrinsic("pix")));

// Input state, updated by the VM before every frame. Keys are 1 while
// pressed and 0 otherwise, so they are declared _Bool, which makes tests
// like `if (UP)` a single jump. DELTA is the time since the last frame
// in ms.
extern const _Bool CONTROL1 __attribute__((address(3)));
extern const _Bool CONTROL2 __attribute__((address(4)));
extern const _Bool A __attribute__((address(5)));
extern const _Bool B __attribute__((address(6)));
extern const _Bool UP __attribute__((address(7)));
extern const _Bool DOWN __attribute__((address(8)));
extern const _Bool LEFT __attribute__((address(9)));
extern const _Bool RIGHT __attribute__((address(10)));
extern const int DELTA __attribute__((address(11)));

// Data loaded into memory by the assembler. For example,
//
//   G2_LOAD(FONT, pack, file, "font.bin");
//   G2_LOAD(GREETING, raw, string, "Hello");
//
// declares FONT and GREETING as const int arrays. The operation is raw
// (one slot per byte) or pack (four bytes per slot, little-endian).
// A string is preceded by its length. Relative file paths are relative
// to the source file. Other tools see a plain array declaration.
#ifdef __g2__
#define G2_LOAD(name, op, type, data) \
  extern const int name[] __attribute__((g2_load(#op, #type, data)))
#else
#define G2_LOAD(name, op, type, data) extern const int name[]
#endif

// The standard library: min, max, seed_rand, next_rand, hexcolor,
// int_to_str, rect, line and draw_text. Define G2_NO_STDLIB before
// including this header to leave it out, e.g. to define functions with
// the same names.
#ifndef G2_NO_STDLIB
#include <g2lib.h>
#endif

#endif
