// expect-error: struct and union assignments are not supported yet
#include "test.h"
struct S { int a; } s, t;
void start() { s = t; }
