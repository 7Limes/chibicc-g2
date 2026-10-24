// expect-error: call to undefined function 'missing'
#include "test.h"
int missing(int x);
void start() { missing(1); }
