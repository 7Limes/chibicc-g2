// expect-error: cannot convert between a pointer and a floating type
int x;
void tick() { float f = (float)&x; }
