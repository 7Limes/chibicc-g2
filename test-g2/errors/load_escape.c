// expect-error: loaded strings must be printable ASCII
#include "test.h"
G2_LOAD(D, raw, string, "a\nb");
