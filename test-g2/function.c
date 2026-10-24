#include "test.h"

int calls;

int add2(int a, int b) { return a + b; }
int sub3(int a, int b, int c) { return a - b - c; }
int add6(int a, int b, int c, int d, int e, int f) { return a + b + c + d + e + f; }
int counted(int x) { calls = calls + 1; return x; }
void bump() { calls = calls + 10; }
int ret_void_cast() { bump(); return calls; }
int f(int a, int b) { return a * 10 + b; }

int d1(int x) { return x + 1; }
int d2(int x) { return d1(x) + 1; }
int d3(int x) { return d2(x) + 1; }
int d4(int x) { return d3(x) + 1; }
int d5(int x) { return d4(x) + 1; }
int d6(int x) { return d5(x) + 1; }
int d7(int x) { return d6(x) + 1; }
int d8(int x) { return d7(x) + 1; }
int d9(int x) { return d8(x) + 1; }
int d10(int x) { return d9(x) + 1; }

int early(int x) {
  if (x > 0)
    return 1;
  bump();
  return 2;
}

void start() {
  ASSERT(3, add2(1, 2));
  ASSERT(-4, sub3(1, 2, 3));
  ASSERT(21, add6(1, 2, 3, 4, 5, 6));
  ASSERT(10, add2(add2(1, 2), add2(3, 4)));

  // Evaluating an argument calls the same function again, which must
  // not clobber the arguments already computed.
  ASSERT(30, f(2, f(1, 0)));
  ASSERT(12, f(f(0, 1), 2));
  ASSERT(-6, sub3(add2(1, 1), sub3(10, 1, 1), add2(0, 0)));

  ASSERT(11, counted(5) + counted(6));
  ASSERT(2, calls);
  bump();
  ASSERT(12, calls);
  ASSERT(22, ret_void_cast());
  ASSERT(1, early(5));
  ASSERT(2, early(-5));
  ASSERT(32, calls);

  ASSERT(10, d10(0));
  int x = d10(5);
  ASSERT(15, x);
  if (add2(x, 1) == 16)
    x = 0;
  ASSERT(0, x);
}
