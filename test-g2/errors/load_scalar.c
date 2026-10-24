// expect-error: g2_load requires an int or char array declaration
int d __attribute__((g2_load("raw", "string", "x")));
