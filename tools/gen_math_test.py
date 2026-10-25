#!/usr/bin/env python3
"""Generates test-g2/math.c, which checks math.h and float_to_str.

Values are Q16.16 raw integers (round(x * 65536)). Exact functions,
including sqrtf (the integer square root of raw * 65536), must match
exactly. The polynomial approximations must be within a few raw units of
Python's math module.

Usage: tools/gen_math_test.py > test-g2/math.c
"""

import math
import random

F = 65536
INT_MIN = -2**31

# Allowed error, in raw units (1/65536).
SIN_TOLERANCE = 6
ATAN_TOLERANCE = 4


def raw(x):
    return math.floor(x * F + 0.5)


def int_lit(n):
    return '(-2147483647 - 1)' if n == INT_MIN else str(n)


def floor_raw(r):
    return (r >> 16) << 16


def ceil_raw(r):
    return -floor_raw(-r)


def trunc_raw(r):
    return ceil_raw(r) if r < 0 else floor_raw(r)


def round_raw(r):
    return -floor_raw(F // 2 - r) if r < 0 else floor_raw(r + F // 2)


def fmod_raw(a, b):
    r = abs(a) % abs(b)
    return -r if a < 0 else r


def sqrt_raw(r):
    return math.isqrt(r * F) if r > 0 else 0


def float_str(r, decimals):
    """What float_to_str prints: |x| rounded half up to `decimals` places."""
    scale = 10**decimals
    digits = (abs(r) * scale + F // 2) // F
    whole, frac = divmod(digits, scale)
    s = str(whole) + (f'.{frac:0{decimals}d}' if decimals else '')
    return ('-' if r < 0 and digits else '') + s


def main():
    rng = random.Random(2026)
    lines = []

    def add(expected, expr):
        lines.append(f'  ASSERT({int_lit(expected)}, fbits({expr}));')

    def near(expected, expr, tol):
        lines.append(f'  ASSERT_NEAR({expected}, fbits({expr}), {tol});')

    values = [0, 1, -1, F // 2, -F // 2, F // 2 - 1, F // 2 + 1, -F // 2 - 1, F, -F, F + 1,
              raw(2.5), raw(-2.5), raw(2.4), raw(-3.7), raw(1234.5678), raw(-1234.5678),
              2**31 - 1 - F, INT_MIN + F, raw(32766.5), raw(-32767.5)]
    values += [rng.randrange(-2**31 + F, 2**31 - F) >> rng.randrange(16) for _ in range(15)]

    for r in values:
        x = f'fv({int_lit(r)})'
        add(abs(r), f'fabsf({x})')
        add(floor_raw(r), f'floorf({x})')
        add(ceil_raw(r), f'ceilf({x})')
        add(trunc_raw(r), f'truncf({x})')
        add(round_raw(r), f'roundf({x})')
        add(sqrt_raw(r), f'sqrtf({x})')

    for a in values[::2]:
        for b in values[1::3]:
            if b:
                add(fmod_raw(a, b), f'fmodf(fv({int_lit(a)}), fv({int_lit(b)}))')

    for a, b in [(raw(1.5), raw(-2)), (raw(-1.5), raw(2)), (raw(0), raw(-1)), (raw(3), raw(3))]:
        add(a if (a < 0) == (b < 0) else -a, f'copysignf(fv({a}), fv({b}))')
        add(min(a, b), f'fminf(fv({a}), fv({b}))')
        add(max(a, b), f'fmaxf(fv({a}), fv({b}))')

    # Every raw value from 0 to 2^31 has an exact square root; check a spread.
    for r in [2, 3, 4, 100, 65535, 4 * F, 9 * F, 2 * F, 2**31 - 1, 2**30, 2**30 - 1] + \
             [rng.randrange(1, 2**31) for _ in range(20)]:
        add(sqrt_raw(r), f'sqrtf(fv({r}))')

    angles = [0, 1, -1, raw(0.5), raw(-0.5), raw(1), raw(math.pi / 6), raw(math.pi / 4),
              raw(math.pi / 2), raw(-math.pi / 2), raw(math.pi), raw(-math.pi), raw(3),
              raw(-3), raw(4), raw(6.28), raw(100), raw(-100), raw(1000.25), raw(32767),
              raw(-32767.9), 2**31 - 1, INT_MIN]
    angles += [rng.randrange(-2**31, 2**31) for _ in range(20)]
    angles += [rng.randrange(-raw(7), raw(7)) for _ in range(30)]
    for r in angles:
        near(raw(math.sin(r / F)), f'sinf(fv({int_lit(r)}))', SIN_TOLERANCE)
        near(raw(math.cos(r / F)), f'cosf(fv({int_lit(r)}))', SIN_TOLERANCE)

    for t in [0.3, -0.3, 1, -1.2, 1.5]:
        exact = math.tan(t)
        # sin and cos errors are amplified by 1/cos^2.
        tol = math.ceil(SIN_TOLERANCE * (1 + exact * exact) * 2)
        near(raw(exact), f'tanf({t})', tol)

    points = [(0, 1), (1, 0), (0, -1), (-1, 0), (1, 1), (-1, 1), (1, -1), (-1, -1),
              (0.5, 2), (2, 0.5), (-3, 0.25), (0.001, 1), (1, 0.001), (100, -300),
              (-20000, 20000), (0.25, -0.0625)]
    points += [(rng.uniform(-30000, 30000), rng.uniform(-30000, 30000)) for _ in range(12)]
    points += [(rng.uniform(-2, 2), rng.uniform(-2, 2)) for _ in range(12)]
    for y, x in points:
        ry, rx = raw(y), raw(x)
        near(raw(math.atan2(ry, rx)), f'atan2f(fv({ry}), fv({rx}))', ATAN_TOLERANCE)
    for x in [0, 0.5, -0.5, 1, -1, 2, -10, 1000]:
        near(raw(math.atan(raw(x) / F)), f'atanf(fv({raw(x)}))', ATAN_TOLERANCE)

    for y, x in [(3, 4), (-3, 4), (0, -5), (0.001, 0.001), (1, 1), (300, 400),
                 (20000, 20000), (-30000, 100), (0.5, 0.25), (0, 0)]:
        ry, rx = raw(y), raw(x)
        exact = math.hypot(ry, rx)
        if exact < 2**31:
            near(round(exact), f'hypotf(fv({ry}), fv({rx}))', 3 + int(exact) // 2**20)

    str_lines = []
    for r, d in [(0, 2), (raw(1.5), 1), (raw(-2.25), 2), (raw(3.14159), 4), (raw(3.14159), 0),
                 (raw(-0.004), 2), (raw(0.996), 2), (raw(-9.9999), 3), (raw(32767.99998), 4),
                 (INT_MIN, 3), (raw(12.5), 0), (raw(-12.5), 0), (raw(100), 6), (raw(7), -1)]:
        expected = float_str(r, min(max(d, 0), 4))
        str_lines.append(f'  float_to_str(s, fv({int_lit(r)}), {d});')
        str_lines.append(f'  ASSERT(1, str_eq(s, "{expected}"));')

    print('#include "test.h"')
    print('#include <math.h>')
    print('#include <float.h>')
    print()
    print('// Generated by tools/gen_math_test.py: math.h and float_to_str against')
    print('// exact results or Python\'s math module. fv() hides values from the')
    print('// compiler.')
    print()
    print('float fv(int raw) { return *(float *)&raw; }')
    print()
    print('int s[12];')
    print()
    print('int str_eq(int *str, char *expected) {')
    print('  int n = 0;')
    print('  while (expected[n])')
    print('    n++;')
    print('  if (str[0] != n)')
    print('    return 0;')
    print('  for (int i = 0; i < n; i++)')
    print('    if (str[i + 1] != expected[i])')
    print('      return 0;')
    print('  return 1;')
    print('}')
    print()
    print('void start() {')
    print('  ASSERT(1, sizeof(float));')
    print('  ASSERT(1, fbits(FLT_EPSILON));')
    print('  ASSERT(2147483647, fbits(FLT_MAX));')
    print('  ASSERT(16, __G2_FIXED_FRAC_BITS__);')
    print(f'  ASSERT({raw(math.pi)}, fbits(M_PI));')
    print('  ASSERT(fbits(sqrtf(fv(123456))), fbits(sqrt(fv(123456))));')
    print('\n'.join(lines))
    print('\n'.join(str_lines))
    print('}')


main()
