#include "test.h"

struct P { int x, y; };
struct Big { int v[12]; int tag; };
union U { int i; struct P p; };

struct P gp = {1, 2};
struct Big gbig;

void copy_p(struct P *dst, struct P *src) { *dst = *src; }
void copy_big(struct Big *dst, struct Big *src) { *dst = *src; }
int sum_big(struct Big *b) {
  int s = b->tag;
  for (int i = 0; i < 12; i++)
    s += b->v[i];
  return s;
}

void start() {
  // Direct to direct
  struct P a = {3, 4};
  struct P b;
  b = a;
  ASSERT(3, b.x);
  ASSERT(4, b.y);
  gp = a;
  ASSERT(3, gp.x);

  // Initialized from another struct
  struct P c = gp;
  ASSERT(4, c.y);

  // Through pointers, small and large
  struct P d = {0, 0};
  copy_p(&d, &a);
  ASSERT(3, d.x);
  ASSERT(4, d.y);

  struct Big big;
  for (int i = 0; i < 12; i++)
    big.v[i] = i + 1;
  big.tag = 100;
  copy_big(&gbig, &big);
  ASSERT(178, sum_big(&gbig));
  struct Big big2 = gbig;
  ASSERT(178, sum_big(&big2));

  // Chained, array elements, members
  struct P arr[3];
  arr[0] = arr[1] = a;
  ASSERT(4, arr[0].y);
  ASSERT(3, arr[1].x);
  int i = 2;
  arr[i] = arr[0];
  ASSERT(4, arr[2].y);
  struct { struct P inner; int z; } outer;
  outer.inner = a;
  outer.z = 9;
  ASSERT(4, outer.inner.y);

  union U u1, u2;
  u1.p = a;
  u2 = u1;
  ASSERT(3, u2.i);
  ASSERT(4, u2.p.y);

  a = a;
  ASSERT(3, a.x);

  // Partially initialized aggregates are still zero-filled.
  int z[5] = {7, 8};
  ASSERT(0, z[2] + z[3] + z[4]);
  ASSERT(15, z[0] + z[1]);
  struct Big zb = {{1}, 5};
  ASSERT(6, sum_big(&zb));
  int full[3] = {1, 2, 3};
  ASSERT(6, full[0] + full[1] + full[2]);
}
