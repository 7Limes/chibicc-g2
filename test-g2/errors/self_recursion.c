// expect-error: recursion is not supported on g2: start -> fact -> fact
#include "test.h"
int fact(int n) { if (n < 2) return 1; return n * fact(n - 1); }
void start() { fact(3); }
