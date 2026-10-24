// expect-error: WIDTH must be at least 1 (got 0)
#define WIDTH (8 - 8)
#include "test.h"
void start() { ASSERT(1, 1); }
