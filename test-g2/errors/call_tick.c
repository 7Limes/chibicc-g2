// expect-error: tick() is called by the program's entry stub
#include <g2.h>
void tick() {}
void start() { tick(); }
