#ifndef __FLOAT_H
#define __FLOAT_H

// Floating types on g2 are Q16.16 fixed point: one slot holding
// round(x * 65536). There is no exponent, so the range is fixed and every
// value is a multiple of 2^-16. float, double and long double are the
// same.

#define FLT_RADIX 2
#define FLT_ROUNDS 3 // toward negative infinity
#define FLT_EVAL_METHOD 0
#define DECIMAL_DIG 5

#define FLT_DIG 4
#define FLT_EPSILON 0x1p-16f // the smallest positive value
#define FLT_MIN 0x1p-16f     // there are no subnormals, so it's the same
#define FLT_TRUE_MIN 0x1p-16f
#define FLT_MAX 0x7fffffffp-16f // 32767.99998

#define DBL_DIG FLT_DIG
#define DBL_EPSILON 0x1p-16
#define DBL_MIN 0x1p-16
#define DBL_TRUE_MIN 0x1p-16
#define DBL_MAX 0x7fffffffp-16

#define LDBL_DIG FLT_DIG
#define LDBL_EPSILON 0x1p-16L
#define LDBL_MIN 0x1p-16L
#define LDBL_TRUE_MIN 0x1p-16L
#define LDBL_MAX 0x7fffffffp-16L

#endif
