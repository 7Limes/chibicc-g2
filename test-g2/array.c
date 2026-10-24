#include "test.h"

int g[5] = {5, 4, 3};
int grid[3][4];
char str[] = "abc";
int zeros[40];

int sum(int *p, int n) {
  int s = 0;
  for (int i = 0; i < n; i++)
    s += p[i];
  return s;
}

void fill(int *p, int n, int v) {
  for (int i = 0; i < n; i++)
    p[i] = v;
}

int strlen_(char *s) {
  int n = 0;
  while (s[n])
    n++;
  return n;
}

void start() {
  ASSERT(5, g[0]);
  ASSERT(3, g[2]);
  ASSERT(0, g[4]);
  ASSERT(12, sum(g, 5));
  g[4] = 8;
  ASSERT(20, sum(g, 5));
  int i = 3;
  g[i] = 1;
  ASSERT(1, g[3]);
  ASSERT(4, g[i - 2]);
  ASSERT(5, *g);

  int local[6] = {1, 2};
  ASSERT(1, local[0]);
  ASSERT(2, local[1]);
  ASSERT(0, local[5]);
  fill(local, 6, 7);
  ASSERT(42, sum(local, 6));
  local[i] = 100;
  ASSERT(135, sum(local, 6));

  // A large local array is cleared with a loop.
  int big[40] = {1};
  ASSERT(1, big[0]);
  ASSERT(0, big[39]);
  ASSERT(1, sum(big, 40));

  for (int r = 0; r < 3; r++)
    for (int c = 0; c < 4; c++)
      grid[r][c] = r * 10 + c;
  ASSERT(23, grid[2][3]);
  ASSERT(10, grid[1][0]);
  ASSERT(1, *grid[0] + 1);
  ASSERT(4, sizeof(grid[0]));
  ASSERT(12, sizeof(grid));
  int *row = grid[1];
  ASSERT(12, row[2]);

  ASSERT(3, strlen_(str));
  ASSERT(98, str[1]);
  ASSERT(5, strlen_("hello"));
  ASSERT(108, "hello"[2]);
  char *s = "xyz";
  ASSERT(122, s[2]);
  ASSERT(0, s[3]);
  ASSERT(4, sizeof(str));

  ASSERT(0, sum(zeros, 40));
}
