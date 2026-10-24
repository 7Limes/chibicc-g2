#include "test.h"

int g = 5;
int arr[3] = {10, 20, 30};
struct { int a, b; } s = {1, 2};

int counter;
int *next_slot() { counter++; return &arr[1]; }

void start() {
  int x = 10;
  x += 5;
  ASSERT(15, x);
  x -= 3;
  ASSERT(12, x);
  x *= 2;
  ASSERT(24, x);
  x /= 5;
  ASSERT(4, x);
  x %= 3;
  ASSERT(1, x);
  ASSERT(6, x += 5);

  ASSERT(6, x++);
  ASSERT(7, x);
  ASSERT(8, ++x);
  ASSERT(8, x--);
  ASSERT(6, --x);
  x++;
  ASSERT(7, x);

  g++;
  g += g;
  ASSERT(12, g);

  arr[0]++;
  arr[2] -= 5;
  ASSERT(11, arr[0]);
  ASSERT(25, arr[2]);
  int i = 1;
  arr[i] *= 3;
  ASSERT(60, arr[1]);
  ASSERT(60, arr[i]++);
  ASSERT(61, arr[1]);

  // The address is evaluated only once.
  *next_slot() += 4;
  ASSERT(65, arr[1]);
  ASSERT(1, counter);

  s.b += 10;
  s.a++;
  ASSERT(12, s.b);
  ASSERT(2, s.a);

  int *p = arr;
  p++;
  ASSERT(65, *p);
  p += 1;
  ASSERT(25, *p);
  ASSERT(25, *p--);
  ASSERT(65, *p);
  (*p)++;
  ASSERT(66, arr[1]);

  int total = 0;
  for (int k = 0; k < 10; k++)
    total += k;
  ASSERT(45, total);
}
