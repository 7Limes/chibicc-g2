#define G2_NO_STDLIB
#include "test.h"

// With G2_NO_STDLIB, a program may use the library's names.
int min(int a, int b) { return a > b ? a : b; }

void start() { ASSERT(5, min(2, 5)); }
