#include "test.h"

int g = 10;
int *gp = &g;
int ga[4] = {1, 2, 3, 4};
int *gap = ga + 2;

void set(int *p, int v) { *p = v; }
int get(int *p) { return *p; }
void swap(int *a, int *b) { int t = *a; *a = *b; *b = t; }
int *pick(int *a, int *b, int first) { return first ? a : b; }
void divmod(int a, int b, int *q, int *r) { *q = a / b; *r = a % b; }

void start() {
  int x = 3, y = 4;
  int *p = &x;
  ASSERT(3, *p);
  *p = 5;
  ASSERT(5, x);
  set(&y, 9);
  ASSERT(9, y);
  ASSERT(9, get(&y));
  swap(&x, &y);
  ASSERT(9, x);
  ASSERT(5, y);
  *pick(&x, &y, 0) = 1;
  ASSERT(1, y);
  *pick(&x, &y, 1) = 2;
  ASSERT(2, x);

  int q, r;
  divmod(-7, 2, &q, &r);
  ASSERT(-4, q);
  ASSERT(1, r);

  ASSERT(10, *gp);
  *gp = 11;
  ASSERT(11, g);
  ASSERT(3, *gap);
  ASSERT(4, gap[1]);
  ASSERT(2, gap[-1]);

  int **pp = &p;
  **pp = 7;
  ASSERT(7, x);

  // Pointer arithmetic and comparison
  int *a = ga, *b = ga + 3;
  ASSERT(3, b - a);
  ASSERT(-3, a - b);
  ASSERT(1, a < b);
  ASSERT(1, a + 3 == b);
  ASSERT(1, a != b);
  ASSERT(4, *b);
  b = b - 1;
  ASSERT(3, *b);
  ASSERT(0, p == 0);
  p = 0;
  ASSERT(1, !p);

  // An absolute address. Slot 1 always holds 1.
  ASSERT(1, *(int *)1);
}
