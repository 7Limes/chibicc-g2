// expect-error: call depth 17 exceeds the g2 call stack (16): start -> f15
#include "test.h"
int f0() { return 0; }
int f1() { return f0(); }
int f2() { return f1(); }
int f3() { return f2(); }
int f4() { return f3(); }
int f5() { return f4(); }
int f6() { return f5(); }
int f7() { return f6(); }
int f8() { return f7(); }
int f9() { return f8(); }
int f10() { return f9(); }
int f11() { return f10(); }
int f12() { return f11(); }
int f13() { return f12(); }
int f14() { return f13(); }
int f15() { return f14(); }
void start() { f15(); }
