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
void color(int r, int g, int b) __attribute__((g2_intrinsic("col")));
void pix(int x, int y) __attribute__((g2_intrinsic("pix")));

// Input state, updated by the VM before every frame. Keys are 1 while
// pressed and 0 otherwise. DELTA is the time since the last frame in ms.
extern const int CONTROL1 __attribute__((address(3)));
extern const int CONTROL2 __attribute__((address(4)));
extern const int A __attribute__((address(5)));
extern const int B __attribute__((address(6)));
extern const int UP __attribute__((address(7)));
extern const int DOWN __attribute__((address(8)));
extern const int LEFT __attribute__((address(9)));
extern const int RIGHT __attribute__((address(10)));
extern const int DELTA __attribute__((address(11)));

#endif
