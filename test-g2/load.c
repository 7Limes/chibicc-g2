#include "test.h"

G2_LOAD(GREETING, raw, string, "Hi!");
G2_LOAD(RAW, raw, file, "data/four.bin");
G2_LOAD(PACKED, pack, file, "data/four.bin");
extern const int RAW[];

int at(const int *p, int i) { return p[i]; }

void start() {
  // A loaded string starts with its length.
  ASSERT(3, GREETING[0]);
  ASSERT('H', GREETING[1]);
  ASSERT('!', GREETING[3]);
  ASSERT(1, RAW[0]);
  ASSERT(255, RAW[3]);
  int i = 2;
  ASSERT(3, RAW[i]);
  ASSERT(3, at(RAW, 2));
  ASSERT(-16580095, PACKED[0]); // 0xFF030201
  ASSERT(1, GREETING != RAW);
}
