// expect-error: invalid operands to &
float f;
void tick() { int x = 3 & f; }
