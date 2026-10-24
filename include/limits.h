#ifndef __LIMITS_H
#define __LIMITS_H

// On g2 every integer type (char included) is a 32-bit signed int
// occupying one memory slot.
#define CHAR_BIT 32
#define MB_LEN_MAX 1

#define SCHAR_MIN (-2147483647 - 1)
#define SCHAR_MAX 2147483647
#define CHAR_MIN SCHAR_MIN
#define CHAR_MAX SCHAR_MAX

#define SHRT_MIN (-2147483647 - 1)
#define SHRT_MAX 2147483647

#define INT_MIN (-2147483647 - 1)
#define INT_MAX 2147483647

#define LONG_MIN (-2147483647 - 1)
#define LONG_MAX 2147483647

#define LLONG_MIN (-2147483647 - 1)
#define LLONG_MAX 2147483647

#endif
