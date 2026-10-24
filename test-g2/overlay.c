#include "test.h"

// Frames of functions that are never active at the same time share
// memory, and so do locals of sibling scopes. These tests check that
// nothing live is overwritten.

int leaf(int x) { int t = x * 2; return t + 1; }
int other_leaf(int x) { int u = x + 100; int v = u * 3; return v - u; }
int both(int a, int b) { return a * 1000 + b; }
int mid(int x) { int keep = x + 5; int r = leaf(x); return keep * 100 + r; }

void write_through(int *p, int v) { int local = v; *p = local; }
int read_through(int *p) { return leaf(*p); }
int deep_helper(int *p, int n);
int deep(int *p, int n) {
  int here = n;
  if (n > 0)
    return deep_helper(p, n) + here;
  return *p + here;
}

int sum_array(int *a, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += leaf(a[i]);
  return s;
}

void start() {
  // Sibling calls as arguments: leaf and other_leaf share memory.
  ASSERT(7202, both(leaf(3), other_leaf(1)));
  ASSERT(205, both(0, other_leaf(1)) + leaf(1));

  // A value kept across a call to a function above it.
  ASSERT(807, mid(3));

  // Pointers to the caller's locals stay valid in callees.
  int x = 0;
  write_through(&x, 42);
  ASSERT(42, x);
  ASSERT(85, read_through(&x));
  ASSERT(106, deep(&x, 3));

  int arr[3] = {1, 2, 3};
  ASSERT(15, sum_array(arr, 3));
  ASSERT(1, arr[0]);

  // Locals in sibling and nested scopes
  int a = 1;
  {
    int b = 2;
    ASSERT(3, a + b);
  }
  {
    int c = 30;
    {
      int d = 400;
      ASSERT(431, a + c + d);
    }
    int e = 5;
    ASSERT(36, a + c + e);
  }
  int total = 0;
  for (int i = 0; i < 3; i++) {
    int sq = i * i;
    total += sq;
  }
  for (int j = 0; j < 3; j++) {
    int cube = j * j * j;
    total += cube;
  }
  ASSERT(14, total);
  ASSERT(1, a);

  int *p;
  {
    int kept = 77;
    p = &kept;
    ASSERT(77, *p);
  }
}

int deep_helper(int *p, int n) {
  int mine = n * 10;
  return leaf(mine) + (n == 3 ? *p : 0);
}
