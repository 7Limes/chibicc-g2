// expect-error: recursion is not supported on g2: start -> a -> b -> a
#include "test.h"
int b(int x);
int a(int x) { return b(x); }
int b(int x) { return a(x); }
void start() { a(1); }
