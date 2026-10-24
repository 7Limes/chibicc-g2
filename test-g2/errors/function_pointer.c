// expect-error: function pointers are not supported on g2
#include "test.h"
int g(void) { return 1; }
int h;
void start() { h = (int)g; }
