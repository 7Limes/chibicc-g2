# chibicc-g2: a C compiler for g2

chibicc-g2 compiles a subset of C to **g2 assembly** (`.g2` files), the text format that the g2
assembler (`g2a`) turns into programs for the g2 virtual machine (`cg2`). It is a fork of
[chibicc](https://github.com/rui314/chibicc) by Rui Ueyama: the preprocessor and parser are
chibicc's, and the x86-64 backend has been replaced with one for g2.

g2 is a small machine. Memory is an array of signed 32-bit integers, there are no registers or
bitwise instructions, and the whole program runs once per frame, 60 times per second. This README
describes the C dialect that results: what compiles, what it means, and where it differs from
standard C.

```c
#define WIDTH 64
#define HEIGHT 64
#include <g2.h>

int x = 32, y = 32;

void tick() {
  if (RIGHT) x = (x + 1) % WIDTH;
  if (LEFT)  x = (x - 1) % WIDTH;   // -1 % 64 == 63 on g2 (see "Division")
  if (DOWN)  y = (y + 1) % HEIGHT;
  if (UP)    y = (y - 1) % HEIGHT;

  color(255, 255, 255);
  pix(x, y);
}
```

## Contents

- [Building and running](#building-and-running)
- [Program structure](#program-structure)
- [The memory model](#the-memory-model)
- [Types](#types)
- [Integer arithmetic](#integer-arithmetic)
- [Floating point](#floating-point)
- [Pointers, arrays, structs and unions](#pointers-arrays-structs-and-unions)
- [Functions](#functions)
- [Statements and other syntax](#statements-and-other-syntax)
- [The hardware interface: `g2.h`](#the-hardware-interface-g2h)
- [Loading data](#loading-data)
- [Libraries and headers](#libraries-and-headers)
- [What isn't supported](#what-isnt-supported)
- [Compiler options](#compiler-options)
- [Reading the output](#reading-the-output)
- [Testing](#testing)
- [Credits](#credits)

## Building and running

```sh
make                 # builds build/chibicc-g2
./install.sh         # optional: installs to ~/.local (set PREFIX to change)
```

`install.sh` puts the compiler and its headers in `~/.local/lib/chibicc-g2` and links
`~/.local/bin/chibicc-g2` to it. The compiler finds its headers in the `include/` directory next to
its real location, so `#include <g2.h>` works from any directory.

To compile, assemble and run a program:

```sh
chibicc-g2 game.c          # writes game.g2
g2a game.g2 game.g2b       # assembles it
cg2 game.g2b               # runs it
```

`-o` picks a different output file. Each program is exactly one `.c` file (plus the headers it
includes): there is no linker, so everything a program calls must be defined in that translation
unit or in a header.

## Program structure

A program defines up to two special functions:

| Function | When it runs |
|---|---|
| `void start()` | once, on the first frame, before `tick()` |
| `void tick()` | every frame, including the first |

Both may also return `int` (the value is ignored), and neither may take parameters or be called
from C. A program without `tick()` compiles with a warning, since it does nothing.

The compiler emits a small **entry stub** as the first instructions of the program. g2 runs the
program from its first instruction every frame, so the stub checks a flag: on the first frame it
sets the flag, runs the global initialisers and calls `start()`; on every frame it then calls
`tick()` and jumps past the end of the program, which ends the frame.

Local variables don't keep their values between frames, as in ordinary C. Globals and `static`
locals do.

### Window size

The window size comes from the `WIDTH` and `HEIGHT` macros. Define them **before** including
`g2.h`, or pass `-DWIDTH=N -DHEIGHT=N`; otherwise `g2.h` sets both to 100. The final values become
the program's `#width` and `#height`, and they are ordinary macros in the code, so the window and
the code can't disagree. They can be any constant expression, such as `(TILE * 8)`. Redefining a
macro with a different value prints a warning that points at both definitions.

## The memory model

Everything is measured in **slots**: one slot is one element of g2's memory, a signed 32-bit
integer. There are no 8-bit bytes.

- `sizeof` counts slots: `sizeof(char) == sizeof(int) == sizeof(long) == sizeof(void *) ==
  sizeof(float) == 1`. Arrays and structs are the sum of their elements. All alignments are 1.
- `CHAR_BIT` is 32. A `char` holds any `int` value, and a string uses one slot per character.
- A pointer is a slot address. Pointer arithmetic counts elements as usual: `p + 1` is the next
  `int`, or the next struct of the pointed-to size.

The compiler allocates all memory at compile time:

| Slots | Contents |
|---|---|
| 0–11 | reserved by g2: constants 0, 1 and −1, the keys, and the frame time |
| 12–31 | reserved: the return value (13), assembler scratch (14) and the call stack (15–31) |
| 32 … | globals, then function frames (parameters, locals and temporaries), then constants |
| `#memory` … | data loaded by the assembler (`G2_LOAD`) |

There is no stack for data. Each function's parameters and locals live at **fixed addresses**, which
makes them as cheap to use as globals. Functions that can never be active at the same time share
memory, and so do locals in sibling blocks. This is why recursion isn't allowed (see
[Functions](#functions)). Taking the address of a local is fine, including passing it to a function
you call.

## Types

| Type | On g2 |
|---|---|
| `char`, `short`, `int`, `long`, `long long`, `signed` variants | 32-bit signed integer, 1 slot. All the same type in practice. |
| `_Bool` (`bool` with `<stdbool.h>`) | 0 or 1, 1 slot. Converting a value to `_Bool` gives `value != 0`. |
| `enum` | `int` |
| `float`, `double`, `long double` | Q16.16 fixed point, 1 slot (see [Floating point](#floating-point)) |
| `T *` | slot address, 1 slot |
| `T[N]`, `struct`, `union` | as in C, sized in slots |
| `void` | as in C |

`unsigned` types and the `u`/`U` integer suffix are **errors**: g2's comparison and division
instructions are signed, and emulating unsigned arithmetic would be slow. `const` is enforced:
assigning to a `const` object is an error. `volatile` and `restrict` are accepted and ignored.

### Literals

- Integer literals are `int`. Decimal, hex (`0x`), octal (`0`) and binary (`0b`) are accepted, and
  `L`/`LL` suffixes are ignored. A value in [2³¹, 2³²) wraps to a negative number with a warning
  (`0xFFFFFFFF` is −1), and anything larger is an error.
- Character literals are `int`: `'a'` is 97. Escape sequences work. Multi-character and wide
  (`L'x'`, `u'x'`, `U'x'`) literals are errors.
- String literals are arrays of `char`, one slot per character, with a terminating 0. `u8"..."` is
  accepted. `L"..."`, `u"..."` and `U"..."` are errors.
- Floating literals (`1.5`, `.25f`, `3e2`, `0x1p-4`) are described under
  [Floating point](#floating-point).

## Integer arithmetic

All integer arithmetic is 32-bit two's complement. Overflow wraps around (it isn't undefined), and
constant folding in the compiler wraps the same way.

| Operation | Behaviour |
|---|---|
| `+ - *`, unary `-` | wrap around |
| `/` | **rounds toward negative infinity**: `-7 / 2 == -4` (C gives −3) |
| `%` | **takes the sign of the divisor**: `-1 % 64 == 63`, `7 % -2 == -1` |
| `== != < <= > >=`, `!` | 0 or 1, signed comparison |
| `&& \|\| ?:` | short-circuit, as in C |
| `<<` | wraps around; the count must be 0–31 |
| `>>` | arithmetic shift (rounds toward negative infinity) |
| `~ & \| ^` | two's complement, as in C |

### Division

`/` and `%` use g2's own division, which rounds down rather than toward zero, so they differ from C
for negative operands. Since `a == (a / b) * b + a % b` still holds, `%` by a positive number is
never negative, which is often what you want: `(x - 1) % WIDTH` wraps around the screen. Constant
expressions are folded with the same rules. Dividing by zero is an error in a constant expression.
At run time it's undefined: `g2run` stops with an error, and `cg2` doesn't check for it.

### Bitwise operators

g2 has no bitwise instructions, so the compiler builds them from arithmetic. Many common forms take
one or two instructions:

| Expression | Compiles to |
|---|---|
| `~x` | `-1 - x` |
| `x << k`, `x >> k` (constant `k`) | multiply or divide by 2^k |
| `x & (2^k - 1)` | `x % 2^k` |
| `x & -2^k` | `x / 2^k * 2^k` |
| `x & 0`, `x & -1`, `x \| 0`, `x \| -1`, `x ^ 0`, `x ^ -1` | trivial |

Anything else (`a & b`, `x | 5`, `x << n` with a variable `n`, …) calls a runtime helper that
works through the bits in a loop. The helpers stop early when they can, but they cost tens to
hundreds of instructions, so prefer the forms above in hot code.

## Floating point

`float`, `double` and `long double` are all the same type in practice: a **Q16.16 fixed-point
number** in one slot. A value `x` is stored as the integer `round(x * 65536)`.

| | |
|---|---|
| Range | −32768 to 32767.99998 |
| Resolution | 1/65536 ≈ 0.0000153 (`FLT_EPSILON`) |
| Precision | about 4.8 significant digits |

There is no exponent, no infinity and no NaN. Results that don't fit wrap around like integer
overflow. Be careful with squares: `x * x` overflows once `|x| > 181`. (`hypotf` in `<math.h>`
avoids that.)

### Literals and conversions

- Floating literals are rounded to the nearest 1/65536. A literal of 32768 or more is an error, and
  a non-zero literal that rounds to 0 (`1e-6`) gives a warning. Suffixes `f` and `l` are accepted.
- `int` → `float` multiplies by 65536. Integers outside ±32768 don't fit and wrap around, with a
  warning if the compiler can see it.
- `float` → `int` **rounds toward negative infinity**, like integer division: `(int)2.7 == 2`,
  `(int)-2.7 == -3` and `(int)-0.5 == -1`. C truncates toward zero; use `truncf()` if you need
  that. Rounding down is usually what pixel code wants: `pix((int)x, (int)y)` picks the pixel that
  contains the point, even left of or above the origin.
- `float` → `_Bool` is `x != 0`. Converting between a pointer and a float is an error.

### Operations

| Operation | Behaviour | Cost |
|---|---|---|
| `+ -`, unary `-`, comparisons, `!`, `if (x)` | exact | the same as for `int` |
| `a * b` | rounded toward negative infinity | 1 instruction when one side is an `int` or a whole number or power of two like `2.0` or `0.25`; 7 for other constants; about 25 otherwise |
| `a / b` | rounded toward negative infinity | 1 instruction when `b` is an `int` or a whole number or power of two; about 30 otherwise, occasionally over 100 |
| `% ~ & \| ^ << >>` | errors, as in C | |

When one operand of `*` (or the divisor of `/`) is an integer, the compiler uses the integer
directly: `x * n` multiplies the raw value by `n`, and `x / n` divides it. This is exact, and stays
exact even when converting `n` to `float` on its own would overflow. Constant folding follows the
same rules, so folded and run-time results always agree.

A float can't be an array subscript or a `switch` value, and integer constant expressions (array
sizes, `case` labels) can't have floating type, although `(int)2.5` is fine.

## Pointers, arrays, structs and unions

These work as in C, with sizes in slots:

- `&`, `*`, `->`, `[]`, pointer arithmetic, pointer subtraction and comparison, `void *` arithmetic
  (one slot per step), pointers to pointers, arrays of arrays, and pointers to arrays.
- Accesses whose address is known at compile time (`arr[3]`, `s.y`, a local or global variable)
  use the address directly. Others load or store through a computed address.
- Structs and unions can be declared, nested, initialised (including designated initialisers),
  assigned (`a = b`, `*p = s`, `a = b = c`) and copied from compound literals. Their members can be
  read and written through any path.
- Struct and union **values** are otherwise not supported: you can't pass them to or return them
  from functions (pass a pointer instead), or use them in `?:`.
- Bit-fields are not supported.

## Functions

Functions are ordinary C functions with a few limits that come from g2:

- **No recursion**, direct or indirect. The compiler checks the call graph and reports the cycle:
  `recursion is not supported on g2: tick -> a -> b -> a`. Every function has a fixed frame, so a
  second active call would overwrite the first.
- **Call depth.** g2's call stack has 16 entries, so at most 16 functions can be active at once,
  counting `start()` or `tick()`, and any runtime helpers that operators call. The compiler checks
  the longest call chain.
- **No function pointers.** A function's name can only be called. Taking its address, storing it or
  calling through a pointer is an error.
- **No variadic functions** (`...`), in definitions or calls.
- Every called function must be **defined** in the program, since there is no linker.
  Declarations (prototypes) are fine as long as a definition exists somewhere in the file.
- `static`, `inline` and `extern` are accepted. Only functions reachable from `start()` and
  `tick()` are emitted, so unused functions cost nothing.

Arguments are copied into the callee's parameter slots, and the return value comes back in slot 13.

## Statements and other syntax

All of C11's statements and expressions are supported apart from the exceptions in
[What isn't supported](#what-isnt-supported):

- `if`/`else`, `while`, `do`/`while`, `for` (with declarations), `break`, `continue`, `return`,
  `goto` and labels, and `switch` with `case`, `default` and GNU case ranges (`case 1 ... 5:`).
- Compound assignment (`+=`, `<<=`, …), `++` and `--`, the comma operator, `?:`.
- Declarations anywhere in a block, `static` locals, `typedef`, `enum`, global and local
  initialisers (including designated initialisers and string initialisers for `char` arrays), and
  compound literals.
- `sizeof`, `_Alignof`, `_Alignas`, `_Generic`, `typeof`, GNU statement expressions (`({ ... })`,
  which must produce a value), and `__func__`.
- The full preprocessor: `#include`, `#define` (object- and function-like, `#`, `##`, variadic
  macros), `#if`/`#ifdef`/`#elif`/`#else`, `#undef`, `#error`, `#pragma once`, `#line`,
  `__FILE__`, `__LINE__`, `__COUNTER__`, `__DATE__`, `__TIME__` and so on.

Predefined macros include `__g2__` (1), `__CHAR_BIT__` (32), `__SIZEOF_INT__` and the other
`__SIZEOF_*__` macros (1), `__INT_MAX__`, `__STDC_HOSTED__` (0) and `__G2_FIXED_FRAC_BITS__` (16).

## The hardware interface: `g2.h`

`#include <g2.h>` provides the machine's features as C declarations:

| Declaration | Meaning |
|---|---|
| `void color(int r, int g, int b)` | set the drawing colour; one `col` instruction |
| `void pix(int x, int y)` | draw a pixel; one `pix` instruction |
| `const _Bool UP, DOWN, LEFT, RIGHT, A, B, CONTROL1, CONTROL2` | 1 while the key is held |
| `const int DELTA` | time since the last frame, in milliseconds |
| `WIDTH`, `HEIGHT` | the window size (see [Window size](#window-size)) |
| `G2_LOAD(...)` | data loaded by the assembler (see [Loading data](#loading-data)) |

It also includes the standard library, `g2lib.h`, unless `G2_NO_STDLIB` is defined first.

These are built from three attributes, which your own declarations can use too:

| Attribute | On | Meaning |
|---|---|---|
| `__attribute__((address(N)))` | a global with no initialiser | the variable lives at slot `N` instead of being allocated |
| `__attribute__((g2_intrinsic("col" \| "pix")))` | a `void` function with no body | calls compile to that instruction (3 or 2 parameters) |
| `__attribute__((g2_load(op, type, data)))` | an `int` or `char` array with no initialiser | see below |

## Loading data

The assembler can load data into memory when the program starts. In C, declare it with `G2_LOAD`:

```c
G2_LOAD(FONT, pack, file, "code8x8.font");   // const int FONT[]
G2_LOAD(GREETING, raw, string, "Hello");     // const int GREETING[]
```

- The operation is `raw` (one slot per byte) or `pack` (four bytes per slot, little-endian).
- The data type is `file` (a path, relative to the `.c` file) or `string` (printable ASCII; the
  first slot holds the length). `bytes` is reserved but not supported yet.
- The name becomes a `const int` array that can be indexed and passed to functions like any other.
- Other tools (editors, language servers) see a plain `extern const int NAME[]`, so the source
  stays valid C.

## Libraries and headers

Library functions are `static` functions in headers, since there's no linker. Only the ones a
program calls are emitted.

### The standard library (`g2lib.h`, included by `g2.h`)

| Function | |
|---|---|
| `int min(int a, int b)`, `int max(int a, int b)` | |
| `void seed_rand(int seed)`, `int next_rand(void)` | a multiplicative LCG; `next_rand()` returns 0 … 2147483646. Seed with a non-zero value. |
| `void hexcolor(int rgb)` | sets the colour from `0xRRGGBB` |
| `void rect(int x, int y, int w, int h)` | a filled rectangle |
| `void line(int x0, int y0, int x1, int y1)` | a line (Bresenham) |
| `void int_to_str(int *dest, int x)` | writes `x` in decimal as a length-prefixed string; `dest` needs 12 slots |
| `void float_to_str(int *dest, float x, int decimals)` | the same for a float, with 0–4 decimals (rounded) |
| `void draw_text(const int *str, const int *font, int size, int x, int y)` | draws a length-prefixed string in a packed `size`×`size` font |

The strings these functions use are **length-prefixed**: `s[0]` is the length and `s[1]`…`s[s[0]]`
are the characters. This is the layout of `G2_LOAD(..., string, ...)` data, but not of C string
literals, which end with a 0 instead.

### `<math.h>`

| Function | Accuracy |
|---|---|
| `fabsf`, `fminf`, `fmaxf`, `copysignf`, `floorf`, `ceilf`, `truncf`, `roundf`, `fmodf` | exact; the small ones are macros that compile inline |
| `sqrtf` | exact (rounded down); 0 for negative input |
| `hypotf` | within 1/65536 up to 128, and about 3·10⁻⁷ relative above |
| `sinf`, `cosf`, `tanf` | within 8·10⁻⁵ for any angle (`tanf` near ±π/2 is less accurate) |
| `atan2f`, `atanf` | within 5·10⁻⁵ |

`M_PI`, `M_PI_2`, `M_PI_4`, `M_E` and `M_SQRT2` are defined. The `double` names (`sqrt`, `sin`, …)
are macros for the `float` functions. Approximate costs, in executed instructions: `floorf` 2,
`sinf` ~170, `sqrtf` ~220, `atan2f` ~250, `hypotf` ~380.

### Other headers

`<stddef.h>` (`NULL`, `size_t`, `ptrdiff_t`, `offsetof`), `<stdbool.h>`, `<limits.h>` (every
integer type has the range of `int`), `<float.h>` (`FLT_MAX`, `FLT_EPSILON`, …), `<stdalign.h>`
and `<stdnoreturn.h>`. The host's system headers are never searched, so `<stdio.h>` and friends
don't exist.

## What isn't supported

Each of these is reported as an error at the offending code:

- `unsigned` types and unsigned literals
- recursion, function pointers, variadic functions, and calls deeper than the 16-entry call stack
- passing or returning structs and unions by value, struct values in `?:`, and bit-fields
- variable-length arrays and `alloca`
- `_Atomic`, `_Thread_local`, inline `asm`, computed `goto` and labels as values
- wide character and string literals (`L`, `u`, `U`)
- `_Static_assert` (chibicc never supported it)
- calls to functions without a definition, and more than one `.c` file
- `%`, bitwise operators, subscripts and `switch` on floating values
- names that aren't plain ASCII identifiers (g2 assembly only allows letters, digits and `_`)

## Compiler options

```
chibicc-g2 [-o <file>] [-E] [-I<dir>] [-D<name>[=<value>]] [-U<name>]
           [-mmemory=<slots>] [-fno-peephole] [-fno-const-pool] [-fno-overlay] <file.c>
```

| Option | |
|---|---|
| `-o <file>` | output file (default: `<name>.g2` in the current directory) |
| `-E` | print the preprocessed source instead of compiling |
| `-I`, `-D`, `-U`, `-include`, `-idirafter`, `-M`, `-MD`, `-MF`, `-MT`, `-MQ`, `-MP`, `-MMD` | as in GCC |
| `-mmemory=N` | set `#memory` to `N` slots; it's an error if the program needs more |
| `-fno-peephole` | turn off the peephole optimiser |
| `-fno-const-pool` | use immediate instructions instead of a pool of constant slots |
| `-fno-overlay` | give every function its own memory instead of sharing between functions that can't be active together |

`-S`, `-O*`, `-W*`, `-g*`, `-std=*`, `-w`, `-ffreestanding` and `-fno-builtin` are accepted and
ignored, so build systems that pass them still work.

## Reading the output

The generated assembly is meant to be read. Each statement is preceded by its source line as a
comment, and every memory location has a name:

| Name | Meaning |
|---|---|
| `g_name` | the global `name` |
| `l_func__name` | the parameter or local `name` in `func` |
| `t_func__N` | a temporary in `func` |
| `__kN`, `__kmN` | the constant pool slot holding N or −N |
| `__strN`, `__anonN` | string literals and `static` locals |
| `fn_name` | the code of function `name` |
| `__LN_*`, `__CN` | internal labels |

A short excerpt:

```g2
fn_tick:
    ; if (RIGHT) x = (x + 1) % WIDTH;
    jnea __L0_end g_RIGHT 1
    add t_tick__0 g_x 1
    mod g_x t_tick__0 __k64
__L0_end:
```

Addresses of array elements and struct members are written as numbers, with a comment that says
what they are (`; g_arr+3`).

## Testing

```sh
make test
```

`make test` compiles every program in `test-g2/`, assembles it with the real `g2a` and runs it in
`tools/g2run.py`, a headless version of the VM. Tests call `ASSERT(expected, actual)` from
`start()`. A test whose first line is `// expect-error: <text>` must fail to compile with that
message. `tools/run_tests.py --cflags="-fno-overlay"` runs the suite with other options, and
`tools/g2run.py` can run any program for a number of frames, hold keys, and print memory and
pixels (`--help` lists the options).

`test-g2/fixed.c` and `test-g2/math.c` are generated by `tools/gen_fixed_test.py` and
`tools/gen_math_test.py`. Design notes are in `g2-port-plan.md` and `g2-float-plan.md`.

## Credits

chibicc-g2 is based on [chibicc](https://github.com/rui314/chibicc) by Rui Ueyama, a small C11
compiler written as the reference implementation for a book about compilers. Its tokenizer,
preprocessor and parser are largely unchanged, and it's well worth reading. Like chibicc,
chibicc-g2 is released under the MIT license (see `LICENSE`).
