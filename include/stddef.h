#ifndef __STDDEF_H
#define __STDDEF_H

#define NULL ((void *)0)

// Sizes on g2 are counted in memory slots, and every integer type is
// a 32-bit signed int.
typedef int size_t;
typedef int ptrdiff_t;
typedef int wchar_t;
typedef int max_align_t;

#define offsetof(type, member) ((size_t)&(((type *)0)->member))

#endif
