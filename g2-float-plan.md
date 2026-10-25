# Plan: `float` as Fixed Point on g2

This plan adds `float` (and `double`) to chibicc-g2. g2 only has signed 32-bit integer slots, so a
floating type is represented as a **fixed-point number**: a 32-bit integer that counts units of
2⁻¹⁶. It follows the conventions of `g2-port-plan.md`, and section numbers like §3.5 refer to that
document.

---

## 1. Goals and Non-Goals

### Goals

- `float`, `double` and `long double` compile. Variables, parameters, return values, arrays, struct
  members, pointers to them, and global initialisers all work.
- Floating literals (`1.5`, `.25f`, `3e2`, `0x1p-4`) compile to fixed-point constants.
- `+ - * /`, unary `-`, comparisons, `!`, `&&`, `||`, `?:`, `op=`, `++`/`--`, and conversions
  between floating and integer types all work.
- Constant folding gives exactly the same bits as the code that runs on the VM, as it already does
  for integers (§3.5).
- The cheap operations (add, subtract, compare) cost the same as integer operations. Multiplication
  and division by constants or by integers compile inline. Only general `float * float` and
  `float / float` call runtime helpers.
- A small `math.h` / `float.h` that is useful for games: `sqrtf`, `sinf`, `cosf`, `floorf`, `fabsf`, …

### Non-Goals

- IEEE 754 behaviour. There are no infinities, NaNs, signed zeros, subnormals or exponent; overflow
  wraps around like integer overflow does.
- A choice of formats at compile time (`-mfixed=Q24.8`). The format is one constant in the compiler
  and in `__g2rt.h`, so this can come later (§12).
- `printf`-style formatting. A `float_to_str` for `draw_text` is enough.

---

## 2. Number Format: Q16.16

A `float` is one slot that holds `round(x · 65536)`: 16 integer bits (including the sign) and 16
fraction bits.

| Property | Value |
|---|---|
| Size | 1 slot (`sizeof(float) == sizeof(double) == 1`) |
| Raw value of `1.0` | `65536` (`0x10000`) |
| Range | −32768.0 to 32767.99998 |
| Resolution (`FLT_EPSILON`) | 1/65536 ≈ 0.0000153 |
| Decimal precision | about 4.8 significant digits |

Why Q16.16:

- It covers screen coordinates and velocities comfortably (windows are at most a few hundred pixels)
  and has enough fraction bits that `x += 0.01 * DELTA` doesn't stall.
- Splitting a value into integer and fraction parts is `x / 65536` and `x % 65536`. g2's `div` and
  `mod` floor, so this split is exact for negative numbers too, with the fraction always in
  `[0, 65536)`. Both the runtime helpers and the inline sequences rely on this.
- The 16/16 split makes the multiplication helper simple (§6.1).

Its weak point is range: `x * x` overflows once `|x| > 181`, and `dist² = dx*dx + dy*dy` overflows
at about 128. This is the price of 32 bits, and the library avoids it where it can (`hypotf` scales
first, §7). §11 lists the alternative formats.

Within this document, *raw* means the 32-bit integer that represents a float (`raw(1.5) == 98304`),
and `F = 65536`.

---

## 3. Language Semantics

### 3.1 Types

- `float`, `double` and `long double` are accepted in `declspec`. They keep their own `TypeKind`s
  (`TY_FLOAT`, `TY_DOUBLE`, `TY_LDOUBLE` already exist in `chibicc.h`), so `_Generic` and type
  compatibility behave as in C. All three have **the same representation**, and conversions between
  them are no-ops.
- `get_common_type`: if either operand is floating, the result is the wider floating type (`long
  double` > `double` > `float`), as in C. Otherwise it stays `int`.
- `is_flonum` and `is_numeric` already exist. Every place that currently means "an integer or a
  pointer" has to be checked, because floats are now a third kind of scalar.

### 3.2 Literals

`convert_pp_number` in `tokenize.c` currently errors on anything that isn't an integer. Instead:

1. Parse with `strtod` (decimal and hex floats). Accept the `f`/`F`/`l`/`L` suffixes. A literal with no
   suffix is `double`, with `f` it is `float`, as in C.
2. Convert with round-to-nearest: `raw = llround(v * 65536)`.
3. If `raw` doesn't fit in 32 bits (|v| ≥ 32768), report an error:
   `floating constant 40000.0 is out of range for g2 fixed point (-32768 to 32767.99998)`.
4. If `v != 0` but `raw == 0`, warn: `floating constant 1e-6 rounds to 0`.
5. Store the raw value in `tok->val` and the type in `tok->ty`. `primary()` must give the `ND_NUM`
   node `tok->ty` instead of `ty_int`.

So an `ND_NUM` whose type is floating holds a raw fixed-point value in `val`. No `fval` field is
needed, and every float constant in the compiler is an exact 32-bit integer.

### 3.3 Operators

| Operation | Allowed? | Semantics |
|---|---|---|
| `a + b`, `a - b`, `-a`, `+a` | yes | raw integer add/sub/negate, wrapping |
| `a < b` … `a == b`, `!a`, `&&`, `\|\|`, `?:`, `if (a)` | yes | raw integer comparison (the raw encoding is monotonic, and `raw == 0` ⇔ `a == 0.0`) |
| `a * b` | yes | `floor(raw(a) · raw(b) / F)` wrapped to 32 bits |
| `a / b` | yes | `floor(raw(a) · F / raw(b))` wrapped to 32 bits |
| `a % b`, `~a`, `&`, `\|`, `^`, `<<`, `>>` with a floating operand | **error**, as in C | `invalid operands to binary %` |
| `x op= y`, `x++`, `x--` | yes | via the rewrites the parser already does; `x++` adds `1.0` (raw 65536) |
| `p + f`, `arr[f]` | **error** | `array subscript is not an integer` (currently `new_add` would accept it) |
| `switch (f)` | **error** | `switch quantity is not an integer` |
| `(int *)f`, `(float)p` | **error** | `cannot convert between a pointer and a floating type` |

When one operand of `*` (or the right operand of `/`) is an integer converted to float, the
operation uses the integer itself: `a * (float)n` is the raw `a · n`, and `a / (float)n` is
`floor(raw(a) / n)`. This is exact and takes one instruction, and it stays exact when `(float)n`
alone would overflow (`|n| ≥ 32768`), where converting first would give a different, wrapped answer.
Constant folding follows the same rule, so folded and runtime results still agree.

Multiplication and division **round down** (toward −∞), like `>>` and like g2's integer division.
This keeps them consistent with the rest of the dialect, and makes `x * 0.5` exactly `x / 2.0` and
`x >> 1` on the raw value. The difference from rounding to nearest is at most one unit (1/65536).

### 3.4 Conversions

| Conversion | Lowering | Notes |
|---|---|---|
| integer → floating | `raw = n · F` (`mul d n __k65536`) | Wraps when `|n| ≥ 32768`. A constant that wraps gives a warning. |
| floating → integer | `n = floor(raw / F)` (`div d a __k65536`) | **Floors**, see below. |
| floating → `_Bool` | `raw != 0` | the existing `gen_truth` |
| `float` ↔ `double` ↔ `long double` | nothing | same representation |
| floating ↔ pointer | error | |

**Floating → integer rounds toward −∞**, so `(int)-0.5 == -1`, where C gives `0`. This is the same
dialect choice as for `/` (§3.5 of the port plan): it costs one instruction instead of about six, and
it is what pixel code usually wants, since `(int)x` then picks the pixel that contains `x` even left
of the origin. `truncf()` in `math.h` gives C's behaviour when it matters. The optional
`-mc-division` flag from the port plan should switch this to truncation as well (§11).

Implicit conversions already go through `new_cast` in the parser (assignment, arguments, `return`,
the usual arithmetic conversions, initialisers), so the backend only has to handle `ND_CAST`.

---

## 4. Compile-Time Arithmetic

Floats are raw integers in the AST, so constant folding needs only a few type-aware cases. Add these
next to `floor_div`/`floor_mod` in `parse.c` and export them in `chibicc.h`, so that the parser's
`eval2` and codegen's `fold`/`const_int` share one definition:

```c
#define FIX_ONE 65536

int64_t fix_from_int(Node *node, int64_t n);    // wrap32(n * 65536); warns when it wraps
int64_t fix_to_int(int64_t raw);                // floor_div(raw, 65536)
int64_t fix_mul(int64_t a, int64_t b);          // wrap32(floor(a * b / 65536)), exact in int64
int64_t fix_div(Node *node, int64_t a, int64_t b); // wrap32(floor(a * 65536 / b)); error if b == 0
```

`a * b` and `a * 65536` fit easily in an `int64_t`, so these are exact. The runtime helpers (§6) must
give the same bits for every input. The tests check this (§9).

Changes:

- **`eval2`**: `ND_MUL` and `ND_DIV` check `is_flonum(node->ty)` and use `fix_mul`/`fix_div`.
  `ND_CAST` converts when it crosses between integer and floating types. `ND_ADD`, `ND_SUB`, `ND_NEG`,
  comparisons and logical operators work unchanged on raw values.
- **Integer constant expressions**: array sizes, `case` labels, bitfield-free `enum` values, `#if`
  (the preprocessor has its own evaluator and doesn't see floats) and `_Alignas` must have integer
  type. `const_expr` reports `integer constant expression required` when the result type is floating.
  `(int)2.5` is still allowed, as in C.
- **Global initialisers** go through `eval2`, so `float g = 1.5;` stores 98304 and
  `int n = 2.7;` stores 2. Nothing else changes in `write_gvar_data`.
- **Codegen**: `fold()` gets the same `fix_mul`/`fix_div` cases. `const_int()` currently refuses any
  cast to a non-integer type. It should fold integer↔floating casts with `fix_from_int`/`fix_to_int`
  and see through casts between floating types.

---

## 5. Code Generation

### 5.1 Free operations

These need no new code once the types are right. Each one is already the correct instruction on the
raw values:

- `ND_NUM` with a floating type → `imm(node->val)`, which goes through the constant pool as usual.
- `+`, `-`, unary `-`, `cmp`-based comparisons, `gen_cond`, `gen_truth`, `?:`, assignment, struct
  copies, `rdi`/`sti` through `float *`, argument passing and slot-13 returns.
- `x++` on a float: the parser builds `x = x + (float)1`, which folds to `x + 65536`, so the existing
  code emits `add x x __k65536`. (`inc` only applies when the raw constant is 1.)

Two spots need a check so they stay correct:

- `is_zero_or_one()` must return false for a floating comparison result that is cast back to float:
  `(float)(a < b)` is 0 or 65536, not 0 or 1.
- The `ND_CAST` case in `gen_expr` assumes every scalar cast is a no-op ("All other scalar types are
  32-bit integers"). It gets the conversions from §3.4.

### 5.2 Multiplication

`a * b` on floats is lowered by the first rule that matches. After §3.4, an integer operand always
appears as a cast `(float)e`, and constants are raw values `C`.

| Case | Lowering | Primitive ops (with the constant pool) |
|---|---|---|
| both constant | folded with `fix_mul` | 0 |
| `a * (float)e` | `mul d a e`, because `a · (e·F) / F = a · e` exactly | 1 |
| `C = n·F` (integer-valued, e.g. `2.0`) | `mul d a n` (or nothing for 1.0, `sub d 0 a` for −1.0) | 1 |
| `C = 2^k`, `k < 16` (e.g. `0.5`, `0.25`) | `div d a 2^(16−k)` | 1 |
| any other constant `C` | inline split (below) | 6–8 |
| otherwise | call `__g2_fmul(a, b)` | call + ~16 |

**Inline split for a constant `C`.** Write `C = n·F + f` with `f ∈ [−32768, 32768)` (round `C / F` to
the nearest integer for `n`). Split `a` into `ah = a / F` and `al = a % F`, so that `0 ≤ al < F`. Then

```
floor(a · C / F) = a·n + ah·f + floor(al·f / F)
```

`|al · f| < 65536 · 32768 = 2³¹`, so nothing overflows except the result itself. Since g2 arithmetic
wraps, the sum is exact modulo 2³², which is the same as `fix_mul`:

```g2
; y = x * 1.3   (C = 85197 = 1·65536 + 19661)
div t0 x __k65536        ; ah
mod t1 x __k65536        ; al
mul t0 t0 __k19661       ; ah·f
mul t1 t1 __k19661       ; al·f
div t1 t1 __k65536       ; floor(al·f / F)
add t0 t0 t1
add y x t0               ; + a·n, with n == 1
```

### 5.3 Division

| Case | Lowering | Primitive ops |
|---|---|---|
| both constant | folded with `fix_div` | 0 |
| `a / (float)e` | `div d a e`, because `a · F / (e·F) = a / e` exactly | 1 |
| `C = n·F` (e.g. `/ 2.0`, or `/ 2` after conversion) | `div d a n` | 1 |
| `C = 2^k`, `k < 16` (e.g. `/ 0.5`) | `mul d a 2^(16−k)` | 1 |
| constant divisor of 0 | error `division by zero`, as for integers | |
| otherwise | call `__g2_fdiv(a, b)` | call + ~10 (fast path) to ~110 |

Other constant divisors are *not* rewritten as multiplication by the reciprocal, because the result
would be off by one unit for some inputs, and folded code would then disagree with runtime code.

### 5.4 Where the lowering happens

This reuses the bitwise design (§6.6 of the port plan). Generalise `lower_bitwise` into
`lower_helpers`, which also rewrites floating `ND_MUL`/`ND_DIV` nodes that match no inline rule into
`ND_FUNCALL`s to `__g2_fmul`/`__g2_fdiv`, and marks the helper live before frames and the call graph
are built. The "does this need a helper?" decision (`fixed_needs_helper`) sits next to the code that
emits the inline forms, like `bitwise_needs_helper`. Compound assignments (`x *= y`) go through the
same path, as bitwise compound assignments already do.

The helpers are leaf functions, so a float operation adds one level to the call depth check, which is
already counted because the lowered call is a normal call edge.

### 5.5 Cost summary

| Operation | Integer cost | Float cost |
|---|---|---|
| `a + b`, `a - b`, `a < b`, `a == b`, `if (a)` | 1–3 | the same |
| `a * 2.0`, `a * n`, `a / 4`, `a * 0.5` | 1 | 1 |
| `a * 1.3` | 1 | 6–8 |
| `a * b` | 1 | ~20 including the call |
| `a / b` | 1 | ~15 typically, ~110 when `|a % b| ≥ 0.5` (§6.2) |
| `(float)n`, `(int)f` | 0 | 1 |

---

## 6. Runtime Helpers (`include/__g2rt.h`)

Written in the C subset, in the style of the bitwise helpers, and always included. They are emitted
only when a program needs them.

### 6.1 `__g2_fmul`

```c
// Q16.16 multiply: floor(a * b / 65536), wrapped to 32 bits. The product
// needs 64 bits, so both operands are split into a signed high half and an
// unsigned low half (x / 65536 and x % 65536, exact because g2 division
// floors). The low × low product is up to 2^32, so `al` is split once
// more into bytes.
static int __g2_fmul(int a, int b) {
  int ah = a / 65536;
  int al = a % 65536;
  int bh = b / 65536;
  int bl = b % 65536;
  int lo1 = al / 256 * bl;          // < 2^24
  int lo0 = al % 256 * bl;          // < 2^24
  int lo = (lo1 + lo0 / 256) / 256; // floor(al * bl / 65536)
  return ah * bh * 65536 + ah * bl + al * bh + lo;
}
```

Why it's exact: `a·b/F = ah·bh·F + ah·bl + al·bh + al·bl/F`, and the first three terms are integers,
so the floor only applies to the last one. `floor((256·lo1 + lo0) / F) = floor((lo1 + floor(lo0/256))
/ 256)`. Every term is exact modulo 2³², so the sum is too.

Possible early exits, to measure before adding: `al == 0` (the left operand is a whole number) skips
the `lo` computation, and `ah == 0 && bh == 0` skips the first three terms.

### 6.2 `__g2_fdiv`

```c
// Q16.16 divide: floor(a * 65536 / b), wrapped to 32 bits. Start from the
// integer quotient and remainder (r has the sign of b and |r| < |b|), then
// produce the 16 fraction bits. When r * 2^k fits in 32 bits, the
// remaining k bits come from one division. Otherwise they come one at a
// time, comparing r with b - r instead of computing 2r, which could
// overflow.
static int __g2_fdiv(int a, int b) {
  int q = a / b;
  int r = a % b;
  int scale = 65536; // 2^k, where k is the number of fraction bits still to produce
  int limit = 32768; // 2^(31 - k): below this, r * scale fits in 32 bits
  while (scale > 1) {
    if (r > -limit && r < limit)
      return q * scale + r * scale / b;
    int d = b - r;
    q *= 2;
    if (b > 0 ? r >= d : r <= d) {
      r -= d; // 2r - b
      q++;
    } else {
      r += r;
    }
    scale /= 2;
    limit *= 2; // wraps on the last step, but is no longer read
  }
  return q;
}
```

The invariant is `a·F/b = q·2^k + r·2^k/b` with `r/b ∈ [0, 1)`:

- **Fast path:** if `|r| < 2^(31−k)`, then `r · 2^k` fits, and the answer is
  `q · 2^k + floor(r · 2^k / b)`. At the start (`k = 16`) that holds whenever `|r| < 32768`, i.e. the
  remainder is below 0.5 in fixed point, which covers most divisions by small values.
- **Bit step:** for `b > 0`, `2r ≥ b ⇔ r ≥ b − r`, and the new remainder `2r − b = r − (b − r)`
  never overflows. For `b < 0` the comparison flips.
- Division by zero does whatever the VM's `div` does with a zero divisor, the same as integer `/`.
  `INT_MIN / -1` (raw) likewise behaves like the integer case. The tests pin both down.

The loop ends as soon as the remainder is small enough for the fast path, so the slow case is rare:
it needs a large remainder, which in turn needs a large divisor.

---

## 7. Headers and Library

### 7.1 `include/float.h` (new)

```c
#define FLT_RADIX 2
#define FLT_EPSILON (1.0f / 65536)     // smallest positive value
#define FLT_MIN FLT_EPSILON            // there are no subnormals, so it's the same
#define FLT_MAX 32767.99998474121f     // raw 0x7FFFFFFF
#define FLT_DIG 4
// DBL_* and LDBL_* are the same values.
```

`preprocess.c` also defines `__G2_FIXED_FRAC_BITS__ 16`, so code can check for the format.

### 7.2 `include/math.h` (new)

Header-only and `static`, like `g2lib.h`, so unused functions cost nothing. `double` names (`sqrt`,
`floor`, …) are `#define`d to the `float` versions, since the types are the same.

| Function | Implementation |
|---|---|
| `fabsf`, `fminf`, `fmaxf`, `copysignf` | raw integer operations |
| `floorf`, `ceilf`, `truncf`, `roundf`, `fmodf` | raw `div`/`mod` by 65536 (`floorf` is one `div` and one `mul`) |
| `sqrtf` | bit-by-bit integer square root of `raw · 2^16`, a 48-bit value. Use the 32-bit "shift two bits at a time" algorithm, adapted to signed slots: the root of a non-negative Q16.16 value is at most `181 · F < 2^24`, so the working values fit. Negative input returns 0. |
| `sinf`, `cosf` | reduce to `[−π/2, π/2]`, then an odd 5th-order polynomial evaluated with inline constant multiplies (§5.2). No table, so no memory or start-up cost. Accuracy target about 1e-4, which is close to the format's resolution. |
| `atan2f` | polynomial approximation on the octant. Useful for aiming. Lower priority. |
| `hypotf(x, y)` | scales both by a power of two so the larger magnitude is in [64, 128), where `x*x + y*y` neither overflows nor loses bits, then scales back |
| `float_to_str(dest, x, decimals)` | in `g2lib.h`, next to `int_to_str`. Writes a length-prefixed string that `draw_text` can print. |

Each library function is checked against the host's `libm` (rounded to Q16.16) with a tolerance, and
the exact functions (`floorf`, `fabsf`, …) are checked for equality.

---

## 8. Diagnostics

New errors:

```
game.c:5:13: floating constant 40000.0 is out of range for g2 fixed point (-32768 to 32767.99998)
game.c:9:11: invalid operands to binary %          (or &, |, ^, <<, >>, ~ with a floating operand)
game.c:12:7: array subscript is not an integer
game.c:14:3: switch quantity is not an integer
game.c:20:9: cannot convert between a pointer and a floating type
game.c:22:7: integer constant expression required
```

New warnings:

```
game.c:5:13: floating constant 1e-6 rounds to 0
game.c:7:15: conversion of 50000 to float wraps around (g2 floats range from -32768 to 32767.99998)
```

Remove the current `floating point is not supported on g2` and `floating-point constants are not
supported on g2` errors, and the "Floating point" entry in the port plan's non-goals.

---

## 9. Testing

1. **`test-g2/float.c`**, in the usual `ASSERT` style: literals, conversions both ways (including
   negatives, which floor), mixed int/float arithmetic, comparisons, `op=`, `++`, float arrays and
   struct members, pointers to floats, arguments and return values, and global initialisers. Tests
   check raw values through a helper in `test.h`:
   ```c
   static int fbits(float f) { return *(int *)&f; }   // the raw Q16.16 value
   ```
2. **Folding vs. runtime.** Each operation runs once on constants (folded by the compiler) and once
   on values passed through `id()` (computed by the VM), and both must equal the value computed by
   the host. This catches any disagreement between `fix_mul` and `__g2_fmul`.
3. **A generated helper test**, like the bitwise one: a script computes `fix_mul`/`fix_div` on the
   host for edge values (0, ±1 raw, ±65535, ±65536, ±32768, `INT_MIN`, `INT_MAX`, values with
   all-ones low halves) and a few hundred random pairs, and emits assertions. It covers every inline
   rule in §5.2 and §5.3 as well as the helpers, including the `fdiv` fast path and the bit loop.
4. **Error tests** in `test-g2/errors/` for each diagnostic in §8.
5. **Library tests** against host `libm` (§7.2).
6. `make test` runs all of it, also with `-fno-peephole`, `-fno-const-pool` and `-fno-overlay`.

---

## 10. Phases

### Phase F1: Types, literals and folding

1. `tokenize.c`: floating literals → raw Q16.16 values with the range error and the rounds-to-0 warning.
2. `parse.c`: accept `float`/`double`/`long double` in `declspec`; `ND_NUM` takes the literal's type;
   the operator and conversion errors from §3.3 and §3.4.
3. `type.c`: `get_common_type` returns a floating type when either side is floating.
4. `fix_*` functions, and floating cases in `eval2` and `const_expr` (§4).
5. Codegen: `ND_CAST` conversions, `const_int`/`fold` for floats. At this point `* /` on floats are
   rejected by codegen with `unsupported`.

*Milestone:* a program that moves a pixel with a `float` position and velocity (only `+`, `-`,
comparisons and `(int)` casts), running on `cg2`.

*Status:* done. The decisions in §11 were taken as recommended. Notes:

- Global initialisers are now converted to the variable's type before folding (`int n = 2.7;` stores
  2). Before this, an integer initialiser never needed a conversion.
- Like `-2147483648` for integers, `-32768.0` can't be written as a literal: the literal `32768.0` is
  out of range before it's negated.
- `test-g2/float.c` covers literals, conversions, `+ -`, comparisons, `op=`/`++`, arrays, structs,
  pointers, globals and folded `* /`. There are nine new error tests. `make test` passes with every
  combination of `-fno-peephole`, `-fno-const-pool` and `-fno-overlay`.
- The milestone program (a pixel that accelerates with the arrow keys and slows down by friction)
  matches exact rational arithmetic in the headless VM. `(int)x` compiles to one `div`, and
  `x >= WIDTH` to a single compare with 63.99998.

### Phase F2: Multiplication and division

1. `__g2_fmul` and `__g2_fdiv` in `__g2rt.h`.
2. `lower_bitwise` → `lower_helpers`, with the inline rules of §5.2 and §5.3.
3. The generated helper test and the folding-vs-runtime test.

*Milestone:* a bouncing ball with gravity and damping (`vy += 0.15; vy *= 0.9;`), compared against a
host build of the same physics with the same fixed-point rules.

*Status:* done. Notes:

- The rule for converted integers (§3.3) came out of writing the tests. Before it, `70000 * 0.5`
  folded to a different value than `id(70000) * 0.5` computed at run time.
- `test-g2/fixed.c` has 2341 assertions generated by `tools/gen_fixed_test.py`. It covers both
  helpers (including `__g2_fdiv`'s bit-by-bit path), every inline form with constants at the edges of
  the split, integer operands, and folding, all against exact results computed in Python. The
  generator is checked in, unlike the bitwise one.
- Division by zero and raw `INT_MIN / -1` behave like integer division: `g2run` reports the first,
  and `cg2` doesn't check either (both are undefined in its C code). The tests skip them.
- The milestone ball (gravity, drag through `__g2_fmul`, a damped bounce through `__g2_fdiv`)
  matches a Python model exactly over 400 frames, at about 144 instructions per frame.
- `n = n * 0.75` with an `int n` compiles to `(n * 49152) / 65536`, with no conversion at all.

### Phase F3: Library

1. `float.h`, `__G2_FIXED_FRAC_BITS__`.
2. `math.h`: the exact functions first, then `sqrtf`, `sinf`/`cosf`, `hypotf`, then `atan2f`.
3. `float_to_str` in `g2lib.h`.

*Milestone:* a rotating line drawn with `sinf`/`cosf`, plus an on-screen readout using
`float_to_str` and `draw_text`.

*Status:* done. Notes:

- `math.h` has `fabsf`, `fminf`, `fmaxf`, `copysignf`, `floorf`, `ceilf`, `truncf`, `roundf`, `fmodf`,
  `sqrtf`, `hypotf`, `sinf`, `cosf`, `tanf`, `atan2f`, `atanf`, the `M_*` constants, and macros for the
  `double` names. `float.h` and `__G2_FIXED_FRAC_BITS__` are as planned.
- `__g2rt.h` gained two macros used by the library: `__G2_FBITS(v)` (the raw bits of a float
  lvalue) and `__G2_RAW(n)` (the float with raw value `n`, which folds through the converted-integer
  rule of §3.3).
- The polynomial coefficients were fitted by simulating the exact fixed-point evaluation and then
  searching nearby raw values. Measured in the VM: `sinf`/`cosf` within 5 raw units (7.6e-5),
  including angles near ±32768; `atan2f` within 3. `sqrtf` and the other exact functions match
  exactly. `hypotf` is within 1 unit up to 128 and about 2.5e-7 relative above that. (The plan's
  first version of `hypotf`, scaling by the larger value, was off by up to 0.54 near the top of the
  range.)
- Cost per call, in executed instructions: `floorf` 11, `sinf`/`cosf` ~180, `atan2f` ~280,
  `sqrtf` ~400–470, `hypotf` ~640. `sqrtf` is the obvious target for Phase F4.
- `test-g2/math.c` (704 assertions, generated by `tools/gen_math_test.py`) checks every function,
  including `float_to_str`. `test.h` gained `ASSERT_NEAR`.
- The milestone (a line rotating about the centre with its angle printed above) runs in the
  headless VM with the expected readout and endpoint, at about 4960 instructions per frame, mostly
  `draw_text` and `line`.

### Phase F4: Polish

1. Measure the helpers on the milestone programs and add the early exits from §6.1 if they pay off.
   Speed up `sqrtf` (§7.2), which costs ~400 instructions.
2. Peephole check: make sure `div t x K; mul y t K` (from `floorf` and casts) isn't made worse by the
   existing rewrites, and that the constant pool shares `__k65536`.
3. Update `README.md` (dialect notes: floats are Q16.16, multiplication, division and `(int)` floor)
   and the port plan (§1 non-goals, §15 checklist).

---

## 11. Decisions to Confirm

These are my recommendations. Each is easy to change before Phase F1 starts, and harder afterwards.

1. **Format: Q16.16.** The alternatives are Q20.12 (range ±524288, resolution 1/4096 ≈ 0.00024) and
   Q24.8 (range ±8.4 million, resolution 1/256). A smaller fraction gives more headroom for `x * x`
   and distances but makes slow movement jerky. Q16.16 is the common choice for 32-bit fixed point
   and keeps the helpers simplest.
2. **`(int)f` floors** instead of truncating (§3.4). The alternative costs about six instructions per
   conversion. `-mc-division` can switch to truncation later for strict C.
3. **`*` and `/` round down** instead of to nearest (§3.3). Rounding to nearest costs one more add in
   `fmul`, and a remainder comparison in `fdiv`, and breaks the `x * 0.5 == x / 2` identities.
4. **`double` is accepted as the same type as `float`**, without a warning. A program that uses
   `double` for precision gets Q16.16 silently. A `-Wdouble` warning is cheap to add if you'd prefer
   to be told.

---

## 12. Possible Future Extensions

- **Choosing the format** with `-mfixed=Q<i>.<f>`: the compiler already funnels everything through
  `FIX_ONE` and the `fix_*` functions, and the helpers would read `__G2_FIXED_FRAC_BITS__`. The
  `fmul` byte split depends on the fraction width, so it needs generalising.
- **Saturating arithmetic** as an opt-in (`-mfixed-saturate`), which clamps on overflow instead of
  wrapping. It costs a compare on every add.
- **Lookup tables in `load` data** for `sinf` once the assembler supports `bytes` (§13 of the port
  plan), trading memory for speed.

---

## 13. File Checklist

| File | Change |
|---|---|
| `tokenize.c` | Floating literals → raw Q16.16, range error, rounds-to-0 warning. |
| `parse.c` | Accept floating types; `ND_NUM` keeps the literal's type; operator/subscript/switch/pointer errors; `fix_*` functions; floating cases in `eval2` and `const_expr`. |
| `type.c` | `get_common_type` for floating types. |
| `chibicc.h` | Export `fix_from_int`, `fix_to_int`, `fix_mul`, `fix_div`, `FIX_ONE`. |
| `preprocess.c` | `__G2_FIXED_FRAC_BITS__`. |
| `codegen_g2.c` | `ND_CAST` conversions; float-aware `const_int`/`fold`/`is_zero_or_one`; inline `*` and `/` rules; `lower_bitwise` → `lower_helpers`. |
| `include/__g2rt.h` | `__g2_fmul`, `__g2_fdiv`. |
| `include/float.h`, `include/math.h` | New. |
| `include/g2lib.h` | `float_to_str`. |
| `test-g2/` | `float.c`, the generated helper test, library tests, error tests; `fbits` in `test.h`. |
| `README.md`, `g2-port-plan.md` | Dialect notes; remove floats from the non-goals. |
