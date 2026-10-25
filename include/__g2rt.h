#ifndef __G2RT_H
#define __G2RT_H

// Runtime helpers for the g2 backend. The compiler includes this header
// in every translation unit. It calls these functions for bitwise
// operators that it can't compile to a few multiplications, divisions or
// remainders (e.g. `x & 7` is `x % 8`, but `x & y` calls __g2_and), and
// for floating-point multiplication and division (see the end of this
// file). Only the helpers a program needs are emitted.
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

// Q16.16 fixed point. A float x is stored as round(x * 65536). The
// compiler calls these for * and / between floats when neither side is
// a constant or a converted integer. The results are exactly those of
// the compiler's constant folding, rounded toward negative infinity and
// wrapped to 32 bits.
//
// __G2_FBITS(v) is the raw value of the float lvalue v, as an int lvalue.
// __G2_RAW(n) is the float whose raw value is the integer constant n: an
// integer times a float uses the integer itself, so this folds to n.
#define __G2_FBITS(v) (*(int *)&(v))
#define __G2_RAW(n) ((n) * 0x1p-16f)

// floor(a * b / 65536). The product needs 64 bits, so both operands are
// split into a signed high half and a low half in [0, 65536) (exact,
// because division floors):
//
//   a * b / 65536 == ah * b + al * bh + al * bl / 65536
//
// The low halves' product can reach 2^32, so `al` is split once more into
// bytes. Every term is exact modulo 2^32, so the sum is too.
static int __g2_fmul(int a, int b) {
  int ah = a / 65536;
  int al = a % 65536;
  int bh = b / 65536;
  int bl = b % 65536;
  int lo1 = al / 256 * bl;          // < 2^24
  int lo0 = al % 256 * bl;          // < 2^24
  int lo = (lo1 + lo0 / 256) / 256; // floor(al * bl / 65536)
  return ah * b + al * bh + lo;
}

// floor(a * 65536 / b). Starts from the integer quotient and remainder
// (r has the sign of b, and |r| < |b|) and then produces the 16 fraction
// bits. Once r * 2^k fits in 32 bits, the k bits that are left come from
// one division. Before that, they come one at a time, comparing r with
// b - r instead of computing 2r, which could overflow.
static int __g2_fdiv(int a, int b) {
  int q = a / b;
  int r = a % b;
  int scale = 65536; // 2^k, where k is the number of bits still to produce
  int limit = 32768; // 2^(31 - k): below this, r * scale fits in 32 bits
  while (scale > 1) {
    if (r > -limit && r < limit)
      return q * scale + r * scale / b;
    int d = b - r;
    q *= 2;
    if (b > 0 ? r >= d : r <= d) {
      r -= d; // 2r - b
      q++;
    } else {
      r += r;
    }
    scale /= 2;
    limit *= 2; // wraps on the last step, but is no longer read
  }
  return q;
}

#endif
