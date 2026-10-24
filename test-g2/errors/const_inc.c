// expect-error: cannot assign to a const object
#include "test.h"
void start() { const int x = 1; x++; }
