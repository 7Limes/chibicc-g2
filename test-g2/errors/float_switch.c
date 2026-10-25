// expect-error: switch quantity is not an integer
float f;
void tick() { switch (f) { case 1: break; } }
