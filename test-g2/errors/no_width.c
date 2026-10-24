// expect-error: WIDTH is not defined
#include "test.h"
#undef WIDTH
void start() { ASSERT(1, 1); }
