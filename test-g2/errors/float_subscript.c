// expect-error: array subscript is not an integer
int a[4];
float f;
void tick() { a[f] = 1; }
