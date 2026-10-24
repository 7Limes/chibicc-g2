#include "test.h"

int sign(int x) {
  if (x < 0)
    return -1;
  else if (x == 0)
    return 0;
  else
    return 1;
}

int sum_to(int n) {
  int sum = 0;
  int i = 1;
  while (i <= n) {
    sum = sum + i;
    i = i + 1;
  }
  return sum;
}

int first_multiple(int n, int start, int limit) {
  for (int i = start; i < limit; i = i + 1) {
    if (i % n)
      continue;
    return i;
  }
  return -1;
}

int count_until(int stop) {
  int i = 0;
  for (;;) {
    if (i == stop)
      break;
    i = i + 1;
  }
  return i;
}

int do_count(int n) {
  int i = 0;
  do {
    i = i + 1;
  } while (i < n);
  return i;
}

int goto_test(int n) {
  int i = 0;
again:
  i = i + 1;
  if (i < n)
    goto again;
  return i;
}

int nested(int n) {
  int total = 0;
  for (int i = 0; i < n; i = i + 1)
    for (int j = 0; j < n; j = j + 1) {
      if (j > i)
        break;
      total = total + 1;
    }
  return total;
}

int collatz_steps(int n) {
  int steps = 0;
  while (n != 1) {
    if (n % 2 == 0)
      n = n / 2;
    else
      n = 3 * n + 1;
    steps = steps + 1;
  }
  return steps;
}

void start() {
  ASSERT(-1, sign(-5));
  ASSERT(0, sign(0));
  ASSERT(1, sign(9));
  ASSERT(55, sum_to(10));
  ASSERT(0, sum_to(0));
  ASSERT(21, first_multiple(7, 15, 100));
  ASSERT(-1, first_multiple(7, 15, 20));
  ASSERT(12, count_until(12));
  ASSERT(5, do_count(5));
  ASSERT(1, do_count(0));
  ASSERT(4, goto_test(4));
  ASSERT(10, nested(4));
  ASSERT(111, collatz_steps(27));

  int x = 0;
  if (1)
    x = 1;
  if (0)
    x = 2;
  ASSERT(1, x);
  { int x = 5; ASSERT(5, x); }
  ASSERT(1, x);
}
