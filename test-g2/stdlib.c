#include "test.h"

int s[12];

int str_eq(int *str, char *expected) {
  int n = 0;
  while (expected[n])
    n++;
  if (str[0] != n)
    return 0;
  for (int i = 0; i < n; i++)
    if (str[i + 1] != expected[i])
      return 0;
  return 1;
}

void start() {
  ASSERT(3, min(3, 7));
  ASSERT(-7, min(3, -7));
  ASSERT(7, max(3, 7));
  ASSERT(3, max(3, -7));

  // Expected values computed with x = x * 1583458089 (32-bit) % 2147483647.
  seed_rand(1337);
  ASSERT(1812071712, next_rand());
  ASSERT(1321906720, next_rand());
  ASSERT(382297888, next_rand());
  ASSERT(1537696799, next_rand());
  ASSERT(836649463, next_rand());

  int_to_str(s, 0);
  ASSERT(1, str_eq(s, "0"));
  int_to_str(s, 7);
  ASSERT(1, str_eq(s, "7"));
  int_to_str(s, 4072);
  ASSERT(1, str_eq(s, "4072"));
  int_to_str(s, -4072);
  ASSERT(1, str_eq(s, "-4072"));
  int_to_str(s, 2147483647);
  ASSERT(1, str_eq(s, "2147483647"));
  int_to_str(s, -10);
  ASSERT(1, str_eq(s, "-10"));
}
