#include "test.h"

int classify(int x) {
  switch (x) {
  case 0:
    return 100;
  case 1:
  case 2:
    return 200;
  case 3 ... 5:
    return 300;
  case -1:
    return -100;
  default:
    return 0;
  }
}

int fallthrough(int x) {
  int r = 0;
  switch (x) {
  case 1:
    r += 1;
  case 2:
    r += 10;
    break;
  case 3:
    r += 100;
  }
  return r;
}

int no_default(int x) {
  int r = 7;
  switch (x * 2) {
  case 4:
    r = 4;
    break;
  }
  return r;
}

void start() {
  ASSERT(100, classify(0));
  ASSERT(200, classify(1));
  ASSERT(200, classify(2));
  ASSERT(300, classify(3));
  ASSERT(300, classify(5));
  ASSERT(0, classify(6));
  ASSERT(-100, classify(-1));
  ASSERT(0, classify(-2));
  ASSERT(11, fallthrough(1));
  ASSERT(10, fallthrough(2));
  ASSERT(100, fallthrough(3));
  ASSERT(0, fallthrough(4));
  ASSERT(4, no_default(2));
  ASSERT(7, no_default(3));

  // Statement expressions
  ASSERT(3, ({ int a = 1; a + 2; }));
  int y = ({ int t = 4; t * t; });
  ASSERT(16, y);
}
