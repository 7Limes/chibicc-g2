#include "chibicc.h"

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

// Emits g2 assembly for a parsed program.
//
// The g2 backend is not implemented yet (see Phase 1 of g2-port-plan.md),
// so nothing is emitted.
void codegen(Obj *prog, FILE *out) {
}
