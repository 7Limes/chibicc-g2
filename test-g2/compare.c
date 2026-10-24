#include "test.h"

int big = 2147483647, small = -2147483647 - 1;

int lt(int a, int b) { if (a < b) return 1; return 0; }
int le(int a, int b) { if (a <= b) return 1; return 0; }
int gt(int a, int b) { if (a > b) return 1; return 0; }
int ge(int a, int b) { if (a >= b) return 1; return 0; }
int eq(int a, int b) { if (a == b) return 1; return 0; }
int ne(int a, int b) { if (a != b) return 1; return 0; }
int nlt(int a, int b) { if (!(a < b)) return 1; return 0; }
int nle(int a, int b) { if (!(a <= b)) return 1; return 0; }
int neq(int a, int b) { if (!(a == b)) return 1; return 0; }
int truth(int x) { if (x) return 1; return 0; }
int falsity(int x) { if (!x) return 1; return 0; }

void start() {
  int a = 3, b = 5, n = -4, z = 0;

  // Comparisons as values
  ASSERT(1, a < b);
  ASSERT(0, b < a);
  ASSERT(0, a < a);
  ASSERT(1, a <= b);
  ASSERT(1, a <= a);
  ASSERT(0, b <= a);
  ASSERT(0, a > b);
  ASSERT(1, b > a);
  ASSERT(0, a > a);
  ASSERT(0, a >= b);
  ASSERT(1, a >= a);
  ASSERT(1, b >= a);
  ASSERT(1, a == a);
  ASSERT(0, a == b);
  ASSERT(0, a != a);
  ASSERT(1, a != b);
  ASSERT(1, n < z);
  ASSERT(1, small < big);
  ASSERT(0, big < small);
  ASSERT(1, big != small);
  ASSERT(0, big == small);
  ASSERT(1, a < 4);
  ASSERT(0, a < 3);
  ASSERT(1, 2 < a);
  ASSERT(1, a == 3);
  ASSERT(1, -1 == n + a);

  // Logical operators
  ASSERT(0, !a);
  ASSERT(1, !z);
  ASSERT(1, !!a);
  ASSERT(1, a && b);
  ASSERT(0, a && z);
  ASSERT(0, z && a);
  ASSERT(1, a || z);
  ASSERT(1, z || a);
  ASSERT(0, z || z);
  ASSERT(1, a < b && b < 10 && n < 0);
  ASSERT(1, a > b || z || n == -4);
  ASSERT(0, a > b || (z && 1));
  ASSERT(5, a ? b : n);
  ASSERT(-4, z ? b : n);
  ASSERT(8, (a < b ? a : b) + b);
  ASSERT(1, (_Bool)n);
  ASSERT(0, (_Bool)z);

  // Comparisons as branch conditions
  ASSERT(1, lt(a, b));
  ASSERT(0, lt(b, a));
  ASSERT(0, lt(a, a));
  ASSERT(1, le(a, a));
  ASSERT(0, le(b, a));
  ASSERT(1, gt(b, a));
  ASSERT(0, gt(a, a));
  ASSERT(1, ge(a, a));
  ASSERT(0, ge(a, b));
  ASSERT(1, eq(a, a));
  ASSERT(0, eq(a, b));
  ASSERT(1, ne(a, b));
  ASSERT(0, ne(b, b));
  ASSERT(0, nlt(a, b));
  ASSERT(1, nlt(b, a));
  ASSERT(1, nlt(a, a));
  ASSERT(0, nle(a, a));
  ASSERT(1, nle(b, a));
  ASSERT(1, neq(a, b));
  ASSERT(0, neq(a, a));
  ASSERT(1, truth(n));
  ASSERT(0, truth(z));
  ASSERT(1, falsity(z));
  ASSERT(0, falsity(a));
  ASSERT(1, lt(small, big));
  ASSERT(1, gt(big, small));
}
