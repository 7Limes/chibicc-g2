#ifndef __MATH_H
#define __MATH_H

// Math functions for g2's Q16.16 floats (see float.h). Every function is
// static, so the ones a program doesn't call are not emitted.
//
// There are no infinities or NaNs: sqrtf of a negative number is 0, and
// results that don't fit wrap around like integer overflow. The double
// names (sqrt, sin, ...) are the float functions, since the types are the
// same. sinf, cosf and atan2f are polynomial approximations, accurate to
// about 1e-4.

#define M_E 2.718281828459045
#define M_PI 3.141592653589793
#define M_PI_2 1.5707963267948966
#define M_PI_4 0.7853981633974483
#define M_SQRT2 1.4142135623730951

// Raw values of 2*pi (rounded up by about 0.17 raw units), pi and pi/2.
#define __G2_TWO_PI 411775
#define __G2_PI 205887
#define __G2_PI_2 102944

//
// Exact functions
//

static float fabsf(float x) {
  return x < 0 ? -x : x;
}

static float fminf(float x, float y) {
  return x < y ? x : y;
}

static float fmaxf(float x, float y) {
  return x > y ? x : y;
}

static float copysignf(float x, float y) {
  return (x < 0) == (y < 0) ? x : -x;
}

// Converting to int rounds toward negative infinity on g2.
static float floorf(float x) {
  return (int)x;
}

static float ceilf(float x) {
  return -floorf(-x);
}

static float truncf(float x) {
  return x < 0 ? ceilf(x) : floorf(x);
}

// Rounds halfway cases away from zero, as in C.
static float roundf(float x) {
  return x < 0 ? -floorf(0.5f - x) : floorf(x + 0.5f);
}

// The remainder of x / y with the sign of x, as in C. It's exact: on the
// raw values it's an integer remainder. Returns 0 if y is 0.
static float fmodf(float x, float y) {
  int a = __G2_FBITS(x);
  int b = __G2_FBITS(y);
  if (b == 0)
    return 0;
  int r = a % b; // has the sign of b
  if (r != 0 && (r < 0) != (a < 0))
    r -= b;
  float result;
  __G2_FBITS(result) = r;
  return result;
}

// The small functions above are also macros, so they compile inline
// instead of costing a call. Each argument is evaluated once.
#define fabsf(x) ({ float __g2_x = (x); __g2_x < 0 ? -__g2_x : __g2_x; })
#define fminf(x, y) ({ float __g2_x = (x), __g2_y = (y); __g2_x < __g2_y ? __g2_x : __g2_y; })
#define fmaxf(x, y) ({ float __g2_x = (x), __g2_y = (y); __g2_x > __g2_y ? __g2_x : __g2_y; })
#define floorf(x) ((float)(int)(x))
#define ceilf(x) (-floorf(-(x)))
#define truncf(x) ({ float __g2_x = (x); __g2_x < 0 ? ceilf(__g2_x) : floorf(__g2_x); })
#define roundf(x) \
  ({ float __g2_x = (x); __g2_x < 0 ? -floorf(0.5f - __g2_x) : floorf(__g2_x + 0.5f); })

// Rounds down, like the other operations: sqrtf(x) is the largest float
// whose square is at most x. Returns 0 for x <= 0.
//
// The raw result is floor(sqrt(raw(x) * 2^16)), which needs 47 bits. The
// integer square root s of raw(x) comes first, by Newton's method from a
// power of two above it. Then 8 more bits come one at a time, as in long
// division, from the remainder raw(x) - s*s. The remainder stays below
// 2^26, so nothing overflows.
static float sqrtf(float x) {
  int a = __G2_FBITS(x);
  float result = 0;
  if (a <= 0)
    return result;

  int s = 1;
  for (int t = a; t > 0; t /= 4)
    s *= 2;
  for (;;) {
    int next = (s + a / s) / 2;
    if (next >= s)
      break;
    s = next;
  }

  int rem = a - s * s;
  for (int i = 0; i < 8; i++) {
    rem *= 4;
    int trial = s * 4 + 1;
    s *= 2;
    if (rem >= trial) {
      rem -= trial;
      s++;
    }
  }
  __G2_FBITS(result) = s;
  return result;
}

// sqrt(x*x + y*y). Both values are scaled by a power of two so that the
// larger magnitude is in [64, 128), where the sum of squares neither
// overflows nor loses bits. Scaling up is exact; scaling down by 2^k
// drops the low k bits, so the error is a few units of 2^(k-16).
static float hypotf(float x, float y) {
  float a = fabsf(x);
  float b = fabsf(y);
  float m = fmaxf(a, b);
  if (m == 0)
    return 0;

  int up = 1;
  int down = 1;
  for (; m < 64; m *= 2)
    up *= 2;
  for (; m >= 128; m /= 2)
    down *= 2;
  a = a * up / down;
  b = b * up / down;
  return sqrtf(a * a + b * b) * down / up;
}

//
// Trigonometry
//

// Reduces the raw angle r to [-pi, pi). k full turns of the rounded-up
// 2*pi remove 0.1677 * k raw units too much, which is added back.
static int __g2_reduce_angle(int r) {
  int k = r / __G2_TWO_PI;
  int t = r % __G2_TWO_PI + k * 10991 / 65536;
  if (t >= __G2_PI)
    t -= __G2_TWO_PI;
  return t;
}

// sin of the raw angle t in [-pi, pi]. The angle is folded into
// [0, pi/2], where sin(x) = x + x^3*(c3 + x^2*(c5 + x^2*c7)). The
// coefficients were fitted for this exact fixed-point evaluation.
static float __g2_sin(int t) {
  if (t > __G2_PI_2)
    t = __G2_PI - t;
  else if (t < -__G2_PI_2)
    t = -__G2_PI - t;
  int negative = t < 0;
  if (negative)
    t = -t;

  float x;
  __G2_FBITS(x) = t;
  float x2 = x * x;
  float p = x2 * __G2_RAW(-14) + __G2_RAW(553);
  p = p * x2 + __G2_RAW(-10929);
  float s = x + x * (p * x2);
  return negative ? -s : s;
}

static float sinf(float x) {
  return __g2_sin(__g2_reduce_angle(__G2_FBITS(x)));
}

static float cosf(float x) {
  // cos(x) == sin(x + pi/2), shifted after reducing so it can't overflow.
  int t = __g2_reduce_angle(__G2_FBITS(x)) + __G2_PI_2;
  if (t >= __G2_PI)
    t -= __G2_TWO_PI;
  return __g2_sin(t);
}

static float tanf(float x) {
  return sinf(x) / cosf(x);
}

// The angle of (x, y), in [-pi, pi]. The ratio of the smaller to the
// larger magnitude is in [0, 1], where
// atan(z) = z*(c1 + z^2*(c3 + z^2*(c5 + z^2*(c7 + z^2*c9)))), and the
// octant gives the rest.
static float atan2f(float y, float x) {
  float ax = fabsf(x);
  float ay = fabsf(y);
  if (ax == 0 && ay == 0)
    return 0;

  int steep = ay > ax;
  float z = steep ? ax / ay : ay / ax;
  float z2 = z * z;
  float p = z2 * __G2_RAW(1363) + __G2_RAW(-5582);
  p = p * z2 + __G2_RAW(11810);
  p = p * z2 + __G2_RAW(-21644);
  p = p * z2 + __G2_RAW(65527);
  float a = z * p;

  if (steep)
    a = __G2_RAW(__G2_PI_2) - a;
  if (x < 0)
    a = __G2_RAW(__G2_PI) - a;
  return y < 0 ? -a : a;
}

static float atanf(float x) {
  return atan2f(x, 1);
}

#define fabs(x) fabsf(x)
#define fmin(x, y) fminf(x, y)
#define fmax(x, y) fmaxf(x, y)
#define copysign(x, y) copysignf(x, y)
#define floor(x) floorf(x)
#define ceil(x) ceilf(x)
#define trunc(x) truncf(x)
#define round(x) roundf(x)
#define fmod(x, y) fmodf(x, y)
#define sqrt(x) sqrtf(x)
#define hypot(x, y) hypotf(x, y)
#define sin(x) sinf(x)
#define cos(x) cosf(x)
#define tan(x) tanf(x)
#define atan2(y, x) atan2f(y, x)
#define atan(x) atanf(x)

#endif
