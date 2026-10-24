#include "test.h"

int zero = 0, one = 1;
int big = 2147483647, small = -2147483647 - 1;

int id(int x) { return x; }

void start() {
  int a = 7, b = -7, c = 2;

  ASSERT(9, a + c);
  ASSERT(5, a - c);
  ASSERT(-5, c - a);
  ASSERT(14, a * c);
  ASSERT(-7, -a);
  ASSERT(-8, ~a);
  ASSERT(6, ~b);
  ASSERT(7, +a);

  // Division rounds toward negative infinity, and the remainder takes
  // the sign of the divisor.
  ASSERT(3, a / c);
  ASSERT(-4, b / c);
  ASSERT(-4, a / -c);
  ASSERT(3, b / -c);
  ASSERT(1, a % c);
  ASSERT(1, b % c);
  ASSERT(-1, a % -c);
  ASSERT(-1, b % -c);
  ASSERT(63, (zero - 1) % 64);

  // 32-bit wraparound
  ASSERT(small, big + one);
  ASSERT(big, small - one);
  ASSERT(0, 65536 * id(65536));
  ASSERT(small, -small);

  // Forms with an immediate operand
  ASSERT(17, a + 10);
  ASSERT(-3, a - 10);
  ASSERT(-3, a + -10);
  ASSERT(17, a - -10);
  ASSERT(17, 10 + a);
  ASSERT(3, 10 - a);
  ASSERT(-7, 0 - a);
  ASSERT(70, a * 10);
  ASSERT(70, 10 * a);
  ASSERT(-7, a * -1);
  ASSERT(0, a * 0);
  ASSERT(7, a * 1);
  ASSERT(7, a + 0);
  ASSERT(-7, a / -1);
  ASSERT(7, a / 1);
  ASSERT(0, a / 10);
  ASSERT(-1, b / 10);
  ASSERT(3, b % 10);
  ASSERT(0, a % 1);
  ASSERT(1, 7 / a);
  ASSERT(2, 16 % a);

  // Constant folding uses the same rules as the VM.
  ASSERT(-4, -7 / 2);
  ASSERT(63, -1 % 64);
  ASSERT(small, 2147483647 + 1);
  ASSERT(15, 0xF0 >> 4);
  ASSERT(256, 1 << 8);
  ASSERT(3, 7 & 3);

  // Nested expressions and results stored back into an operand.
  ASSERT(67, (a + c) * (a - c) + id(a) * 3 + 1);
  ASSERT(10, ((((a)))) + 3);
  int x = 3;
  x = x * x + x;
  ASSERT(12, x);
  x = 5 - x;
  ASSERT(-7, x);
  x = a - x;
  ASSERT(14, x);
  x = x / (c + c);
  ASSERT(3, x);
  x = (x = 4) + x;
  ASSERT(8, x);
  int y = x = 9;
  ASSERT(9, y);
  ASSERT(9, x);
  ASSERT(20, (a, c, 20));
}
