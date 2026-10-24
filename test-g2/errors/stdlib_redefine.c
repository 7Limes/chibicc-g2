// expect-error: redefinition of min
#include "test.h"
int min(int a, int b) { return a; }
