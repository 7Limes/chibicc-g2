// Test harness for g2 programs. Tests run in start(), so one frame is
// enough. The runner (tools/run_tests.py) reads the result variables.
#include <g2.h>

int __test_count;
int __test_failed_line;
int __test_expected;
int __test_actual;

static void assert_eq(int line, int expected, int actual) {
  __test_count = __test_count + 1;
  if (expected != actual && !__test_failed_line) {
    __test_failed_line = line;
    __test_expected = expected;
    __test_actual = actual;
  }
}

#define ASSERT(x, y) assert_eq(__LINE__, x, y)

// Passes if |expected - actual| <= tolerance.
static void assert_near(int line, int expected, int actual, int tolerance) {
  int diff = expected - actual;
  if (diff < 0)
    diff = -diff;
  if (diff > tolerance)
    assert_eq(line, expected, actual);
  else
    assert_eq(line, actual, actual);
}

#define ASSERT_NEAR(x, y, tolerance) assert_near(__LINE__, x, y, tolerance)

// The raw Q16.16 value of a float: fbits(1.5) == 98304.
static int fbits(float f) { return *(int *)&f; }

void tick() {}
