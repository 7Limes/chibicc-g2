# Plan: Retargeting chibicc to g2 Assembly

This plan covers turning this chibicc fork into a compiler that turns a subset of C into readable
g2 assembly (the `g2asm` text format). It does not produce `.g2b` binaries; the g2 assembler does that.

References: `g2asm-spec.md`, `g2-spec.md`, `example/snake_g1.c`, `example/g2.h`.

---

## 1. Goals and Non-Goals

### Goals

- Compile one C translation unit (plus headers) into one `.g2asm` file that the g2 assembler accepts.
- Support the C subset that `example/snake_g1.c` uses, plus the obvious neighbours: integers, pointers,
  arrays, functions, the usual control flow, globals, and string/char literals.
- Expose g2 features in C: the drawing instructions (`col`, `pix`), reserved memory (keys, delta time),
  the `start`/`tick` hooks (run through a compiler-generated entry stub), window metadata, and `load`
  data statements.
- Keep the output readable: symbolic names for variables, a comment with the source line for each
  statement, and no wasted copies in the common cases.
- Reject unsupported features with a clear error, not wrong code.

### Non-Goals (for now)

- Floating point (`float`, `double`, `long double`).
- Unsigned arithmetic semantics (see §3.4).
- Recursion (not needed, by design; see §3.2).
- Function pointers (possible later; see §12).
- Variadic functions, VLAs, `alloca`, inline asm, atomics, TLS, bitfields.
- Struct pass-by-value and return-by-value (structs themselves are a later phase).
- Linking several `.c` files. Everything is one translation unit.
- Self-hosting (chibicc compiling itself). This goes away once the x86 backend is removed.

---

## 2. What g2 Gives Us (Constraints That Shape the Design)

| Fact | Consequence for the compiler |
|---|---|
| Memory is an array of signed 32-bit integers, and each element is a *slot*; there is no byte addressing | The smallest addressable unit is one slot. In this compiler, `sizeof(int) == sizeof(char) == sizeof(T*) == 1`. |
| There are no registers; every operand is a memory address | Every variable and temporary lives at a fixed memory address. Instructions can use variables directly as operands, so there is no load/store step for plain variables. |
| `ldi` is the only instruction that takes an immediate (pseudo-ops add `addi`/`subi`/`muli`/`divi`/`modi`) | A constant operand needs `ldi` into a temp, or a pseudo-op with an immediate, or one of the reserved constant slots `0`=0, `1`=1, `2`=-1. |
| Indirection is only possible through `rdi` (load) and `sti` (store) | Pointer dereference becomes `rdi` or `sti` through a slot that holds the address. |
| `cmp` returns 1/0/-1, and the only conditional jump is `jne` (`jnea` pseudo) | Comparisons need a small lowering table (§6.3). |
| `div` rounds toward -∞ and `mod` takes the sign of the divisor | This differs from C, which truncates toward zero. We adopt g2's semantics and document it (§3.5). Constant folding has to use the same rules. |
| There are no bitwise instructions | `~x` is `-1 - x` (free). Shifts and masks by constants map to `mul`/`div`/`mod`. Bitwise ops on non-constant values call runtime helpers (§6.6). |
| The call stack lives in slots 15–31 (16 entries), and `call`/`ret` are pseudo-ops | Call depth is limited to about 16. The compiler checks this statically (§5.4). Locals cannot go on that stack, so they need another home. |
| Slot 13 is the "return register" and slot 14 is pseudo-op scratch | Return values go in slot 13. The compiler never uses slot 14 itself. |
| Slots 0–31 are reserved | Compiler-allocated memory starts at 32. |
| Execution: there is one entry point, the first instruction. The whole program runs from there every frame (60 times per second), and the frame ends when execution goes past the last instruction | The compiler emits an **entry stub** as the first instructions. It calls `start()` once on the first frame (tracked by a flag in memory, which starts zeroed) and calls `tick()` every frame, then jumps to `__g2_end:`, the last line of the file (§6.1). |

---

## 3. Key Design Decisions

### 3.1 Memory model: sizes are measured in slots

**Terminology:** throughout this plan, a *slot* is one element of g2's memory array, that is, one signed
32-bit integer. Slot `n` is the integer at address `n`.

chibicc measures every size and offset in bytes. The simplest correct port **redefines chibicc's size
unit to be one slot**. Every `size`, `align` and `offset` that chibicc computes is then a count of
slots, not bytes. There are no 8-bit bytes anywhere in the target. In practice:

- `sizeof(char) == sizeof(short) == sizeof(int) == sizeof(long) == sizeof(long long) == sizeof(void*) == 1`.
- All alignments are 1.
- `CHAR_BIT == 32`. The C standard allows this; DSP compilers do the same thing.
- Pointer arithmetic needs no scaling for scalars. Arrays and structs still scale by element size in slots.

This keeps most of `parse.c` and `type.c` as they are. Struct layout, array indexing, `sizeof`, and
initializers all keep working because they only ever see sizes in "units".

### 3.2 Static frame allocation instead of a runtime stack

g2 has no stack pointer for data, and every access through a pointer costs an `rdi`/`sti` plus address
arithmetic. A software stack would make every local variable access cost 2–4 instructions.

Instead, **each function's parameters, locals and temporaries get fixed memory addresses**. This is the
"compiled stack" model used by 8051/PIC compilers. Then:

- A local is used directly as an instruction operand: `add move_snake__i move_snake__i 1`.
- Taking `&local` is a constant: `ldi t move_snake__seg_x`. This works because the address never changes.
- Calling a function means copying arguments into the callee's parameter slots and then `call`.
- **Recursion is not supported, and it is not a requirement.** The compiler builds the call graph and
  reports an error for any cycle (§5.4).
- Memory can be saved later by **overlaying** frames of functions that are never active at the same
  time (§5.3). Phase 1 just gives every function its own disjoint frame.

### 3.3 Operand-based code generation (replacing chibicc's stack machine)

The x86 backend (`codegen.c`) works like a stack machine: every expression ends in `%rax`, with push/pop
for binary operators. That is a poor match for a memory-to-memory ISA. The new backend works like this:

```c
// Where a value lives.
typedef struct {
  OpKind kind;   // OP_SLOT (a memory address/symbol) or OP_IMM (compile-time constant)
  char *sym;     // symbolic name, e.g. "g_head_x", "move_snake__t3", "13"
  int addr;      // numeric address (always known: the compiler allocates everything)
  int imm;       // value for OP_IMM
} Operand;

Operand gen_expr(Node *node, Operand *dest_hint);   // evaluates node, returns where the value is
LValue  gen_lval(Node *node);                        // see below
```

- If the expression is a variable, `gen_expr` emits **nothing** and returns that variable's slot.
- If it is a constant, `gen_expr` returns `OP_IMM`. The consumer decides how to use it: an immediate
  pseudo-op (`addi`), a reserved constant slot (`0`, `1`, `2`), or `ldi` into a temp.
- `dest_hint` lets the **outermost** operation write straight into its final destination, so
  `x = a + b` is one `add x a b` and not `add t a b; cpy x t`. Subexpressions always go to fresh temps,
  so aliasing cannot go wrong. (Every g2 op reads its operands before writing its destination. Pseudo-ops
  only use slot 14 as scratch, so this holds for them as well; see §13.)
- L-values come in two kinds:
  - `LV_DIRECT(addr)`: the address is known at compile time (a global, a local, `arr[CONST]`,
    `s.member`). Reads and writes use the address directly.
  - `LV_INDIRECT(slot)`: the address is computed at runtime and sits in a slot (`*p`, `arr[i]`, `p->m`).
    Reads use `rdi`, writes use `sti`.

### 3.4 Integer types

- `int`, `char`, `short`, `long`, `long long` and `_Bool` are all 32-bit signed and one slot each.
  Casts between them are no-ops, except casts to `_Bool`, which normalise to 0/1.
- `unsigned` gives an **error** in phase 1. g2's `cmp`, `div` and `mod` are signed, and unsigned
  semantics would need costly emulation. If you want it later, a "treat as signed with a warning" mode
  is easy to add.
- Integer literals are `int`. Literals in `0x80000000..0xFFFFFFFF` wrap to negative with a warning.
  Literals wider than 32 bits are an error.
- `enum` works without changes.

### 3.5 Division and modulo semantics

`/` and `%` compile straight to `div`/`mod`, so they **floor** instead of truncating. For example,
`-1 % 64 == 63` on g2, where C gives `-1`. The snake example actually relies on this for screen
wrap-around (`(head_x + dir_x) % WIDTH` with `dir_x == -1`). This is a documented dialect difference.
**chibicc's constant folder (`eval2` in `parse.c`) must use the same floor rules and wrap to 32 bits**,
or folded and unfolded code would disagree.

An optional `-mc-division` flag could emit truncating sequences for strict C. This is low priority.

### 3.6 Exposing g2 features to C

| Feature | C surface | Mechanism |
|---|---|---|
| Program hooks | `int start()` / `int tick()` (`void` also allowed) | Recognised by name and compiled as **ordinary functions** (`fn_start`, `fn_tick`, ending in `ret`). The compiler-generated entry stub at instruction 0 calls them (§6.1). Calling them from C is an error. |
| Draw instructions | `void color(int r, int g, int b);` `void pix(int x, int y);` | New `__attribute__((g2_intrinsic("col")))` on the declaration. Calls emit the instruction directly with the arguments as operands. |
| Reserved memory | `UP`, `DOWN`, … `DELTA` | New `__attribute__((address(N)))` on a global declaration. The global is bound to a fixed address and not allocated. Declare them `const` so assignments are rejected. |
| Window size | `#define WIDTH 64` / `#define HEIGHT 64`, placed **before** `#include "g2.h"` | After preprocessing, the compiler evaluates the final values of the `WIDTH` and `HEIGHT` macros as constant expressions and emits `#width`/`#height` (§3.7). There are no separate CLI flags; use `-DWIDTH=N`. |
| Memory size | (none) | `#memory` is set to the end of the compiler's own allocation (§5.1). The assembler appends `load` data after it. `-mmemory=N` overrides it, and an error is reported if N is too small. |
| Data loading | `load FONT pack file "code8x8.font";` | New top-level declaration (§4.4). `FONT` acts like `extern int FONT[]`, and the statement is copied into the output as-is. |

Updated `example/g2.h` using these attributes:

```c
#ifndef G2_H
#define G2_H

// Default window size. A program overrides it by defining WIDTH/HEIGHT
// *before* including this header (or with -DWIDTH=N on the command line).
#ifndef WIDTH
#define WIDTH 100
#endif
#ifndef HEIGHT
#define HEIGHT 100
#endif

void color(int r, int g, int b) __attribute__((g2_intrinsic("col")));
void pix(int x, int y)          __attribute__((g2_intrinsic("pix")));

extern const int CONTROL1 __attribute__((address(3)));
extern const int CONTROL2 __attribute__((address(4)));
extern const int A        __attribute__((address(5)));
extern const int B        __attribute__((address(6)));
extern const int UP       __attribute__((address(7)));
extern const int DOWN     __attribute__((address(8)));
extern const int LEFT     __attribute__((address(9)));
extern const int RIGHT    __attribute__((address(10)));
extern const int DELTA    __attribute__((address(11)));

// Standard library (static so unused functions are removed; see §8)
static void hexcolor(int c);
static void seed_rand(int seed);
static int  next_rand(void);
static void int_to_str(int *buf, int n);
static void draw_text(int *str, int *font, int size, int x, int y);
#include "g2lib.c"   // or put the definitions inline here

#endif
```

> If you would rather keep `g2.h` exactly as it is now, the fallback is a table built into the compiler
> that maps the names `color`/`pix` and `UP`…`DELTA` to intrinsics and addresses. It needs less work but
> is less flexible. I recommend the attribute approach. It is a small change in the parser, since
> attribute parsing already exists for structs.

### 3.7 Window size

**What was decided:** the window size comes from the `WIDTH` and `HEIGHT` macros. One definition sets
the window **and** is the value the program's code uses (`% WIDTH`, bounds checks …), so the two can
never disagree.

**Rules:**

1. **Defaults sit behind guards in `g2.h`** (`#ifndef WIDTH` / `#define WIDTH 100`). A program sets
   its size by defining the macros *before* `#include "g2.h"`:
   ```c
   #define WIDTH 64
   #define HEIGHT 64
   #include "g2.h"
   ```
   The same works from the command line: `chibicc-g2 -DWIDTH=128 -DHEIGHT=96 game.c`.
2. **The compiler reads the final values after the whole file is preprocessed** and evaluates each
   one as a constant expression, using the code `#if` uses. So `#define WIDTH (TILE * 8)` works.
3. **No `-mwidth`/`-mheight` flags.** A flag that changed only `#width` would let the window and the
   code disagree, which is exactly the problem this design avoids.
4. **Redefinitions warn** (§4.2). If anything redefines `WIDTH`/`HEIGHT` with a different value
   after it was first defined, the compiler prints a warning that points at both definitions, so a
   silent change to the window cannot happen.
5. **`#memory` is not part of this.** The compiler always computes it. `-mmemory=N` is only for
   reserving extra space, and asking for less than the program needs is an error.

**Errors** (window sizes of 0 are rejected by the VM at load time anyway, so they are caught at
compile time instead):

```
error: WIDTH is not defined (define it before including g2.h, or pass -DWIDTH=N)
error: WIDTH must be an object-like macro
error: WIDTH does not evaluate to an integer constant: 'TILE * cols'
error: WIDTH must be at least 1 (got 0)
warning: 'WIDTH' redefined
  game.c:3: previous definition was here
```

In practice the first error only happens without `g2.h`, because the header always provides defaults.

---

## 4. Front-End Changes (by file)

### 4.1 `main.c`: driver

- Remove the assembler and linker steps (`assemble`, `run_linker`, `find_libpath`, `find_gcc_libpath`,
  `-c`, the `.o` and `.a` handling). The only outputs are `.g2asm`, or preprocessed text with `-E`.
- Default output: `foo.c` → `foo.g2asm`. Keep `-o`, `-E`, `-I`, `-D`, `-U`, `-include`, `-M*`,
  `-fcommon` (harmless), `-###`, and `-cc1` (or drop the cc1 subprocess split and compile in-process,
  which is simpler).
- New flags: `-mmemory=N` to reserve extra memory beyond what the compiler needs, and `-fno-overlay`
  to turn off frame overlay (§5.3). Optionally `-mc-division`. There are **no** `-mwidth`/`-mheight`
  flags. `-DWIDTH=N` / `-DHEIGHT=N` already do the job and keep the code's view of the size in sync
  with the window (§3.7).
- After `preprocess()`, call `window_dimension("WIDTH")` and `window_dimension("HEIGHT")` (§4.2), and
  pass the results to codegen for the `#width`/`#height` lines.
- `add_default_include_paths`: **remove** `/usr/include`, `/usr/local/include` and the GCC paths.
  Keep only `<exe dir>/include` (see §4.6) and `-I` paths, so `#include <stdio.h>` cannot pull in glibc.
- Remove the "multiple input files" loop, or give an error for more than one `.c` input.

### 4.2 `preprocess.c`

- `init_macros()`: remove `__x86_64__`, `__amd64__`, `__linux__`, `__unix__`, `__ELF__`, `_LP64`,
  `__LP64__`, and the float/double size macros. Add `__g2__ 1`, `__CHAR_BIT__ 32`,
  `__SIZEOF_INT__ 1`, `__SIZEOF_POINTER__ 1`, `__SIZEOF_LONG__ 1`, `__SIZEOF_SHORT__ 1`,
  `__SIZEOF_LONG_LONG__ 1`, `__SIZE_TYPE__ int`, `__PTRDIFF_TYPE__ int`, `__INT_MAX__ 2147483647`,
  and `__STDC_HOSTED__ 0`.
- Add `int window_dimension(char *name)`, which runs after the whole translation unit has been
  preprocessed (§3.7):
  1. Look up `name` in the `macros` hashmap. If it is missing, report the "not defined" error. If it is
     a function-like macro, report an error too.
  2. Expand it and evaluate the result with the same machinery `#if` uses: `read_const_expr`
     / `eval_const_expr`, factored so it can run on a token list built from just the macro name. That
     way `#define WIDTH (8 * 8)` and `#define WIDTH (TILE * COLS)` both work.
  3. If the value is not ≥ 1 or does not fit in 32 bits, report an error.
- **Macro redefinition warning.** chibicc's `add_macro` currently replaces an existing macro without
  saying anything. Add the standard C check: if an object-like or function-like macro is redefined
  with a different replacement list (comparing token spelling and spacing, but not source locations),
  print `warning: 'WIDTH' redefined` with the location of the earlier definition. Identical
  redefinitions stay silent, as the C standard requires. This catches the "last definition silently
  wins" problem for the window size and for every other macro.

### 4.3 `tokenize.c`

- Remove the float literal path. A floating literal becomes an error ("floating point not supported on g2").
- `convert_pp_int`: give every literal type `int`, warn on wrap-around in `[2^31, 2^32)`, and error above
  that. Drop the `u`/`U` suffix, or make it an error, to match §3.4.
- Character literals: `'a'` is an `int` with value 97. Multi-character and wide literals are errors.
- String literals: each `char` is one slot. `u8"…"`, `u"…"`, `U"…"` and `L"…"` still produce one slot
  per code unit, which is fine and needs no change. Keep the original spelling (`tok->loc`, `tok->len`)
  available for `load` statements (§4.4).
- `load` is **not** a keyword. It is recognised by context in the parser so it stays usable as an
  identifier elsewhere.

### 4.4 `parse.c`

**Type and feature restrictions** (errors with a source location):

- `float`, `double`, `long double`: error in `declspec`.
- `unsigned`: error in `declspec` (phase 1).
- `_Thread_local`, `_Atomic`, `asm`, VLAs, `alloca`, `__builtin_va_*`, variadic function definitions,
  bitfields, `goto *expr` and `&&label`, and struct/union parameters or return values: errors.
  - Calls to a variadic *declaration* are also an error, because there is no way to pass varargs.
- Function pointers: error on calling through a non-function expression (`ND_FUNCALL` whose
  `lhs` is not a direct `ND_VAR` of a function) and on taking a function's address, for now.

**Remove x86 assumptions:**

- `function()`: stop creating `__alloca_size__` (`fn->alloca_bottom`) and `__va_area__`. Remove the
  hidden struct-return buffer parameter, since struct returns are rejected.
- `__func__` / `__FUNCTION__`: create the string literal **lazily** on first use. Right now every
  function gets an anonymous global char array holding its name. Each one would cost memory and init
  instructions on g2.
- `declare_builtin_functions()`: remove `alloca`.
- `new_add`/`new_sub`: these already scale by `ty->base->size`. With size 1 that becomes `* 1`, which
  codegen folds away. No change is needed, but checking `size == 1` here keeps the AST cleaner.

**Constant evaluation (`eval`, `eval2`):**

- Wrap every result to `int32_t`.
- `/` and `%` follow g2 floor semantics (§3.5).
- `>>` is arithmetic and `<<` wraps. Bitwise ops fold normally at compile time, which is fine because
  results are 32-bit two's complement either way.
- Address constants (`&global + k`): the existing relocation machinery (`eval2` with `label`) still
  works. Initializers become `ldi` of the symbol plus `addi k` (§6.9).

**Global initializer buffer:**

- `write_gvar_data` / `write_buf` / `read_buf` currently write `ty->size` bytes. With size 1 they would
  **truncate every value to 8 bits**. Change `init_data` to an `int32_t *` with one element per slot,
  and replace `write_buf(buf+off, val, sz)` with `buf[off] = (int32_t)val`. Relocation `offset`s are
  already slot indices.

**Attributes:**

- Generalise `attribute_list` so it can attach to an `Obj` as well as a `Type`. Parse
  `__attribute__((...))` after a declarator in `global_variable()` and `function()`, and also in
  `declspec` position, for GNU compatibility.
- New attributes, stored on new `Obj` fields:
  - `address(N)` on a global: sets `var->fixed_addr = N` and `var->has_fixed_addr = true`. The global
    must be `extern`, or at least have no initializer.
  - `g2_intrinsic("col"|"pix")` on a function declaration: sets `fn->intrinsic`. The parameter count
    must match the instruction (3 for `col`, 2 for `pix`). The function must not have a body.

**`load` statement (new top-level form):**

```
load-decl = "load" ident ident ident string-literal ";"
            ; NAME   OPERATION  DATA_TYPE  DATA_STRING
```

- In `parse()`, before `declspec`: if `tok` is the identifier `load`, the next three tokens are
  identifiers, and the fourth is a string literal, parse a load declaration.
- Check the operation against `raw|pack` and the data type against `file|bytes|string`.
- Create a global `Obj`: `name = NAME`, `ty = array_of(ty_int, 0)` (incomplete, so it decays to
  `int *`), `is_load = true`, `is_definition = false`. Also store `load_op`, `load_type`, and
  `load_data`, which is the **raw spelling** of the string token copied from `tok->loc`/`tok->len`
  so the assembler sees exactly what the user wrote.
- Using `FONT` as a value gives its address (`ldi t FONT`). Assigning to it is an error (array type).
- `file` paths: a relative path is relative to the assembler's working directory, which may not be the
  source directory. Add an option `-mload-relative` that rewrites relative paths to be relative to the
  `.c` file. The default is to leave paths unchanged.

**Liveness and roots (reusing `mark_live`/`refs`):**

- chibicc already records which functions each function references (`current_fn->refs`) and marks
  liveness from roots. Change the root rule: **only `start` and `tick` are roots**, because they are the
  functions the entry stub calls. Any function not
  reachable from them is never emitted and gets no frame. This is how the header-only standard library
  stays free when unused.
- Runtime helper calls inserted by lowering (§6.6) must add to `refs` too.
- Unused globals: also drop anonymous string literal globals that nothing references. This needs a
  small reference pass or a `refs`-style flag on gvars. Named user globals are kept.

### 4.5 `type.c` and `chibicc.h`

- Set every scalar type to size 1, align 1: `ty_char`, `ty_short`, `ty_int`, `ty_long`, `ty_bool`
  and their unsigned twins. `pointer_to()` gets size 1, align 1. `ty_void` stays at 1, which only
  matters for GNU `void*` arithmetic.
- Delete `ty_float`, `ty_double`, `ty_ldouble`, or leave them in only so the parser can reject them.
- `add_type()`: drop the "usual arithmetic conversion" paths for floats. `get_common_type` always
  returns `ty_int` for integers and keeps pointers as pointers.
- `struct Obj` gains: `bool has_fixed_addr; int fixed_addr;`, `char *intrinsic;`,
  `bool is_load; char *load_op, *load_type, *load_data;`, and backend fields `int addr;`
  (absolute slot address) and `char *asm_name;` (emitted symbol).
- `struct Obj` loses (or ignores): `offset` (replaced by `addr`), `va_area`, `alloca_bottom`,
  `stack_size` (replaced by `frame_size`), `is_tls`.
- `init_data` becomes `int32_t *`.

### 4.6 `include/`

Replace the x86 `include/` headers (`stdarg.h`, `stdatomic.h`, `float.h`, …) with a small g2 set:

- `stddef.h`: `NULL`, `size_t` (int), `ptrdiff_t` (int), `offsetof`.
- `stdbool.h`: keep.
- `limits.h`: `CHAR_BIT 32`, `INT_MAX`, `INT_MIN`, and `CHAR_MAX = SHRT_MAX = LONG_MAX = INT_MAX`.
- `g2.h`: the hardware interface plus standard library (§3.6, §8). This could move here from `example/`
  so that `#include <g2.h>` works.
- `__g2rt.h`: compiler runtime helpers (bitwise ops). The driver `-include`s it automatically (§6.6).

---

## 5. Memory Layout and Frame Allocation

### 5.1 Layout

```
 0 .. 11   reserved: constants 0/1/-1, keys, delta time       (fixed by VM)
 12        unused (reserved; compiler does not touch)
 13        return register (function return values)
 14        pseudo-instruction scratch (compiler NEVER uses)
 15 .. 31  call stack pointer + call stack                       (fixed by VM)
 32 ..     globals, in declaration order (including anonymous string literals)
 ..        constant pool (optional optimisation, §10)
 ..        function frames (params, locals, temps), disjoint or overlaid
 #memory = end of the frame region             (emitted as `#memory N`)
 N ..      load data, placed by the assembler in declaration order;
           the array grows to N + total data size. The compiler never
           allocates here and refers to it only through load names.
```

### 5.2 Frame contents and naming

For each live function `f`:

1. **Parameters** first, in order. Callers write arguments here.
2. **Locals**, from `fn->locals` minus parameters. Each declared object gets `ty->size` slots. Locals in
   sibling scopes (`for` loop bodies, for example) can share slots later on. Phase 1 gives each its own slot.
3. **Temporaries**. Allocate them with a stack discipline while generating code (`alloc_temp()` /
   `free_temp()`), track the high-water mark, and reserve that many slots. Because the frame size
   depends on temp usage, **generate each function's code into a buffer first, then assign addresses,
   then print**. Using symbolic names makes this easy.

Every slot gets a `define` so the assembly reads well:

| Kind | Emitted name | Example |
|---|---|---|
| Global | `g_<name>` | `define g_head_x 161` |
| Fixed-address global | `g_<name>` (address from attribute) | `define g_UP 7` |
| String literal | `__str<N>` | `define __str3 200` |
| Param/local | `l_<func>__<name>`, plus `_<k>` if the name is taken | `define l_move_snake__packed_pos 190` |
| Temp | `t_<func>__<k>` | `define t_move_snake__0 192` |
| Other anonymous global (static local) | `__anon<N>` | `define __anon0 40` |
| Compiler globals | `__g2_<name>` | `define __g2_started 32` |
| Function label | `fn_<name>` (including `fn_start`, `fn_tick`) | `fn_move_snake:` |
| Internal label | `__L<N>_<what>` (codegen), `__C<N>` (chibicc's `break`/`continue`/`goto` labels), `__g2_entry_*`, `__g2_end` | `__L12_else:` |
| Load data | `<NAME>` exactly as written (it is the user's own symbol) | `load FONT pack file "..."` |

g2 identifiers follow C's identifier rules, so every mangled name above is valid as it is. Labels have
their own namespace. **Defines and load names share a namespace**, so:

- **Every emitted name starts with one of the prefixes `g_`, `l_`, `t_`, `fn_` or `__`.** The
  assembler's lexer (rply) uses the first rule that matches rather than the longest match, so a name
  that *begins* with `define`, `load`, `WIDTH`, `HEIGHT` or `MEMORY` is split into that keyword and the
  rest. A local in a function called `load_level` would break if names began with the C name.
- Labels and defines must not share a name either: when the assembler resolves an operand, it checks
  labels before constants, so a label would silently shadow a define. The prefixes keep the two sets apart.

- Function labels (`fn_*`, `__L*`) cannot clash with any define or load name.
- The prefixes keep defines from clashing with mnemonics (`add`, `col`, `pix` …) and with each other.
- The one remaining risk is a user `load` name equal to an emitted define, for example a load named
  `g_score` next to a C global `score`, or a load named `__g2_started`. The compiler checks every load
  name against the set of emitted defines and reports an error on a match. (A load name equal to a C
  identifier is already rejected by the C parser as a redeclaration.) Operands that are a known address plus a constant offset
(`arr[3]`, `s.y`) are emitted as the **numeric address with a comment**: `cpy 35 13 ; g_arr+3`. Defines
are plain text substitution, so `g_arr+3` would not be evaluated by the assembler.

### 5.3 Frame overlay (memory optimisation, phase 4)

With recursion banned, the call graph from `start` and `tick` is a DAG. Two functions can share memory
if neither can be active while the other is. Standard compiled-stack algorithm:

```
base(start) = base(tick) = FRAMES_BEGIN        // the entry stub calls them one after the other
for f in topological order of the call graph:
    base(f) = max over callers c of (base(c) + frame_size(c))
frames_end = max over f of (base(f) + frame_size(f))
```

Notes:

- A callee's frame always sits above every caller's frame, so `&local` passed down (as in
  `unpack_pos(&seg_x, &seg_y, ...)`) is still safe.
- Sibling callees overlap. This is why the argument rule in §6.8 matters.
- Locals do not survive between `tick` calls, which is ordinary C semantics. `static` locals become
  globals and do survive.
- `-fno-overlay` turns this off for debugging.

### 5.4 Static checks

- **Recursion**: run DFS on the call graph from the roots. If a back edge is found, report
  `error: recursion is not supported on g2: tick -> a -> b -> a`.
- **Call depth**: the entry stub's `call` takes one call-stack entry, so the longest call chain from
  `start` or `tick` plus 1 must fit in the 16-entry call stack (slots 16–31). Exceeding it is an error.
  Intrinsics (`col`, `pix`) do not count, because they are not calls.
- **Memory**: if the compiler's allocation is larger than `-mmemory`, report an error. Load data does
  not count, because the assembler adds it beyond `#memory`.

---

## 6. Code Generation (new `codegen_g2.c`, replacing `codegen.c`)

### 6.1 File structure of the output

```g2
; Generated by chibicc-g2 from snake_g1.c
#width 64
#height 64
#memory 431

load FONT pack file "code8x8.font"

; ---- globals ----
define __g2_started 32            ; set to 1 once start() has run
define g_segments 33              ; int segments[128]  (33..160)
define g_segments_start_index 161
...
; ---- frame: move_snake ----
define move_snake__packed_pos 190
define move_snake__next_index 191
define move_snake__t0 192
...

; ---- entry stub: instruction 0, runs every frame ----
    jnea __g2_entry_first __g2_started 1   ; first frame: flag is still 0
    ja __g2_entry_tick
__g2_entry_first:
    ldi __g2_started 1
    ; ---- global initialisers ----
    ...
    call fn_start
__g2_entry_tick:
    call fn_tick
    ja __g2_end

fn_start:
    ; seed_rand(1337);
    ldi seed_rand__seed 1337
    call fn_seed_rand
    ; reset();
    call fn_reset
    ret

fn_tick:
    ...
    ret

fn_pack_pos:
    ...
    ret

__g2_end:
```

**How the entry stub works.** g2 has a single entry point: each frame it runs from instruction 0 until
execution goes past the last instruction. The compiler therefore always emits the stub above as the
first code in the file, before any function:

- `__g2_started` is a compiler-reserved global, the first slot after the reserved area. Memory starts
  zeroed, so on the first frame `jnea` sees `0 != 1` and branches to the one-time path. That path
  sets the flag, runs the global initialisers, and calls `start()`.
- On the first frame, execution then falls through into `call fn_tick`. `tick()` runs on every frame,
  including the first, right after `start()`. This matches the revised sample in `g2asm-spec.md`.
- After `tick()` returns, `ja __g2_end` jumps past the last instruction and ends the frame. All the
  function bodies sit between the stub and `__g2_end:`, so they only run when called.
- `start` and `tick` are compiled as ordinary functions, so `return` inside them is a normal `ret`.
  Their locals get normal frames. They cost one call-stack entry and a `call`/`ret` pair per frame,
  which is negligible.
- If the program has no `start`, leave out `call fn_start`. The one-time path still sets the flag and
  runs the initialisers. If there is no `tick`, leave out `call fn_tick`. Warn about both. If there
  are no global initialisers and no `start`, leave out the whole one-time path and `__g2_started`.

Each function ends with an unconditional `ret`, so execution never falls from one function into the next.
- Before each statement, emit `; <source line>` as a comment. This uses `tok->line_no` and the file's
  contents. It replaces chibicc's `.loc` directives.

### 6.2 Expressions: the basic lowering

`a`, `b` are operands from `gen_expr`, and `d` is the destination (the hint or a new temp). When an
operand is `OP_IMM`, the cheapest form is chosen.

| C | Both slots | One immediate |
|---|---|---|
| `a + b` | `add d a b` | `addi d a k` (`inc` if `d==a, k==1`) |
| `a - b` | `sub d a b` | `subi d a k` (`dec` if `d==a, k==1`); `k - b`: `ldi` + `sub` |
| `a * b` | `mul d a b` | `muli d a k`; `k==1` → copy/nothing, `k==0` → `ldi d 0`, `k==-1` → `sub d 0 a` |
| `a / b` | `div d a b` | `divi d a k`; `k==1` → nothing |
| `a % b` | `mod d a b` | `modi d a k` |
| `-a` | `sub d 0 a` | folded |
| `~a` | `sub d 2 a` (slot 2 = -1, so `-1 - a`) | folded |
| `+a`, casts between integers | nothing | nothing |
| `(_Bool)a` | `cmp d a 0; mul d d d` | folded |
| `k` (constant, needed in a slot) | `0`/`1`/`-1` → reserved slot 0/1/2; otherwise `ldi t k` (or the constant pool) | |
| `&global`, `&local`, `array` (decay) | `ldi d g_name` | |
| `&load_sym` / `load_sym` | `ldi d NAME` | |
| `&x + k` | `ldi d <addr+k> ; g_x+k` | |
| `a, b` | evaluate `a`, discard it, evaluate `b` | |

Never multiply raw user values to test them. `x * x` can overflow to 0 (65536² = 2³²). All
normalisation goes through `cmp` first, whose result is always in {-1, 0, 1}.

### 6.3 Comparisons

**As values (0 or 1)**, with no branches, 3 instructions each:

| C | Sequence | Why it works (`t = cmp(a,b)` ∈ {1: a<b, 0: a=b, -1: a>b}) |
|---|---|---|
| `a < b` | `cmp t a b; addi t t 1; divi d t 2` | (1+1)/2=1, (0+1)/2=0, (-1+1)/2=0 |
| `a <= b` | `cmp t a b; addi t t 2; divi d t 2` | 3/2=1, 2/2=1, 1/2=0 |
| `a > b` | `cmp t b a; addi t t 1; divi d t 2` | swapped operands of `<` |
| `a >= b` | `cmp t b a; addi t t 2; divi d t 2` | swapped operands of `<=` |
| `a == b` | `cmp t a b; mul t t t; sub d 1 t` | 1 - t² |
| `a != b` | `cmp t a b; mul d t t` | t² |
| `!a` | `cmp t a 0; mul t t t; sub d 1 t` | |

(The floor division `divi` gives `1/2 == 0` for positive numerators, which is exactly what is needed here.)

**As branch conditions** (`if`, `while`, `for`, `?:`, `&&`, `||`), use the special function
`gen_cond(node, Label false_target)`, which never materialises a 0/1:

| Condition | Jump to `F` when false |
|---|---|
| `a == b` | `jnea F a b` |
| `a != b` | `jnea T a b; ja F; T:` *(see below)* |
| `a < b` | `cmp t a b; jnea F t 1` |
| `a > b` | `cmp t a b; jnea F t 2` (slot 2 = -1) |
| `a <= b` | `cmp t a b; jnea T t 2; ja F; T:` (false when t == -1) |
| `a >= b` | `cmp t a b; jnea T t 1; ja F; T:` (false when t == 1) |
| `x` (any other expr) | `jnea T x 0; ja F; T:` (false when x == 0) |
| `!c` | `gen_cond_true(c, F)`, the mirror function, so `!` costs nothing |
| `a && b` | `gen_cond(a, F); gen_cond(b, F)` |
| `a \|\| b` | `gen_cond_true(a, T); gen_cond(b, F); T:` |

Write the matching `gen_cond_true(node, T)` (jump when true) so that each form has a short encoding
in one direction or the other. For example, `if (x)` jumps to `F` when `x == 0`. That is
`gen_cond_true` of `x == 0`, which is `jnea`'s inverse, so emit `jnea T x 0` and fall into `ja F`.
Better still, flip the layout so the `then` block is the jump target.

"Jump if equal" is always built from `jnea` plus a label. The compiler never emits `sne`, which is
being removed from the spec.

### 6.4 L-values, assignment, increment

- `x = e` with `x` direct: `gen_expr(e, &x)`. If the result did not land in `x`, emit `cpy x r`.
  If `e` is an immediate, emit `ldi x k`.
- `*p = e`, `a[i] = e`: compute the address into temp `ta` (`ldi ta g_a; add ta ta i`, or
  `addi ta g_p...`), compute `e` into a slot `r`, then `sti ta r`.
- Reads of an indirect l-value: `rdi d ta`.
- `x op= e`: direct form `op x x e`. Indirect form: `rdi t ta; op t t e; sti ta t`.
  The address is computed only once. chibicc's parser already rewrites `a op= b` as
  `tmp = &a, *tmp = *tmp op b`. **Pattern-match that shape in codegen**, or add
  `ND_ASSIGN_OP` nodes to the parser, so direct variables don't go through a pointer temp.
- `x++`, `++x`, `x--`, `--x` (also rewritten by the parser): direct form `inc x`/`dec x`. For the
  post-forms used as values, emit `cpy t x` first. Statement-level `x++` should not produce a copy,
  which needs a "value unused" flag passed from `ND_EXPR_STMT`.
- Struct assignment (phase 4): an unrolled `cpy` for each slot if size ≤ 8, otherwise a copy loop.

### 6.5 Control flow

- `if/else`, `while`, `do-while`, `for`, `break`, `continue`, `goto`, labels and `?:` map
  directly onto `gen_cond` and `ja`.
- `switch`: a chain of `jnea`-based compares to the case labels, then `ja default` (or break).
  chibicc's `case lo ... hi` ranges become two compares. A jump table is not possible, because `jne`
  jumps to `$index`, which would need label values in memory. It might become possible later (§12).
- `return e`: `gen_expr(e, &slot13)`, then `ret`. `start` and `tick` are no exception. The entry
  stub ignores their return values.
- Statement expressions (`({ ... })`) are supported because they are just blocks.

### 6.6 Bitwise operations

| C | Lowering |
|---|---|
| `~a` | `sub d 2 a` |
| `a << k` (constant 0 ≤ k ≤ 30) | `muli d a 2^k` (wrap-around matches the two's-complement shift) |
| `a << 31` | `modi d a 2` → `muli d d 2^30` → `muli d d 2` |
| `a >> k` (constant) | `divi d a 2^k` (floor division is the same as an arithmetic shift) |
| `a & (2^k - 1)` (constant mask) | `modi d a 2^k` (floor mod by a positive number gives the low k bits) |
| `a & -2^k` | `divi t a 2^k; muli d t 2^k` |
| any other `&`, `\|`, `^`, `<<`, `>>` | call `__g2_and(a,b)`, `__g2_or`, `__g2_xor`, `__g2_shl`, `__g2_shr` |

The helpers live in `include/__g2rt.h`. They are written in the C subset as `static` functions
(32-iteration bit loops using `/2` and `%2`, with care for the sign bit) and are auto-included by the
driver. Do the lowering **in the parser** (or in a small AST pass after parsing) so the inserted calls
show up in `refs`, which makes liveness, frame allocation and the call graph work for them.

### 6.7 Intrinsics

`color(r, g, b)` → `col R G B`, and `pix(x, y)` → `pix X Y`. Each argument is evaluated with
`gen_expr`, and immediates are moved into temps first. For example, `color(0, 0, 0)` becomes
`col 0 0 0` with no temps at all, because 0 is reserved slot 0. The intrinsic has no frame and no `call`.

### 6.8 Calls

```
; r = f(a, g(b), 7);
    ; args containing calls (or any call among the args) go to caller temps first,
    ; because evaluating g() may clobber f's param slots (and with overlay, siblings overlap)
    cpy  g__x b
    call fn_g
    cpy  caller__t0 13
    cpy  f__p0 a
    cpy  f__p1 caller__t0
    ldi  f__p2 7
    call fn_f
    cpy  r 13          ; or consume 13 directly if the next op reads it immediately
```

- If no argument contains a call, evaluate each argument **directly into the parameter slot**
  (`dest_hint = &f__pN`).
- If any argument contains a call (scan the AST for `ND_FUNCALL`, including those from lowered bitwise
  ops), first evaluate every argument into caller temps, then copy them all.
- The result in slot 13 must be copied out before the next call. The simplest rule is to always
  `cpy` it to the destination or a temp right away.
- Calls to functions with no definition (and no intrinsic) are an error that names the missing function.
  There is no linker to supply it.

### 6.9 Initialisers

- **Globals**: emit `ldi`s in the entry stub's one-time path (§6.1), before `call fn_start`, for every
  **non-zero** slot of `init_data`, since memory starts zeroed. They run once, on the first frame.
  Relocations (`int *p = &x + 2;`) become `ldi g_p <addr>`.
  - Large initialised arrays (lookup tables, sprites) produce many `ldi`s. A later optimisation could
    emit them as `load __init_N pack bytes "..."` data statements instead. Since load data sits above
    `#memory`, such a global would be addressed only through its load name, so `g_arr+3`-style numeric
    operands would become `ldi`+`addi` sequences.
- **Locals**: chibicc lowers `int a[3] = {1,2,3}` to `ND_MEMZERO` plus element assignments.
  `ND_MEMZERO` of n slots becomes n × `cpy x 0` for small n, or a loop for large n.
- **String literals** are anonymous char arrays in the global area, initialised like any global.
  An optional `-mpascal-strings` could match the `load … string` format (length prefix). By default
  they stay C-style, NUL-terminated.

### 6.10 Peephole pass (cheap, high value)

Run it on the buffered instruction list for each function:

- Remove `cpy x x`.
- `cpy t X` followed by reads of `t` before `t` is overwritten → use `X` directly when `t` is a temp
  used only once.
- `call f; cpy t 13; cpy x t` → `call f; cpy x 13`.
- Remove `ja L` directly before `L:`.
- `ja` to an unconditional `ja` → thread the jump.
- Remove unreachable code after `ret` / `ja` up to the next label.

---

## 7. Worked Example (target output for parts of `snake_g1.c`)

```c
int pack_pos(int x, int y) { return x * 0x10000 + y; }
```
```g2
fn_pack_pos:
    ; return x * 0x10000 + y;
    muli pack_pos__t0 pack_pos__x 65536
    add 13 pack_pos__t0 pack_pos__y
    ret
```

```c
int unpack_pos(int *x, int *y, int pos) { *x = pos / 0x10000; *y = pos % 0x10000; }
```
```g2
fn_unpack_pos:
    ; *x = pos / 0x10000;
    divi unpack_pos__t0 unpack_pos__pos 65536
    sti unpack_pos__x unpack_pos__t0
    ; *y = pos % 0x10000;
    modi unpack_pos__t0 unpack_pos__pos 65536
    sti unpack_pos__y unpack_pos__t0
    ret
```

```c
void move_snake() {
    int packed_pos = pack_pos(head_x, head_y);
    int next_index = (segments_start_index + segment_count) % MAX_SEGMENTS;
    segments[next_index] = packed_pos;
    head_x = (head_x + dir_x) % WIDTH;
    ...
    if (segments_to_add) { segments_to_add--; segment_count++; }
    else { segments_start_index = (segments_start_index+1 % MAX_SEGMENTS); }
}
```
```g2
fn_move_snake:
    ; int packed_pos = pack_pos(head_x, head_y);
    cpy pack_pos__x g_head_x
    cpy pack_pos__y g_head_y
    call fn_pack_pos
    cpy move_snake__packed_pos 13
    ; int next_index = (segments_start_index + segment_count) % MAX_SEGMENTS;
    add move_snake__t0 g_segments_start_index g_segment_count
    modi move_snake__next_index move_snake__t0 128
    ; segments[next_index] = packed_pos;
    ldi move_snake__t0 g_segments
    add move_snake__t0 move_snake__t0 move_snake__next_index
    sti move_snake__t0 move_snake__packed_pos
    ; head_x = (head_x + dir_x) % WIDTH;
    add move_snake__t0 g_head_x g_dir_x
    modi g_head_x move_snake__t0 64
    ...
    ; if (segments_to_add)
    jnea __L1_then g_segments_to_add 0
    ja __L1_else
__L1_then:
    dec g_segments_to_add
    inc g_segment_count
    ja __L1_end
__L1_else:
    ; segments_start_index = (segments_start_index+1 % MAX_SEGMENTS);
    addi g_segments_start_index g_segments_start_index 1
__L1_end:
    ret
```

```c
int tick() { ... move_timer--; if (move_timer <= 0) { ... } ... color(0, 0, 0); }
```
```g2
fn_tick:
    ...
    ; move_timer--;
    dec g_move_timer
    ; if (move_timer <= 0)
    cmp tick__t0 g_move_timer 0
    jnea __L7_then tick__t0 2        ; false only when move_timer > 0 (cmp == -1)
    ja __L7_end
__L7_then:
    call fn_move_snake
    ...
    ldi g_move_timer 3
__L7_end:
    ...
    ; color(0, 0, 0);
    col 0 0 0
    ret
```

---

## 8. Standard Library (`g2.h` / `g2lib.c`)

`snake_g1.c` calls `hexcolor`, `seed_rand`, `next_rand`, `int_to_str` and `draw_text`, which are not
declared anywhere yet. chibicc correctly rejects implicit declarations. Because there is no linker,
the library is **header-only C** that `g2.h` includes. Every function is `static`, so liveness (§4.4)
strips the ones a program doesn't use.

Sketches that rely on g2 semantics:

```c
static void hexcolor(int c) {             // c = 0xRRGGBB, non-negative
    color(c / 65536, (c / 256) % 256, c % 256);
}

static int __rand_state;
static void seed_rand(int seed) { __rand_state = seed; }
static int next_rand(void) {              // LCG; mul wraps mod 2^32 on g2
    __rand_state = __rand_state * 1103515245 + 12345;
    return (__rand_state / 65536) % 32768; // floor div/mod => always in [0, 32767]
}

static void int_to_str(int *buf, int n);  // decide: NUL-terminated or length-prefixed
static void draw_text(int *str, int *font, int size, int x, int y);  // depends on the .font format
```

`draw_text` depends on the layout of `code8x8.font` (bit-packed rows from `pack`?), which you define.
It will need bit tests, which can be written as `(row / 2^(7-col)) % 2` without bitwise helpers.

---

## 9. Diagnostics

Every unsupported construct should report `error_tok` at the offending token. For example:

```
snake.c:12:5: float is not supported on g2
snake.c:40:3: recursion is not supported on g2: tick -> collide_snake -> reset -> collide_snake
snake.c:51:9: call to undefined function 'next_rand' (g2 has no linker; define it or include its header)
snake.c:60:1: call depth 18 exceeds the g2 call stack (16)
```

Also keep chibicc's existing type errors. Add warnings for: no `start`/`tick`, literal wrap-around,
and macro redefinition with a different value (§4.2). Window-size errors are listed in §3.7.

---

## 10. Implementation Phases

Each phase ends with a milestone program that has to assemble and run on the real VM.

### Phase 0: Strip and rewire (no new features)

1. Create a branch. Delete `codegen.c`'s x86 code and add an empty `codegen_g2.c` with the same
   `codegen(Obj *prog, FILE *out)` entry point. Update the `Makefile`: drop the stage2 and self-hosting
   targets, and drop the x86 test targets.
2. `main.c`: remove assembler and linker steps, set default output `.g2asm`, and remove host include paths.
3. `type.c`: all scalar and pointer sizes become 1. `init_data` becomes `int32_t*`, with `write_buf`
   fixed to match.
4. `preprocess.c`: new predefined macros.
5. `parse.c`/`tokenize.c`: reject float/unsigned/variadic/VLA/alloca/TLS/atomics/asm. Lazy `__func__`.
   Roots are only `start`/`tick`. Fix `eval` to 32-bit floor semantics.

*Milestone:* `chibicc-g2 -E` works, and parsing `snake_g1.c` (with stub prototypes) succeeds with no output.

### Phase 1: Minimum viable backend

1. Memory allocator: globals from 32, a disjoint frame per live function, and naming plus `define`s.
2. Per-function buffered emission, with temp allocation and its high-water mark.
3. `gen_expr` for variables, constants, `+ - * / %`, unary `-`, assignment to direct l-values, and calls
   with the argument rule.
4. `gen_stmt` for blocks, expression statements, `if`, `while`, `for`, `return`, `break`, `continue`.
5. `gen_cond` for all six comparisons and truthiness.
6. The entry stub (`__g2_started` flag, one-time path, `call fn_start`/`call fn_tick`), `__g2_end`, and
   `#width`/`#height` via `window_dimension()`, the computed `#memory`, and the macro redefinition warning.
7. Attributes `address(N)` and `g2_intrinsic(...)`, then the updated `g2.h`.
8. Recursion check and call-depth check.

*Milestone:* a program that moves a pixel with the arrow keys and changes colour on `A`.

*Status:* done (`example/move_pixel.c`). Some Phase 2 items were cheap to include once `gen_cond`
existed, so Phase 1 also has `&&`, `||`, `!`, `?:`, the comma operator, comparisons as values,
`_Bool` casts, `~`, `do-while`, `goto`/labels, scalar global initialisers, and source-line comments.
It also has the headless test runner (§11). Only a missing `tick()` triggers a warning: a program
without `start()` is normal.

### Phase 2: Full snake subset

1. Pointers: `&`, `*`, `rdi`/`sti`, indirect l-values. Arrays: decay, indexing (direct when the index
   is constant).
2. Compound assignment and `++`/`--` (pattern-matching the parser's rewrites), plus value-unused tracking.
3. `switch`/`case`/`default` and statement expressions.
4. Local aggregate initialisers and `ND_MEMZERO`, and string literals as values. Global
   initialisers, arrays and strings included, are already emitted with one `ldi` per non-zero slot.
5. Enforce `const`. chibicc parses it and then ignores it, so `UP = 1;` currently compiles even
   though `g2.h` declares the keys `const`.
6. The `load` statement. Also reject load names that clash with the assembler's predefined
   constants (§13).

*Milestone:* `snake_g1.c` compiles, given the library functions it needs, and runs.

### Phase 3: Library and bitwise

1. `g2lib.c`: `hexcolor`, `seed_rand`, `next_rand`, `int_to_str`, `draw_text`.
2. Constant shift/mask lowering, plus the `__g2rt.h` helpers and the parser lowering to helper calls.
3. Dropping unreferenced string literal globals.

*Milestone:* snake shows its score with the font.

### Phase 4: Quality and memory

1. Peephole pass (§6.10).
2. Frame overlay (§5.3) and sharing slots between sibling scopes.
3. Constant pool: each distinct constant used as a non-immediate operand goes in one global slot,
   initialised once in the entry stub's one-time path. This turns `ldi t k; op d a t` into `op d a K_k`.
4. Structs and unions: member access as a constant offset on direct l-values or `addi` on indirect
   ones, and struct assignment. Struct pass/return by value stays rejected, or gets lowered to a hidden
   pointer.
5. Optionally, initialised data emitted as `load` statements (data placement is now known; see §6.9).

### Phase 5: Optional extensions

See §12.

---

## 11. Testing Strategy

The existing `test/` suite targets x86, `printf` and the host linker, so it cannot run as is.
Build a new one:

1. **A headless g2 runner (implemented in Phase 1).** Programs are assembled by the real assembler
   (`g2a`), so pseudo-instruction expansion is the real one. The resulting `.g2b` runs in
   `tools/g2run.py`, a small headless VM that follows the reference VM (`cg2`). It runs N frames from
   instruction 0 with scripted key presses (`--press RIGHT:0-4`) and prints memory by define name and
   the pixels drawn in the last frame. `tools/run_tests.py` (run by `make test`) drives the suite.
   Tests whose first line is `// expect-error: <text>` must fail to compile with that message.
2. **`test-g2/*.c` in chibicc's `ASSERT` style.** `test-g2/test.h` provides
   `static void assert_eq(int line, int expected, int actual)`, which on failure records
   `__test_fail_line = line` (a global). Each test does its work in `start()`, so a single frame is enough. The runner reads the
   `define` table from the `.g2asm` to find `g_test_fail_line`, and a non-zero value means failure.
   Port the relevant tests over: `arith.c`, `control.c`, `pointer.c`, `function.c`, `string.c`,
   `struct.c`, `sizeof.c` (with new expected sizes), `initializer.c`, `literal.c`, `usualconv.c`.
3. **Semantics tests specific to g2:** floor division and mod on negatives (`-7/2 == -4`, `-1%64 == 63`),
   folded and runtime results agreeing for the same expressions, 32-bit wrap-around, all comparison
   lowerings at the boundaries (`INT_MIN`, `INT_MAX`, equal values), bitwise helpers against
   C-computed tables, the argument-clobber case `f(2, f(1, 0))`, and `&local` passed to callees with
   overlay on.
4. **Error tests:** each rejected feature gives its diagnostic (`test-g2/errors/*.c` plus expected
   message substrings).
5. **Golden-output tests (optional):** check in the `.g2asm` for a few small programs so codegen changes
   show up as diffs.
6. `make test` runs all of the above.

---

## 12. Possible Future Extensions

- **Recursion** is out of scope by decision. If it is ever wanted, a "stack mode" for just the
  functions in a call-graph cycle would keep the rest of the design unchanged.
- **Function pointers and jump tables**: these need `ldi t some_label` to load an instruction index,
  plus an indirect-call sequence that pushes the return address the same way the `call` pseudo-op
  does. That means relying on the exact expansion of `call`/`ret` (slot 15 and 16–31), or adding a
  `calli slot` pseudo-op to the assembler. The second option is cleaner.
- **Unsigned types**: emulate unsigned compare through a bias (`a ^ INT_MIN`, which is `a + INT_MIN`
  under wrap-around) and use helpers for unsigned div/mod.
- **Multiple translation units**: compile to an intermediate form and do a final "link" that allocates
  all memory globally. This is not needed while everything is one TU.
- **Real register-style optimisations**: copy propagation across statements, and dead-store elimination
  on temps.

---

## 13. Assembler and Design Questions

The resolved items are recorded here because the design depends on them.

### Resolved

- **Where `load` data goes.** The assembler places loaded data right after the `#memory` region and
  grows the array to fit. For example, `#memory 64` plus a 10-slot file puts the data at slots 64–73,
  and memory becomes 74 slots. So the compiler sets `#memory` to the end of its own allocation (§5.1)
  and never needs to know data sizes. Load names resolve to addresses at or above `#memory`.
- **`define` names as operands.** Defines are replaced at the token level, so they work as any operand
  and do not cause substring problems. The compiler uses symbolic names everywhere (§5.2).
- **Identifier syntax and namespaces.** Identifiers follow C rules. Labels have their own namespace.
  Defines and load names share one, and the compiler checks for clashes there (§5.2).
- **`sne`.** It is being removed from the spec, and the compiler never emits it (§6.3).
- **Pseudo-op side effects.** Pseudo-ops only modify slot 14 and, for `call`/`ret`, the call stack
  (slots 15–31). So the `dest_hint` scheme (§3.3) is sound as long as the compiler never allocates
  slot 14.
- **Entry point.** There is a single code entry point at the first instruction, and the program runs
  from there every frame. The compiler emits its own entry stub that runs `start()` once and `tick()`
  every frame (§6.1).
- **Operand syntax.** Operands are separated by whitespace only, with no commas. Comments start with
  `;`. The compiler emits one space between operands.
- **Labels as values.** `ldi` cannot take a label. Function pointers and jump tables stay out of scope
  (§12) until that is added to the assembler.
- **Window metadata.** It comes from the `WIDTH`/`HEIGHT` macros, with header defaults behind
  `#ifndef` guards and no `-mwidth`/`-mheight` flags (§3.7).
- **Facts from the assembler's source (`g2asm/assembler.py`, `instructions.py`, `lexer.py`).**
  - Pseudo-instructions expand to: `cpy` → `add d s 0`; `sub` → `mul 14 2 b; add d a 14`;
    `addi`/`muli`/`divi`/`modi` → `ldi 14 k` plus the operation; `inc`/`dec` → `add d d 1`/`2`;
    `jnea`/`ja` → `ldi 14 L; jne 14 …`. `call` pushes the return address at `$15` and increments
    slot 15. `ret` decrements it and jumps. Every one of them reads its operands before it writes
    its destination.
  - The code section has to start with an instruction. A label as the first code token is rejected,
    which the entry stub satisfies.
  - The assembler predefines the constants `CONTROL1`, `CONTROL2`, `A`, `B`, `UP`, `DOWN`, `LEFT`,
    `RIGHT` and `DELTA` (as addresses). A `load` with one of these names would only produce a
    warning and keep the predefined value, so the `load` parser (Phase 2) must reject them.
  - Naming constraints from the lexer are described in §5.2.
- **Recursion.** It is not required. The fixed-address frame model (§3.2) is the final design.

### Still open

None. All assembler questions are resolved.

---

## 14. Notes on `example/snake_g1.c`

These are things noticed while planning, not compiler requirements:

- `segments_start_index = (segments_start_index+1 % MAX_SEGMENTS);` parses as
  `segments_start_index + (1 % 128)`, so the index never wraps. You probably want
  `(segments_start_index + 1) % MAX_SEGMENTS`.
- `unpack_pos`, `start` and `tick` are declared `int` but return nothing. That is legal C and harmless
  here (the compiler can warn), but `void` would be clearer.
- `hexcolor`, `seed_rand`, `next_rand`, `int_to_str`, `draw_text` need declarations or definitions
  before use (§8).
- `#define WIDTH 64` currently comes **after** `#include "g2.h"`. Under the chosen design (§3.7) it
  has to move above the include:
  ```c
  #define WIDTH 64
  #define HEIGHT 64
  #include "g2.h"
  ```
  Otherwise the header's `#ifndef` guard defines `WIDTH` as 100 first, and the program's own
  definition then triggers the new redefinition warning.
- `int score_str[12]` used as a string buffer works well, because `char` and `int` are the same type
  on g2.

---

## 15. File-Level Checklist

| File | Change |
|---|---|
| `main.c` | Remove the as/ld pipeline and host include paths. Add `.g2asm` output, `-mmemory`/`-fno-overlay`, and window metadata through `window_dimension()`. |
| `preprocess.c` | New predefined macros, `window_dimension()` (constant-expression evaluation of `WIDTH`/`HEIGHT`), and the macro redefinition warning. |
| `tokenize.c` | Remove float literals. 32-bit int literals with a wrap warning. Keep raw string spelling. |
| `parse.c` | Feature rejection, attributes (`address`, `g2_intrinsic`), `load` declarations, lazy `__func__`, 32-bit floor `eval`, `int32_t` init buffer, `start`/`tick` roots, bitwise lowering into runtime helper calls, removal of the alloca/va-area/struct-return locals. |
| `type.c` | All scalar and pointer sizes 1. Remove float conversions. |
| `chibicc.h` | New `Obj` fields (fixed address, intrinsic, load info, addr/asm_name/frame_size). Remove x86 fields. Add the new codegen API. |
| `codegen.c` → `codegen_g2.c` | Rewritten: memory allocator, frame allocation and overlay, call graph checks, operand-based expression and condition codegen, statement codegen, initialisers, peephole pass, output formatting. |
| `include/` | Replace with `stddef.h`, `stdbool.h`, `limits.h`, `g2.h`, `__g2rt.h`. Add `g2lib.c`. |
| `Makefile` | Drop stage2 and the x86 tests. Add the `test-g2` target and the runner. |
| `test-g2/`, `tools/` | New test suite and headless runner (§11). |
| `example/g2.h` | Use the attributes and pull in the library (§3.6). |
