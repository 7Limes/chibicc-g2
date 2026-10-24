// expect-error: a variable with a fixed address can't have an initializer
int k __attribute__((address(12))) = 3;
