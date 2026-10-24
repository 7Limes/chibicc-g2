#include "test.h"

int a = 5;
int b;
int c = -100000, d = 0x7FFFFFFF;
char ch = 'A';
_Bool flag = 7;
extern int later;
int later = 42;

extern int scratch __attribute__((address(12)));

int next_id() {
  static int id = 100;
  id = id + 1;
  return id;
}

int read_later() { return later; }

void start() {
  ASSERT(5, a);
  ASSERT(0, b);
  ASSERT(-100000, c);
  ASSERT(2147483647, d);
  ASSERT(65, ch);
  ASSERT(1, flag);
  ASSERT(42, read_later());
  ASSERT(101, next_id());
  ASSERT(102, next_id());

  b = a * 2;
  ASSERT(10, b);

  scratch = 77;
  ASSERT(77, scratch);

  ASSERT(1, sizeof(int));
  ASSERT(1, sizeof(char));
  ASSERT(1, sizeof(int *));
  ASSERT(32, __CHAR_BIT__);
}
