#ifndef __G2RT_H
#define __G2RT_H

// Runtime helpers for the g2 backend. The compiler includes this header
// in every translation unit. It calls these functions for bitwise
// operators that it can't compile to a few multiplications, divisions or
// remainders (e.g. `x & 7` is `x % 8`, but `x & y` calls __g2_and). Only
// the helpers a program needs are emitted.
//
// g2 has no bitwise instructions, so the helpers go bit by bit. On g2,
// x % 2 is the lowest bit of x (0 or 1, even for negative x) and x / 2
// is an arithmetic shift right, because division rounds toward negative
// infinity. `bit` is 2^i, which wraps around to INT_MIN for i == 31.
//
// Once the remaining bits of one operand are all 0 or all 1, the rest of
// the result is known, so the loops usually stop early. If the remaining
// bits of `b` are all that's left, they are `b * bit`, which is `b << i`.

static int __g2_and(int a, int b) {
  int r = 0;
  int bit = 1;
  for (int i = 0; i < 32; i++) {
    if (a == 0 || b == 0)
      return r;
    if (a == -1)
      return r + b * bit;
    if (b == -1)
      return r + a * bit;
    if (a % 2 && b % 2)
      r += bit;
    a /= 2;
    b /= 2;
    bit *= 2;
  }
  return r;
}

static int __g2_or(int a, int b) {
  int r = 0;
  int bit = 1;
  for (int i = 0; i < 32; i++) {
    if (a == 0)
      return r + b * bit;
    if (b == 0)
      return r + a * bit;
    if (a == -1 || b == -1)
      return r - bit;
    if (a % 2 || b % 2)
      r += bit;
    a /= 2;
    b /= 2;
    bit *= 2;
  }
  return r;
}

static int __g2_xor(int a, int b) {
  int r = 0;
  int bit = 1;
  for (int i = 0; i < 32; i++) {
    if (a == 0)
      return r + b * bit;
    if (b == 0)
      return r + a * bit;
    if (a % 2 != b % 2)
      r += bit;
    a /= 2;
    b /= 2;
    bit *= 2;
  }
  return r;
}

// Shifts by a count that isn't a constant. The count must be 0..31.
static int __g2_shl(int a, int n) {
  for (; n > 0; n--)
    a *= 2;
  return a;
}

static int __g2_shr(int a, int n) {
  for (; n > 0; n--)
    a /= 2;
  return a;
}

#endif
