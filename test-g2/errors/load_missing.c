// expect-error: cannot open
#include "test.h"
G2_LOAD(D, raw, file, "missing.bin");
void start() {}
