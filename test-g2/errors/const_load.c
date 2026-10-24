// expect-error: cannot assign to a const object
#include "test.h"
G2_LOAD(D, raw, string, "x");
void start() { D[0] = 1; }
