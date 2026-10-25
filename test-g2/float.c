#include "test.h"

// Floats are Q16.16 fixed point: fbits(x) == round(x * 65536).

float g = 1.5;
float gneg = -2.25;
int gi = 2.7;
int gineg = -2.7;
float garr[] = {1, 0.5, -0.25};
_Bool gb = 0.25;

struct point {
  float x, y;
};

float fid(float x) { return x; }
int id(int x) { return x; }
float int_to_float(int x) { return x; }   // converts on return
float add_int(float x, int n) { return x + n; }

void scale_point(struct point *p, float dx) {
  p->x += dx;
  p->y -= dx;
}

void start() {
  ASSERT(1, sizeof(float));
  ASSERT(1, sizeof(double));
  ASSERT(1, sizeof(long double));

  // Literals
  ASSERT(65536, fbits(1.0));
  ASSERT(98304, fbits(1.5f));
  ASSERT(16384, fbits(.25));
  ASSERT(6554, fbits(0.1));
  ASSERT(19660800, fbits(3e2));
  ASSERT(4096, fbits(0x1p-4));
  ASSERT(65536, fbits(1.0L));
  ASSERT(2147483647, fbits(32767.99998474121));
  ASSERT(1, fbits(0x1p-16));
  ASSERT(0, fbits(0.0));

  // Types
  ASSERT(1, _Generic(1.0, double: 1, default: 0));
  ASSERT(1, _Generic(1.0f, float: 1, default: 0));
  ASSERT(1, _Generic(1.0f + 1.0, double: 1, default: 0));
  ASSERT(1, _Generic(1.0f + 1, float: 1, default: 0));
  ASSERT(1, _Generic(1 + 2, int: 1, default: 0));

  // int -> float, folded and at runtime
  ASSERT(196608, fbits(3));
  ASSERT(196608, fbits(id(3)));
  ASSERT(-196608, fbits(id(-3)));
  ASSERT(-2147483647 - 1, fbits(id(-32768)));

  // float -> int rounds toward negative infinity
  ASSERT(2, (int)2.7);
  ASSERT(-3, (int)-2.7);
  ASSERT(-1, (int)-0.5);
  ASSERT(0, (int)0.5);
  ASSERT(2, (int)fid(2.7));
  ASSERT(-3, (int)fid(-2.7));
  ASSERT(-1, (int)fid(-0.5));
  ASSERT(-2, (int)fid(-2.0));
  ASSERT(5, id(fid(5.99)));

  // Conversions between floating types are no-ops.
  double d = 1.25f;
  ASSERT(81920, fbits(d));

  // To _Bool
  ASSERT(1, (_Bool)0.25);
  ASSERT(1, (_Bool)fid(0x1p-16));
  ASSERT(0, (_Bool)fid(0.0));
  ASSERT(1, gb);

  // + - and negation work on the raw values.
  float a = 1.5, b = -0.25;
  ASSERT(81920, fbits(a + b));
  ASSERT(114688, fbits(a - b));
  ASSERT(-98304, fbits(-a));
  ASSERT(16384, fbits(-b));
  ASSERT(163840, fbits(a + 1));
  ASSERT(-32768, fbits(1 - a));
  ASSERT(229376, fbits(add_int(a, 2)));

  // Comparisons
  ASSERT(1, a > b);
  ASSERT(0, a < b);
  ASSERT(1, b < 0);
  ASSERT(1, a >= 1.5);
  ASSERT(1, a <= 1.5);
  ASSERT(0, a == 1);
  ASSERT(1, a != 1);
  ASSERT(1, fid(2.0) == 2);
  ASSERT(1, fid(-0.5) < fid(-0.25));
  ASSERT(0, !a);
  ASSERT(1, !fid(0.0));
  ASSERT(1, a && b);
  ASSERT(1, fid(0.0) || b);

  int taken = 0;
  if (fid(0.5))
    taken = 1;
  ASSERT(1, taken);
  taken = 0;
  if (fid(-0.5) < -0.25)
    taken = 1;
  ASSERT(1, taken);

  int n = 0;
  for (float x = 0; x < 2; x += 0.25)
    n++;
  ASSERT(8, n);

  ASSERT(98304, fbits(id(1) ? 1.5 : 2));
  ASSERT(131072, fbits(id(0) ? 1.5 : 2));

  // Increment, decrement and compound assignment
  float f = 1.5;
  f++;
  ASSERT(163840, fbits(f));
  --f;
  ASSERT(98304, fbits(f));
  ASSERT(98304, fbits(f--));
  ASSERT(32768, fbits(f));
  f += 2;
  ASSERT(163840, fbits(f));
  f -= 0.75;
  ASSERT(114688, fbits(f));

  int i = 3;
  i += 0.5;      // i = (int)(3.0 + 0.5)
  ASSERT(3, i);
  i -= 0.5;      // i = (int)(3.0 - 0.5)
  ASSERT(2, i);

  // Arrays, structs and pointers
  float arr[3] = {1, 2.5, -1};
  ASSERT(163840, fbits(arr[1]));
  arr[id(2)] += 0.5;
  ASSERT(-32768, fbits(arr[2]));
  float *p = arr;
  *p = 4.5;
  ASSERT(294912, fbits(arr[0]));
  ASSERT(163840, fbits(p[1]));
  p[1]++;
  ASSERT(229376, fbits(arr[1]));

  struct point pt = {1.5, 2};
  scale_point(&pt, 0.5);
  ASSERT(131072, fbits(pt.x));
  ASSERT(98304, fbits(pt.y));

  // Global initializers
  ASSERT(98304, fbits(g));
  ASSERT(-147456, fbits(gneg));
  ASSERT(2, gi);
  ASSERT(-3, gineg);
  ASSERT(65536, fbits(garr[0]));
  ASSERT(32768, fbits(garr[1]));
  ASSERT(-16384, fbits(garr[2]));

  // Arguments and return values convert.
  ASSERT(458752, fbits(int_to_float(7)));
  ASSERT(-1, id(fid(-0.75)));

  // * and / on constants fold with the runtime's rounding: floor(a * b / 65536)
  // and floor(a * 65536 / b).
  ASSERT(245760, fbits(1.5 * 2.5));
  ASSERT(21845, fbits(1.0 / 3));
  ASSERT(-21846, fbits(-1.0 / 3));
  ASSERT(-1, fbits(-0x1p-16 * 0.5));
  ASSERT(0, fbits(0x1p-16 * 0.5));
  ASSERT(6, (int)(2.5 * 2.5));
  ASSERT(4, sizeof(int[(int)(1.5 * 3)]));

  // * and / at run time (test-g2/fixed.c checks them exhaustively)
  float m = fid(1.5);
  ASSERT(245760, fbits(m * fid(2.5)));
  ASSERT(-21846, fbits(fid(-1.0) / 3));
  ASSERT(21845, fbits(fid(1.0) / fid(3.0)));
  ASSERT(147456, fbits(m * id(3) / 2));
  m *= 2;
  ASSERT(196608, fbits(m));
  m /= fid(4);
  ASSERT(49152, fbits(m));
  m *= m;
  ASSERT(36864, fbits(m));
  m = fid(10);
  m /= 0.5;
  ASSERT(1310720, fbits(m));
  m *= 1.3;    // raw 85197
  ASSERT(1703940, fbits(m));
  arr[id(1)] *= fid(-0.5);
  ASSERT(-114688, fbits(arr[1]));
  pt.y /= pt.x;
  ASSERT(49152, fbits(pt.y));

  int k = 7;
  k *= 1.5;      // k = (int)(7.0 * 1.5)
  ASSERT(10, k);
  k /= 0.25;
  ASSERT(40, k);

  // Helper calls inside the arguments of another call
  // 3.75 + (int)1.125
  ASSERT(311296, fbits(add_int(fid(1.5) * fid(2.5), id(3) * 1.5 / fid(4))));

  // With an integer operand, the integer itself is used, so the result
  // is exact even beyond the range of (float)n. Folding agrees.
  ASSERT(-2001207296, fbits(id(70000) * 0.5));
  ASSERT(-2001207296, fbits(70000 * 0.5));
  ASSERT(65536, fbits(fid(2.0) / id(2)));
  ASSERT(1, fbits(fid(1.0) / id(65536)));
  ASSERT(1, fbits(1.0 / 65536));
}
