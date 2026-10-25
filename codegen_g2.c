// This file implements the g2 backend. See g2-port-plan.md for the design.
//
// g2 has no registers, and every instruction operand (except ldi's value)
// is a memory address. So instead of a stack machine, this backend gives
// every variable, parameter and temporary a fixed memory address (a
// "compiled stack"), and uses variables directly as instruction operands.
// Recursion is therefore not allowed.
//
// Memory layout:
//
//   0..31   reserved by the VM (constants 0/1/-1, keys, delta time,
//           return register 13, pseudo-instruction scratch 14, call stack)
//   32..    globals, then one frame per function (params, locals, temps)
//
// Each frame of the program starts at instruction 0, where the compiler
// emits an entry stub that runs start() once and tick() every frame.

#include "chibicc.h"

#define FIRST_FREE_ADDR 32
#define CALL_STACK_SIZE 16

// Reserved slots that hold constants and the return value.
#define SLOT_ZERO "0"
#define SLOT_ONE "1"
#define SLOT_MINUS_ONE "2"
#define SLOT_RET "13"

// Round up `n` to the nearest multiple of `align`. For instance,
// align_to(5, 8) returns 8 and align_to(11, 8) returns 16.
int align_to(int n, int align) {
  return (n + align - 1) / align * align;
}

//
// Per-function state
//

typedef struct FnInfo FnInfo;
struct FnInfo {
  Obj *fn;

  // Generated code, buffered because the frame size isn't known until
  // the whole function has been compiled.
  char *code;
  size_t code_len;

  int base;    // address of the frame's first slot
  int nlocals; // slots for params and locals
  int ntemps;  // slots for temporaries

  // Call graph
  FnInfo **callees;
  Token **call_toks;
  int ncallees;
  int callees_cap;

  int visit; // DFS state: 0 = new, 1 = on stack, 2 = done
  int depth; // longest call chain starting at this function
};

static FnInfo **fns;
static int nfns;
static HashMap fn_infos; // function name -> FnInfo

static FnInfo *current;
static bool unreachable;  // true after ret/ja until the next label
static int label_count;

static bool *temp_used;
static int temp_cap;

static HashMap asm_names;    // every emitted define or label name
static HashMap global_names; // C name -> allocated global Obj

// Numeric operands that address part of a variable, such as "45" for
// arr[12], mapped to a description ("g_arr+12") for comments.
static HashMap addr_comments;

//
// Output helpers
//

static void emit(char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Returns a comment naming the variables behind numeric address operands
// in `line`, or NULL. Immediate values are not addresses, so the last
// operand of ldi and of the *i pseudo-instructions is skipped, as are the
// labels of jumps.
static char *address_comment(char *mnemonic, char **ops, int n) {
  int first = 0, last = n;
  if (!strcmp(mnemonic, "ldi") || !strcmp(mnemonic, "addi") || !strcmp(mnemonic, "subi") ||
      !strcmp(mnemonic, "muli") || !strcmp(mnemonic, "divi") || !strcmp(mnemonic, "modi"))
    last = n - 1;
  if (!strcmp(mnemonic, "jnea") || !strcmp(mnemonic, "ja") || !strcmp(mnemonic, "call"))
    first = 1;

  char *comment = NULL;
  for (int i = first; i < last; i++) {
    char *desc = hashmap_get(&addr_comments, ops[i]);
    bool seen = false;
    for (int j = first; j < i; j++)
      seen = seen || !strcmp(ops[j], ops[i]);
    if (desc && !seen)
      comment = comment ? format("%s, %s", comment, desc) : desc;
  }
  return comment;
}

// Generated code is collected as a list of lines, so that the peephole
// optimizer (see below) can work on it before it is printed.
typedef enum { LINE_INSN, LINE_LABEL, LINE_COMMENT } LineKind;

typedef struct {
  LineKind kind;
  char *text;    // label name or comment text (for instructions, see below)
  char *comment; // a comment written with the instruction, or NULL

  // Instruction
  char *op;
  char *args[3];
  int nargs;
} Line;

static Line *lines;
static int nlines;
static int lines_cap;

static Line *push_line(LineKind kind) {
  if (nlines == lines_cap) {
    lines_cap = lines_cap ? lines_cap * 2 : 256;
    lines = realloc(lines, sizeof(Line) * lines_cap);
  }
  Line *l = &lines[nlines++];
  *l = (Line){.kind = kind};
  return l;
}

static void emit(char *fmt, ...) {
  // Code after an unconditional jump is never executed.
  if (unreachable)
    return;

  va_list ap;
  va_start(ap, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  Line *l = push_line(LINE_INSN);
  char *comment = strstr(buf, " ; ");
  if (comment) {
    l->comment = strdup(comment + 3);
    *comment = '\0';
  }

  char *text = strdup(buf);
  l->op = strtok(text, " ");
  for (char *t = strtok(NULL, " "); t && l->nargs < 3; t = strtok(NULL, " "))
    l->args[l->nargs++] = t;

  if (!strcmp(l->op, "ret") || !strcmp(l->op, "ja"))
    unreachable = true;
}

static void emit_label(char *name) {
  push_line(LINE_LABEL)->text = name;
  unreachable = false;
}

static char *new_label(char *what) {
  return format("__L%d_%s", label_count++, what);
}

// chibicc names its internal labels ".L..N". Labels in g2 assembly must
// be identifiers.
static char *chibicc_label(char *name) {
  if (strncmp(name, ".L..", 4))
    unreachable();
  return format("__C%s", name + 4);
}

static File *last_comment_file;
static int last_comment_line;

// Emits the source line containing `tok` as a comment, once per line.
static void emit_source_line(Token *tok) {
  while (tok->origin)
    tok = tok->origin;
  if (!tok->file || unreachable)
    return;
  if (tok->file == last_comment_file && tok->line_no == last_comment_line)
    return;
  last_comment_file = tok->file;
  last_comment_line = tok->line_no;

  char *start = tok->loc;
  while (start > tok->file->contents && start[-1] != '\n')
    start--;
  while (*start == ' ' || *start == '\t')
    start++;
  char *end = start;
  while (*end && *end != '\n')
    end++;
  while (end > start && isspace(end[-1]))
    end--;

  push_line(LINE_COMMENT)->text = format("%.*s", (int)(end - start), start);
}

static noreturn void unsupported(Node *node, char *what) {
  error_tok(node->tok, "%s are not supported yet by the g2 backend", what);
}

//
// Names
//

static bool is_asm_ident(char *s) {
  if (!*s || isdigit(*s))
    return false;
  for (; *s; s++)
    if (!isalnum(*s) && *s != '_')
      return false;
  return true;
}

// Returns `base`, or `base_2`, `base_3`, ... if it is already taken.
// All names start with a prefix (g_, l_, t_, fn_ or __), which keeps them
// clear of assembler keywords. The assembler's lexer would otherwise split
// a name such as "loader" into the keyword "load" and "er".
static char *unique_asm_name(char *base) {
  char *name = base;
  for (int i = 2; hashmap_get(&asm_names, name); i++)
    name = format("%s_%d", base, i);
  hashmap_put(&asm_names, name, (void *)1);
  return name;
}

static char *c_ident(char *name) {
  if (!is_asm_ident(name))
    error("identifier '%s' can't be used on g2: names in g2 assembly may only"
          " contain letters, digits and underscores", name);
  return name;
}

//
// Operands
//

// The location of a value: a memory slot, or a compile-time constant
// that the consumer turns into an immediate or a slot.
typedef struct {
  bool is_imm;
  int32_t imm;
  char *name; // slot (define name or address) if !is_imm
  int temp;   // index of the temporary that holds the value, or -1
  char *sym;  // for an address constant, the define that names it
} Operand;

static Operand imm(int64_t val) {
  return (Operand){true, wrap32(val), NULL, -1, NULL};
}

// The address of a variable, which is a compile-time constant. `sym` is
// printed instead of the number, e.g. `ldi t g_arr`.
static Operand imm_sym(int64_t val, char *sym) {
  return (Operand){true, wrap32(val), NULL, -1, sym};
}

static char *imm_text(Operand op) {
  return op.sym ? op.sym : format("%d", op.imm);
}

static Operand slot(char *name) {
  return (Operand){false, 0, name, -1, NULL};
}

static bool same_slot(Operand a, Operand b) {
  return !a.is_imm && !b.is_imm && !strcmp(a.name, b.name);
}

static Operand new_temp(void) {
  int i = 0;
  while (i < temp_cap && temp_used[i])
    i++;

  if (i == temp_cap) {
    int cap = temp_cap ? temp_cap * 2 : 16;
    temp_used = realloc(temp_used, cap * sizeof(bool));
    memset(temp_used + temp_cap, 0, (cap - temp_cap) * sizeof(bool));
    temp_cap = cap;
  }

  temp_used[i] = true;
  if (current->ntemps < i + 1)
    current->ntemps = i + 1;
  return (Operand){false, 0, format("t_%s__%d", current->fn->name, i), i, NULL};
}

static void free_op(Operand op) {
  if (op.temp >= 0)
    temp_used[op.temp] = false;
}

// The constant pool: each constant that is needed in a slot gets a slot
// of its own, set once at startup by the entry stub. Instructions then
// use it like a variable. Without the pool, a constant is loaded into a
// temporary (`ldi`) every time, and arithmetic uses the pseudo-
// instructions addi, muli, ..., which expand to two primitives.
typedef struct {
  char *name;  // define name, e.g. __k32
  char *value; // the value as text, a number or a define name
  int addr;
} PoolEntry;

static PoolEntry *pool;
static int npool;
static HashMap pool_names; // value text -> index + 1

static char *pool_slot(int32_t val, char *sym) {
  char *value = sym ? sym : format("%d", val);
  int i = (int)(intptr_t)hashmap_get(&pool_names, value) - 1;
  if (i >= 0)
    return pool[i].name;

  char *name;
  if (sym)
    name = format("__k_%s", sym);
  else if (val < 0)
    name = format("__km%lld", -(long long)val);
  else
    name = format("__k%d", val);

  pool = realloc(pool, sizeof(PoolEntry) * (npool + 1));
  pool[npool] = (PoolEntry){name, value};
  hashmap_put(&pool_names, value, (void *)(intptr_t)(npool + 1));
  npool++;
  return name;
}

// Returns a slot that holds the value of `op`. The constants 0, 1 and -1
// live in reserved slots. Other constants come from the constant pool, or
// are loaded into a temporary.
static Operand to_slot(Operand op) {
  if (!op.is_imm)
    return op;
  if (op.imm == 0 && !op.sym)
    return slot(SLOT_ZERO);
  if (op.imm == 1 && !op.sym)
    return slot(SLOT_ONE);
  if (op.imm == -1 && !op.sym)
    return slot(SLOT_MINUS_ONE);
  if (opt_const_pool)
    return slot(pool_slot(op.imm, op.sym));

  Operand t = new_temp();
  emit("ldi %s %s", t.name, imm_text(op));
  return t;
}

// Emits `d = a op k` for op add, mul, div or mod.
static void emit_imm_op(char *op, char *d, char *a, int32_t k) {
  if (!opt_const_pool) {
    emit("%si %s %s %d", op, d, a, k);
    return;
  }
  emit("%s %s %s %s", op, d, a, to_slot(imm(k)).name);
}

// The destination of an operation: the caller's hint if there is one,
// otherwise a new temporary.
static Operand dest(Operand *hint) {
  return hint ? *hint : new_temp();
}

static void move(Operand to, Operand from) {
  if (same_slot(to, from))
    return;
  if (from.is_imm)
    emit("ldi %s %s", to.name, imm_text(from));
  else
    emit("cpy %s %s", to.name, from.name);
  free_op(from);
}

//
// Expressions
//

static Operand gen_expr(Node *node, Operand *hint);
static void gen_cond(Node *node, bool jump_if, char *target);
static void gen_stmt(Node *node);

static Node *strip_casts(Node *node) {
  while (node->kind == ND_CAST)
    node = node->lhs;
  return node;
}

// chibicc rewrites `x op= y`, `x++` and `x--` to go through a pointer to
// `x`: `tmp = &x, *tmp = *tmp op y`. This matches the `tmp = &x` part.
static bool is_compound_assign(Node *node) {
  return node->kind == ND_ASSIGN && node->lhs->kind == ND_VAR &&
         !*node->lhs->var->name && strip_casts(node->rhs)->kind == ND_ADDR;
}

static bool const_int(Node *node, int64_t *val) {
  int64_t a, b;
  switch (node->kind) {
  case ND_NUM:
    *val = wrap32(node->val);
    return true;
  case ND_CAST:
    // chibicc's usual arithmetic conversion casts the integer operand of
    // pointer arithmetic to the pointer type.
    if (!is_numeric(node->ty) && node->ty->kind != TY_PTR)
      return false;
    if (node->ty->kind == TY_BOOL || !const_int(node->lhs, val))
      return false;
    *val = convert_const(node, node->lhs->ty, *val);
    return true;
  case ND_NEG:
    if (!const_int(node->lhs, &a))
      return false;
    *val = wrap32(-(uint64_t)a);
    return true;
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
    if (!const_int(node->lhs, &a) || !const_int(node->rhs, &b))
      return false;
    if (node->kind == ND_ADD)
      *val = wrap32((uint64_t)a + (uint64_t)b);
    else if (node->kind == ND_SUB)
      *val = wrap32((uint64_t)a - (uint64_t)b);
    else if (is_flonum(node->ty))
      *val = fix_mul(a, b);
    else
      *val = wrap32((uint64_t)a * (uint64_t)b);
    return true;
  }
  return false;
}

// Evaluates `node` into `to`.
static void gen_expr_to(Node *node, Operand to) {
  move(to, gen_expr(node, &to));
}

// Evaluates `node` for its side effects only.
static void gen_void(Node *node) {
  // `x++` is `(x += 1) - 1`. The subtraction is useless if the value is
  // unused.
  Node *n = node;
  int64_t k;
  while (n->kind == ND_CAST)
    n = n->lhs;
  if ((n->kind == ND_ADD || n->kind == ND_SUB) && const_int(n->rhs, &k)) {
    Node *lhs = n->lhs;
    while (lhs->kind == ND_CAST)
      lhs = lhs->lhs;
    if (lhs->kind == ND_COMMA && is_compound_assign(lhs->lhs)) {
      gen_void(lhs);
      return;
    }
  }

  free_op(gen_expr(node, NULL));
}

static bool has_call(Node *node) {
  if (!node)
    return false;
  if (node->kind == ND_FUNCALL && !(node->lhs->kind == ND_VAR && node->lhs->var->intrinsic))
    return true;
  if (has_call(node->lhs) || has_call(node->rhs) || has_call(node->cond) ||
      has_call(node->then) || has_call(node->els) || has_call(node->init) ||
      has_call(node->inc))
    return true;
  for (Node *n = node->body; n; n = n->next)
    if (has_call(n))
      return true;
  for (Node *n = node->args; n; n = n->next)
    if (has_call(n))
      return true;
  return false;
}

// Finds the allocated global that a declaration refers to. A declaration
// may be a separate Obj from the definition, e.g. `extern int x;`.
static void resolve_global(Obj *var, Token *tok) {
  Obj *def = hashmap_get(&global_names, var->name);
  if (!def)
    error_tok(tok, "'%s' is declared but never defined (g2 has no linker)", var->name);
  var->asm_name = def->asm_name;
  var->addr = def->addr;
  var->is_load = def->is_load;
}

//
// L-values
//

// The location of an object: either an address known at compile time,
// or an address computed at run time and held in a slot.
typedef struct {
  bool direct;
  int addr;      // direct: absolute address
  Obj *var;      // direct: the variable that contains the address, if any
  Operand ptr;   // indirect: slot holding the address
  int offset;    // indirect: added to the address held in `ptr`
  bool borrowed; // indirect: `ptr` belongs to a binding
} LValue;

// While compiling `x op= y`, chibicc's temporary pointer to `x` is bound
// to the l-value of `x`, so that `x`'s address is computed only once and
// direct variables don't go through a pointer at all.
typedef struct Binding Binding;
struct Binding {
  Binding *next;
  Obj *var;
  LValue lv;
};

static Binding *bindings;

static void ensure_resolved(Obj *var, Token *tok) {
  if (!var->asm_name)
    resolve_global(var, tok);
}

// The operand that names a direct l-value's slot.
static char *direct_name(LValue *lv) {
  Obj *var = lv->var;
  if (var && lv->addr == var->addr)
    return var->asm_name;
  char *name = format("%d", lv->addr);
  if (var)
    hashmap_put(&addr_comments, name, format("%s+%d", var->asm_name, lv->addr - var->addr));
  return name;
}

static void free_lval(LValue *lv) {
  if (!lv->direct && !lv->borrowed)
    free_op(lv->ptr);
}

// Returns a slot that holds the exact address of an indirect l-value.
static Operand lval_ptr(LValue *lv) {
  if (lv->offset) {
    if (!lv->borrowed)
      free_op(lv->ptr);
    Operand t = new_temp();
    emit_imm_op("add", t.name, lv->ptr.name, lv->offset);
    lv->ptr = t;
    lv->offset = 0;
    lv->borrowed = false;
  }
  return lv->ptr;
}

static bool const_addr(Node *node, LValue *lv);

// If the object that `node` designates has an address known at compile
// time, stores it in `lv` and returns true. No code is emitted.
static bool const_lval(Node *node, LValue *lv) {
  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->var;
    if (var->is_function)
      return false;
    ensure_resolved(var, node->tok);
    // The assembler decides where loaded data goes.
    if (var->is_load)
      return false;
    *lv = (LValue){.direct = true, .addr = var->addr, .var = var};
    return true;
  }
  case ND_MEMBER:
    if (!const_lval(node->lhs, lv))
      return false;
    lv->addr += node->member->offset;
    return true;
  case ND_DEREF:
    return const_addr(node->lhs, lv);
  }
  return false;
}

// If the pointer value `node` is a compile-time constant, stores the
// object it points to in `lv` and returns true.
static bool const_addr(Node *node, LValue *lv) {
  int64_t k;
  switch (node->kind) {
  case ND_ADDR:
    return const_lval(node->lhs, lv);
  case ND_VAR:
  case ND_DEREF:
  case ND_MEMBER:
    // An array decays to the address of its first element.
    if (node->ty->kind == TY_ARRAY)
      return const_lval(node, lv);
    return false;
  case ND_CAST:
    if (const_addr(node->lhs, lv))
      return true;
    if (node->ty->kind == TY_PTR && const_int(node->lhs, &k)) {
      *lv = (LValue){.direct = true, .addr = k};
      return true;
    }
    return false;
  case ND_ADD:
  case ND_SUB:
    if (!const_addr(node->lhs, lv) || !const_int(node->rhs, &k))
      return false;
    lv->addr += (node->kind == ND_ADD) ? k : -k;
    return true;
  }
  return false;
}

static LValue gen_lval(Node *node) {
  LValue lv = {};
  if (const_lval(node, &lv))
    return lv;

  switch (node->kind) {
  case ND_VAR: {
    Obj *var = node->var;
    if (var->is_function)
      error_tok(node->tok, "function pointers are not supported on g2");
    // Loaded data, whose address only the assembler knows.
    Operand t = new_temp();
    emit("ldi %s %s", t.name, var->asm_name);
    return (LValue){.ptr = t};
  }
  case ND_DEREF: {
    if (node->lhs->kind == ND_VAR) {
      for (Binding *b = bindings; b; b = b->next) {
        if (b->var == node->lhs->var) {
          lv = b->lv;
          lv.borrowed = true;
          return lv;
        }
      }
    }

    Operand p = gen_expr(node->lhs, NULL);
    if (p.is_imm)
      return (LValue){.direct = true, .addr = p.imm};
    return (LValue){.ptr = p};
  }
  case ND_MEMBER:
    lv = gen_lval(node->lhs);
    if (lv.direct)
      lv.addr += node->member->offset;
    else
      lv.offset += node->member->offset;
    return lv;
  }

  error_tok(node->tok, "not an lvalue");
}

// Reads the value of an l-value.
static Operand load_lval(LValue lv, Operand *hint) {
  if (lv.direct)
    return slot(direct_name(&lv));

  Operand p = lval_ptr(&lv);
  free_lval(&lv);
  Operand d = dest(hint);
  emit("rdi %s %s", d.name, p.name);
  return d;
}

// The address of the object `node` designates, as a value. This is also
// the value of an array, which decays to a pointer.
static Operand gen_address(Node *node, Operand *hint) {
  if (node->kind == ND_VAR && node->var->is_function)
    error_tok(node->tok, "function pointers are not supported on g2");

  // Loaded data
  if (node->kind == ND_VAR) {
    ensure_resolved(node->var, node->tok);
    if (node->var->is_load) {
      Operand d = dest(hint);
      emit("ldi %s %s", d.name, node->var->asm_name);
      return d;
    }
  }

  LValue lv = gen_lval(node);
  if (lv.direct) {
    if (lv.var && lv.addr == lv.var->addr)
      return imm_sym(lv.addr, lv.var->asm_name);
    return imm(lv.addr);
  }

  Operand p = lval_ptr(&lv);
  if (lv.borrowed)
    p.temp = -1;
  return p;
}

static Operand gen_var(Node *node) {
  Obj *var = node->var;
  if (var->is_function)
    error_tok(node->tok, "function pointers are not supported on g2");
  if (var->is_addr_tmp)
    unreachable();
  ensure_resolved(var, node->tok);
  return slot(var->asm_name);
}

// The address of slot `i` of an l-value, as an operand for rdi/sti.
static Operand slot_addr(LValue *lv, int i) {
  Operand p = lval_ptr(lv);
  if (i == 0)
    return p;
  Operand t = new_temp();
  emit_imm_op("add", t.name, p.name, i);
  return t;
}

// The l-value of a struct or union expression.
static LValue gen_struct_lval(Node *node) {
  switch (node->kind) {
  case ND_VAR:
  case ND_DEREF:
  case ND_MEMBER:
    return gen_lval(node);
  case ND_COMMA:
    gen_void(node->lhs);
    return gen_struct_lval(node->rhs);
  case ND_ASSIGN:
    // a = b = c
    free_op(gen_expr(node, NULL));
    return gen_struct_lval(node->lhs);
  }
  unsupported(node, "struct and union values other than variables and"
              " dereferences");
}

// Copies a struct or union slot by slot.
static Operand gen_struct_assign(Node *node) {
  int size = node->ty->size;
  LValue dst = gen_lval(node->lhs);
  LValue src = gen_struct_lval(node->rhs);

  if (!dst.direct)
    lval_ptr(&dst);
  if (!src.direct)
    lval_ptr(&src);

  if ((dst.direct && src.direct) || size <= 8) {
    for (int i = 0; i < size; i++) {
      Operand v;
      if (src.direct) {
        LValue s = src;
        s.addr += i;
        v = slot(direct_name(&s));
      } else {
        Operand a = slot_addr(&src, i);
        v = new_temp();
        emit("rdi %s %s", v.name, a.name);
        if (i)
          free_op(a);
      }

      if (dst.direct) {
        LValue d = dst;
        d.addr += i;
        move(slot(direct_name(&d)), v);
      } else {
        Operand a = slot_addr(&dst, i);
        emit("sti %s %s", a.name, v.name);
        if (i)
          free_op(a);
        free_op(v);
      }
    }
  } else {
    // A loop for large copies through pointers.
    Operand p = new_temp(), q = new_temp(), end = new_temp(), v = new_temp();
    if (src.direct)
      emit("ldi %s %d", p.name, src.addr);
    else
      emit("cpy %s %s", p.name, src.ptr.name);
    if (dst.direct)
      emit("ldi %s %d", q.name, dst.addr);
    else
      emit("cpy %s %s", q.name, dst.ptr.name);
    emit_imm_op("add", end.name, q.name, size);

    char *loop = new_label("copy");
    emit_label(loop);
    emit("rdi %s %s", v.name, p.name);
    emit("sti %s %s", q.name, v.name);
    emit("inc %s", p.name);
    emit("inc %s", q.name);
    emit("jnea %s %s %s", loop, q.name, end.name);
    free_op(p);
    free_op(q);
    free_op(end);
    free_op(v);
  }

  free_lval(&dst);
  free_lval(&src);
  return imm(0);
}

static Operand gen_assign(Node *node, Operand *hint) {
  if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
    return gen_struct_assign(node);

  LValue lv = gen_lval(node->lhs);
  if (lv.direct) {
    Operand s = slot(direct_name(&lv));
    gen_expr_to(node->rhs, s);
    return s;
  }

  Operand p = lval_ptr(&lv);
  Operand v = to_slot(gen_expr(node->rhs, NULL));
  emit("sti %s %s", p.name, v.name);
  free_lval(&lv);
  return v;
}

// `tmp = &x, *tmp = *tmp op y`, from `x op= y`.
static Operand gen_compound_assign(Node *node, Operand *hint) {
  LValue lv = gen_lval(strip_casts(node->lhs->rhs)->lhs);
  if (!lv.direct)
    lval_ptr(&lv);

  Binding b = {bindings, node->lhs->lhs->var, lv};
  bindings = &b;
  Operand r = gen_expr(node->rhs, hint);
  bindings = b.next;

  free_lval(&lv);
  return r;
}

static bool refers_to(Node *node, Obj *var) {
  if (!node)
    return false;
  if (node->kind == ND_VAR && node->var == var)
    return true;
  if (refers_to(node->lhs, var) || refers_to(node->rhs, var) ||
      refers_to(node->cond, var) || refers_to(node->then, var) ||
      refers_to(node->els, var) || refers_to(node->init, var) ||
      refers_to(node->inc, var))
    return true;
  for (Node *n = node->body; n; n = n->next)
    if (refers_to(n, var))
      return true;
  for (Node *n = node->args; n; n = n->next)
    if (refers_to(n, var))
      return true;
  return false;
}

// Marks the slots of `var` that the initializer `node` assigns. Returns
// false if the initializer could observe the slots before they are
// assigned, because it calls a function or reads `var`.
static bool init_covers(Node *node, Obj *var, bool *covered) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return true;
  case ND_COMMA:
    return init_covers(node->lhs, var, covered) && init_covers(node->rhs, var, covered);
  case ND_ASSIGN: {
    LValue lv;
    if (!node->is_init || !const_lval(node->lhs, &lv))
      return false;
    if (has_call(node->rhs) || refers_to(node->rhs, var))
      return false;
    for (int i = 0; i < node->ty->size; i++)
      covered[lv.addr - var->addr + i] = true;
    return true;
  }
  }
  return false;
}

// Clears a local array or struct before its initializer runs, except
// for the slots in `covered`, which the initializer assigns anyway.
static void gen_memzero(Obj *var, bool *covered) {
  int size = var->ty->size;
  int first = 0, last = size, n = 0;
  while (first < size && covered[first])
    first++;
  while (last > first && covered[last - 1])
    last--;
  for (int i = first; i < last; i++)
    n += !covered[i];

  if (n <= 32) {
    for (int i = first; i < last; i++) {
      if (covered[i])
        continue;
      LValue lv = {.direct = true, .addr = var->addr + i, .var = var};
      emit("ldi %s 0", direct_name(&lv));
    }
    return;
  }

  char *loop = new_label("zero");
  Operand p = new_temp();
  Operand end = new_temp();
  emit("ldi %s %d", p.name, var->addr + first);
  emit("ldi %s %d", end.name, var->addr + last);
  emit_label(loop);
  emit("sti %s %s", p.name, SLOT_ZERO);
  emit("inc %s", p.name);
  emit("jnea %s %s %s", loop, p.name, end.name);
  free_op(p);
  free_op(end);
}

// Evaluates a binary operator on two constants with g2 semantics.
static int64_t fold(Node *node, int64_t a, int64_t b) {
  switch (node->kind) {
  case ND_ADD: return wrap32((uint64_t)a + (uint64_t)b);
  case ND_SUB: return wrap32((uint64_t)a - (uint64_t)b);
  case ND_MUL:
    if (is_flonum(node->ty))
      return fix_mul(a, b);
    return wrap32((uint64_t)a * (uint64_t)b);
  case ND_DIV:
    if (is_flonum(node->ty))
      return fix_div(node, a, b);
    return floor_div(node, a, b);
  case ND_MOD: return floor_mod(node, a, b);
  case ND_BITAND: return a & b;
  case ND_BITOR: return a | b;
  case ND_BITXOR: return a ^ b;
  case ND_SHL:
  case ND_SHR:
    if (b < 0 || b > 31)
      error_tok(node->rhs->tok, "shift count out of range (0..31): %ld", (long)b);
    return node->kind == ND_SHL ? wrap32((uint64_t)a << b) : a >> b;
  case ND_EQ: return a == b;
  case ND_NE: return a != b;
  case ND_LT: return a < b;
  case ND_LE: return a <= b;
  }
  unreachable();
}

// Converts between an integer and a Q16.16 float: int -> float multiplies
// by 65536 and float -> int divides by it, which rounds toward negative
// infinity.
static Operand gen_fixed_conversion(Node *node, Operand *hint) {
  Operand a = gen_expr(node->lhs, NULL);
  if (a.is_imm)
    return imm(convert_const(node, node->lhs->ty, a.imm));
  free_op(a);
  Operand d = dest(hint);
  emit_imm_op(is_flonum(node->ty) ? "mul" : "div", d.name, a.name, FIX_ONE);
  return d;
}

static Operand gen_arith(Node *node, Operand *hint) {
  Operand a = gen_expr(node->lhs, NULL);
  Operand b = gen_expr(node->rhs, NULL);

  if (a.is_imm && b.is_imm)
    return imm(fold(node, a.imm, b.imm));

  NodeKind kind = node->kind;

  // Floating + and - are integer operations on the raw Q16.16 values,
  // but * and / need a rescaling step.
  if (is_flonum(node->ty) && (kind == ND_MUL || kind == ND_DIV))
    unsupported(node, "floating-point multiplication and division");

  // Put the constant of a commutative operator on the right.
  if ((kind == ND_ADD || kind == ND_MUL) && a.is_imm) {
    Operand tmp = a;
    a = b;
    b = tmp;
  }

  if (b.is_imm) {
    int32_t k = b.imm;
    Operand d;

    switch (kind) {
    case ND_ADD:
    case ND_SUB:
      if (kind == ND_SUB)
        k = wrap32(-(int64_t)k);
      if (k == 0)
        return a;
      free_op(a);
      d = dest(hint);
      if (same_slot(d, a) && k == 1)
        emit("inc %s", d.name);
      else if (same_slot(d, a) && k == -1)
        emit("dec %s", d.name);
      else if (k < 0 && k != INT32_MIN && !opt_const_pool)
        emit("subi %s %s %d", d.name, a.name, -k);
      else
        emit_imm_op("add", d.name, a.name, k);
      return d;
    case ND_MUL:
      if (k == 0) {
        free_op(a);
        return imm(0);
      }
      if (k == 1)
        return a;
      free_op(a);
      d = dest(hint);
      if (k == -1)
        emit("sub %s %s %s", d.name, SLOT_ZERO, a.name);
      else
        emit_imm_op("mul", d.name, a.name, k);
      return d;
    case ND_DIV:
      if (k == 0)
        error_tok(node->rhs->tok, "division by zero");
      if (k == 1)
        return a;
      free_op(a);
      d = dest(hint);
      if (k == -1)
        emit("sub %s %s %s", d.name, SLOT_ZERO, a.name);
      else
        emit_imm_op("div", d.name, a.name, k);
      return d;
    case ND_MOD:
      if (k == 0)
        error_tok(node->rhs->tok, "division by zero");
      free_op(a);
      if (k == 1 || k == -1)
        return imm(0);
      d = dest(hint);
      emit_imm_op("mod", d.name, a.name, k);
      return d;
    }
  }

  // `0 - x` is a negation.
  if (kind == ND_SUB && a.is_imm && a.imm == 0) {
    free_op(b);
    Operand d = dest(hint);
    emit("sub %s %s %s", d.name, SLOT_ZERO, b.name);
    return d;
  }

  a = to_slot(a);
  b = to_slot(b);
  free_op(a);
  free_op(b);
  Operand d = dest(hint);

  switch (kind) {
  case ND_ADD: emit("add %s %s %s", d.name, a.name, b.name); break;
  case ND_SUB: emit("sub %s %s %s", d.name, a.name, b.name); break;
  case ND_MUL: emit("mul %s %s %s", d.name, a.name, b.name); break;
  case ND_DIV: emit("div %s %s %s", d.name, a.name, b.name); break;
  case ND_MOD: emit("mod %s %s %s", d.name, a.name, b.name); break;
  default: unreachable();
  }
  return d;
}

// Comparisons produce 0 or 1 without branching. `cmp` gives 1, 0 or -1
// for <, == and >, and those are mapped to 0/1 arithmetically. Raw
// values are never multiplied, because x * x can overflow to 0.
static Operand gen_compare(Node *node, Operand *hint) {
  Operand a = gen_expr(node->lhs, NULL);
  Operand b = gen_expr(node->rhs, NULL);

  if (a.is_imm && b.is_imm)
    return imm(fold(node, a.imm, b.imm));

  a = to_slot(a);
  b = to_slot(b);
  free_op(a);
  free_op(b);
  Operand d = dest(hint);

  emit("cmp %s %s %s", d.name, a.name, b.name);
  switch (node->kind) {
  case ND_LT: // (cmp + 1) / 2 maps 1, 0, -1 to 1, 0, 0
    emit("inc %s", d.name);
    emit_imm_op("div", d.name, d.name, 2);
    break;
  case ND_LE: // (cmp + 2) / 2 maps 1, 0, -1 to 1, 1, 0
    emit_imm_op("add", d.name, d.name, 2);
    emit_imm_op("div", d.name, d.name, 2);
    break;
  case ND_EQ: // 1 - cmp^2
    emit("mul %s %s %s", d.name, d.name, d.name);
    emit("sub %s %s %s", d.name, SLOT_ONE, d.name);
    break;
  case ND_NE: // cmp^2
    emit("mul %s %s %s", d.name, d.name, d.name);
    break;
  default:
    unreachable();
  }
  return d;
}

// Computes `x != 0` (as 0 or 1), or `x == 0` if `negate` is true.
static Operand gen_truth(Node *node, bool negate, Operand *hint) {
  Operand a = gen_expr(node, NULL);
  if (a.is_imm)
    return imm((a.imm != 0) != negate);

  free_op(a);
  Operand d = dest(hint);
  emit("cmp %s %s %s", d.name, a.name, SLOT_ZERO);
  emit("mul %s %s %s", d.name, d.name, d.name);
  if (negate)
    emit("sub %s %s %s", d.name, SLOT_ONE, d.name);
  return d;
}

// && and || as values. The destination is only written after the whole
// condition has been evaluated, so it may be a variable the condition uses.
static Operand gen_logical(Node *node, Operand *hint) {
  char *f = new_label("false");
  char *end = new_label("end");

  gen_cond(node, false, f);
  Operand d = dest(hint);
  emit("ldi %s 1", d.name);
  emit("ja %s", end);
  emit_label(f);
  emit("ldi %s 0", d.name);
  emit_label(end);
  return d;
}

static Operand gen_ternary(Node *node, Operand *hint) {
  char *els = new_label("else");
  char *end = new_label("end");

  if (node->ty->kind == TY_VOID) {
    gen_cond(node->cond, false, els);
    gen_void(node->then);
    emit("ja %s", end);
    emit_label(els);
    gen_void(node->els);
    emit_label(end);
    return imm(0);
  }

  // Both branches must leave the result in the same slot.
  Operand d = dest(hint);
  gen_cond(node->cond, false, els);
  gen_expr_to(node->then, d);
  emit("ja %s", end);
  emit_label(els);
  gen_expr_to(node->els, d);
  emit_label(end);
  return d;
}

//
// Bitwise operators
//
// g2 has no bitwise instructions. With a constant operand, shifts and
// many masks are a multiplication, division or remainder by a power of
// two, because g2's division rounds toward negative infinity:
//
//   x << k   x * 2^k
//   x >> k   x / 2^k (an arithmetic shift)
//   x & m    x % (m + 1)            if m + 1 is a power of two
//   x & -m   x / m * m              if m is a power of two
//
// Everything else is rewritten into a call to a runtime helper in
// __g2rt.h (see lower_bitwise).

static bool is_bitwise(NodeKind kind) {
  return kind == ND_BITAND || kind == ND_BITOR || kind == ND_BITXOR ||
         kind == ND_SHL || kind == ND_SHR;
}

static bool is_commutative(NodeKind kind) {
  return kind == ND_BITAND || kind == ND_BITOR || kind == ND_BITXOR;
}

// Returns log2(k) if k is a power of two, or -1.
static int log2_exact(int64_t k) {
  if (k <= 0 || (k & (k - 1)))
    return -1;
  int n = 0;
  while (k > 1) {
    k /= 2;
    n++;
  }
  return n;
}

// Returns true if `kind` with the constant operand `k` compiles inline.
static bool bitwise_inline(NodeKind kind, int64_t k) {
  switch (kind) {
  case ND_SHL:
  case ND_SHR:
    return true; // out-of-range counts are reported by gen_bitwise
  case ND_BITAND: {
    if (k == 0 || k == -1)
      return true;
    int n = (k > 0) ? log2_exact(k + 1) : log2_exact(-k);
    return 0 < n && n <= 30;
  }
  case ND_BITOR:
  case ND_BITXOR:
    return k == 0 || k == -1;
  }
  unreachable();
}

static bool bitwise_needs_helper(Node *node) {
  int64_t a, b;
  bool lhs_const = const_int(node->lhs, &a);
  bool rhs_const = const_int(node->rhs, &b);
  if (lhs_const && rhs_const)
    return false;
  if (rhs_const && bitwise_inline(node->kind, b))
    return false;
  if (lhs_const && is_commutative(node->kind) && bitwise_inline(node->kind, a))
    return false;
  return true;
}

static Operand gen_bitwise(Node *node, Operand *hint) {
  Operand a = gen_expr(node->lhs, NULL);
  Operand b = gen_expr(node->rhs, NULL);

  if (a.is_imm && b.is_imm)
    return imm(fold(node, a.imm, b.imm));

  if (is_commutative(node->kind) && a.is_imm) {
    Operand tmp = a;
    a = b;
    b = tmp;
  }

  // lower_bitwise turned every other case into a helper call.
  if (!b.is_imm || !bitwise_inline(node->kind, b.imm))
    unreachable();

  int32_t k = b.imm;
  Operand d;

  switch (node->kind) {
  case ND_SHL:
  case ND_SHR:
    if (k < 0 || k > 31)
      error_tok(node->rhs->tok, "shift count out of range (0..31): %d", k);
    if (k == 0)
      return a;
    free_op(a);
    d = dest(hint);
    if (node->kind == ND_SHL && k <= 30) {
      emit_imm_op("mul", d.name, a.name, 1 << k);
    } else if (node->kind == ND_SHL) {
      // x << 31 keeps only the lowest bit, which becomes the sign bit.
      emit_imm_op("mod", d.name, a.name, 2);
      emit_imm_op("mul", d.name, d.name, INT32_MIN);
    } else if (k <= 30) {
      emit_imm_op("div", d.name, a.name, 1 << k);
    } else {
      // 2^31 isn't representable, so divide twice.
      emit_imm_op("div", d.name, a.name, 1 << 30);
      emit_imm_op("div", d.name, d.name, 2);
    }
    return d;
  case ND_BITAND:
    if (k == 0) {
      free_op(a);
      return imm(0);
    }
    if (k == -1)
      return a;
    free_op(a);
    d = dest(hint);
    if (k > 0) {
      emit_imm_op("mod", d.name, a.name, k + 1);
    } else {
      emit_imm_op("div", d.name, a.name, -k);
      emit_imm_op("mul", d.name, d.name, -k);
    }
    return d;
  case ND_BITOR:
    if (k == 0)
      return a;
    free_op(a);
    return imm(-1);
  case ND_BITXOR:
    if (k == 0)
      return a;
    free_op(a);
    d = dest(hint);
    emit("sub %s %s %s", d.name, SLOT_MINUS_ONE, a.name);
    return d;
  }
  unreachable();
}

static HashMap helper_fns; // name -> Obj for the functions in __g2rt.h

static char *helper_name(NodeKind kind) {
  switch (kind) {
  case ND_BITAND: return "__g2_and";
  case ND_BITOR: return "__g2_or";
  case ND_BITXOR: return "__g2_xor";
  case ND_SHL: return "__g2_shl";
  case ND_SHR: return "__g2_shr";
  }
  unreachable();
}

// Rewrites bitwise operators that can't be compiled inline into calls to
// the runtime helpers, and marks the helpers live. This happens before
// any code is generated, so the helpers get frames and appear in the
// call graph like any other function.
static bool lower_bitwise(Node *node) {
  if (!node)
    return false;

  bool changed = false;
  changed |= lower_bitwise(node->lhs);
  changed |= lower_bitwise(node->rhs);
  changed |= lower_bitwise(node->cond);
  changed |= lower_bitwise(node->then);
  changed |= lower_bitwise(node->els);
  changed |= lower_bitwise(node->init);
  changed |= lower_bitwise(node->inc);
  for (Node *n = node->body; n; n = n->next)
    changed |= lower_bitwise(n);
  for (Node *n = node->args; n; n = n->next)
    changed |= lower_bitwise(n);

  if (!is_bitwise(node->kind) || !bitwise_needs_helper(node))
    return changed;

  char *name = helper_name(node->kind);
  Obj *fn = hashmap_get(&helper_fns, name);
  if (!fn)
    error_tok(node->tok, "this operator needs the runtime helper %s, which is"
              " defined in __g2rt.h", name);

  Node *callee = calloc(1, sizeof(Node));
  callee->kind = ND_VAR;
  callee->var = fn;
  callee->tok = node->tok;
  callee->ty = fn->ty;

  Node *a = node->lhs;
  Node *b = node->rhs;
  a->next = b;
  b->next = NULL;

  node->kind = ND_FUNCALL;
  node->lhs = callee;
  node->rhs = NULL;
  node->args = a;
  node->func_ty = fn->ty;

  bool was_live = fn->is_live;
  fn->is_live = true;
  return changed || !was_live;
}

//
// Unused globals
//

// Marks the globals that live code refers to. Anonymous globals (string
// literals and static locals) and static globals that aren't used are not
// emitted, so e.g. the library's random number state costs nothing in a
// program that doesn't use it.
static void mark_used(Node *node) {
  if (!node)
    return;
  if (node->kind == ND_VAR && !node->var->is_local)
    node->var->is_used = true;

  mark_used(node->lhs);
  mark_used(node->rhs);
  mark_used(node->cond);
  mark_used(node->then);
  mark_used(node->els);
  mark_used(node->init);
  mark_used(node->inc);
  for (Node *n = node->body; n; n = n->next)
    mark_used(n);
  for (Node *n = node->args; n; n = n->next)
    mark_used(n);
}

static bool is_anonymous(Obj *var) {
  return !strncmp(var->name, ".L..", 4);
}

static bool droppable(Obj *var) {
  return is_anonymous(var) || var->is_static;
}

static FnInfo *find_fn_info(Obj *fn) {
  return hashmap_get(&fn_infos, fn->name);
}

static void add_call_edge(FnInfo *callee, Token *tok) {
  for (int i = 0; i < current->ncallees; i++)
    if (current->callees[i] == callee)
      return;

  if (current->ncallees == current->callees_cap) {
    current->callees_cap = current->callees_cap ? current->callees_cap * 2 : 8;
    current->callees = realloc(current->callees, sizeof(FnInfo *) * current->callees_cap);
    current->call_toks = realloc(current->call_toks, sizeof(Token *) * current->callees_cap);
  }
  current->callees[current->ncallees] = callee;
  current->call_toks[current->ncallees] = tok;
  current->ncallees++;
}

// An intrinsic is a single instruction whose operands are the arguments.
static Operand gen_intrinsic(Node *node, Obj *fn) {
  Operand args[3];
  int n = 0;
  for (Node *arg = node->args; arg; arg = arg->next)
    args[n++] = to_slot(gen_expr(arg, NULL));

  if (n == 3)
    emit("%s %s %s %s", fn->intrinsic, args[0].name, args[1].name, args[2].name);
  else
    emit("%s %s %s", fn->intrinsic, args[0].name, args[1].name);

  for (int i = 0; i < n; i++)
    free_op(args[i]);
  return imm(0);
}

static Operand gen_funcall(Node *node, Operand *hint) {
  if (node->lhs->kind != ND_VAR || !node->lhs->var->is_function)
    error_tok(node->tok, "calling through a function pointer is not supported on g2");

  Obj *fn = node->lhs->var;
  Token *tok = node->lhs->tok;

  if (fn->intrinsic)
    return gen_intrinsic(node, fn);
  if (!strcmp(fn->name, "start") || !strcmp(fn->name, "tick"))
    error_tok(tok, "%s() is called by the program's entry stub and can't be"
              " called directly", fn->name);
  if (!fn->is_definition)
    error_tok(tok, "call to undefined function '%s' (g2 has no linker; define"
              " it or include its header)", fn->name);

  add_call_edge(find_fn_info(fn), tok);

  // Arguments are written to the callee's parameter slots. If evaluating
  // an argument calls another function, that call could overwrite
  // parameters that were already written, so in that case all arguments
  // are evaluated before any of them is stored.
  bool calls = false;
  int nargs = 0;
  for (Node *arg = node->args; arg; arg = arg->next) {
    calls = calls || has_call(arg);
    nargs++;
  }

  if (!calls) {
    Obj *param = fn->params;
    for (Node *arg = node->args; arg; arg = arg->next, param = param->next)
      gen_expr_to(arg, slot(param->asm_name));
  } else {
    Operand *vals = calloc(nargs, sizeof(Operand));
    int i = 0;
    for (Node *arg = node->args; arg; arg = arg->next) {
      Operand v = gen_expr(arg, NULL);
      // A variable could be changed by a later argument's call, so keep
      // a copy of anything that isn't a constant or a temporary.
      if (!v.is_imm && v.temp < 0 && arg->next) {
        Operand t = new_temp();
        emit("cpy %s %s", t.name, v.name);
        v = t;
      }
      vals[i++] = v;
    }

    Obj *param = fn->params;
    for (i = 0; i < nargs; i++, param = param->next)
      move(slot(param->asm_name), vals[i]);
  }

  emit("call %s", fn->asm_name);

  if (fn->ty->return_ty->kind == TY_VOID)
    return imm(0);

  // Slot 13 is overwritten by the next call, so copy the result out.
  if (hint) {
    move(*hint, slot(SLOT_RET));
    return *hint;
  }
  Operand t = new_temp();
  emit("cpy %s %s", t.name, SLOT_RET);
  return t;
}

static Operand gen_expr(Node *node, Operand *hint) {
  switch (node->kind) {
  case ND_NULL_EXPR:
    return imm(0);
  case ND_NUM:
    return imm(node->val);
  case ND_VAR:
  case ND_DEREF:
  case ND_MEMBER:
    if (node->ty->kind == TY_ARRAY)
      return gen_address(node, hint);
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
      unsupported(node, "struct and union values");
    if (node->kind == ND_VAR)
      return gen_var(node);
    return load_lval(gen_lval(node), hint);
  case ND_ADDR:
    return gen_address(node->lhs, hint);
  case ND_CAST:
    if (node->ty->kind == TY_VOID) {
      gen_void(node->lhs);
      return imm(0);
    }
    if (node->ty->kind == TY_BOOL)
      return gen_truth(node->lhs, false, hint);
    if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
      unsupported(node, "struct and union values");
    if (is_flonum(node->ty) != is_flonum(node->lhs->ty))
      return gen_fixed_conversion(node, hint);
    // All other scalar types are 32-bit integers (or floats converted to
    // floats), so the cast is a no-op.
    return gen_expr(node->lhs, hint);
  case ND_COMMA:
    if (is_compound_assign(node->lhs))
      return gen_compound_assign(node, hint);
    if (node->lhs->kind == ND_MEMZERO) {
      Obj *var = node->lhs->var;
      bool *covered = calloc(var->ty->size, sizeof(bool));
      if (!init_covers(node->rhs, var, covered))
        memset(covered, 0, var->ty->size * sizeof(bool));
      gen_memzero(var, covered);
      return gen_expr(node->rhs, hint);
    }
    gen_void(node->lhs);
    return gen_expr(node->rhs, hint);
  case ND_ASSIGN:
    return gen_assign(node, hint);
  case ND_STMT_EXPR: {
    Node *n = node->body;
    if (!n)
      return imm(0);
    for (; n->next; n = n->next)
      gen_stmt(n);
    if (n->kind == ND_EXPR_STMT) {
      emit_source_line(n->tok);
      return gen_expr(n->lhs, hint);
    }
    gen_stmt(n);
    return imm(0);
  }
  case ND_MEMZERO:
    gen_memzero(node->var, calloc(node->var->ty->size, sizeof(bool)));
    return imm(0);
  case ND_ADD:
  case ND_SUB:
  case ND_MUL:
  case ND_DIV:
  case ND_MOD:
    return gen_arith(node, hint);
  case ND_NEG: {
    Operand a = gen_expr(node->lhs, NULL);
    if (a.is_imm)
      return imm(-(int64_t)a.imm);
    free_op(a);
    Operand d = dest(hint);
    emit("sub %s %s %s", d.name, SLOT_ZERO, a.name);
    return d;
  }
  case ND_BITNOT: {
    // ~x == -1 - x in two's complement.
    Operand a = gen_expr(node->lhs, NULL);
    if (a.is_imm)
      return imm(~(int64_t)a.imm);
    free_op(a);
    Operand d = dest(hint);
    emit("sub %s %s %s", d.name, SLOT_MINUS_ONE, a.name);
    return d;
  }
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
    return gen_compare(node, hint);
  case ND_NOT:
    return gen_truth(node->lhs, true, hint);
  case ND_LOGAND:
  case ND_LOGOR:
    return gen_logical(node, hint);
  case ND_COND:
    return gen_ternary(node, hint);
  case ND_FUNCALL:
    return gen_funcall(node, hint);
  case ND_BITAND:
  case ND_BITOR:
  case ND_BITXOR:
  case ND_SHL:
  case ND_SHR:
    return gen_bitwise(node, hint);
  }

  error_tok(node->tok, "invalid expression");
}

//
// Conditions
//

// Returns true if the value of `node` is always 0 or 1. Testing such a
// value for false takes one jump (x != 1) instead of two (x == 0).
static bool is_zero_or_one(Node *node) {
  int64_t k;
  switch (node->kind) {
  case ND_VAR:
  case ND_DEREF:
  case ND_MEMBER:
  case ND_CAST:
    return node->ty->kind == TY_BOOL;
  case ND_MOD:
    // x % 2 is 0 or 1 even for negative x, because g2's mod takes the
    // sign of the divisor.
    return const_int(node->rhs, &k) && k == 2;
  case ND_BITAND:
    return (const_int(node->rhs, &k) && k == 1) || (const_int(node->lhs, &k) && k == 1);
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE:
  case ND_NOT:
  case ND_LOGAND:
  case ND_LOGOR:
    return true;
  }
  return false;
}

// Jumps to `target` if `a == b`. g2 only has "jump if not equal", so this
// jumps over an unconditional jump instead.
static void jump_if_equal(char *target, Operand a, Operand b) {
  char *skip = new_label("skip");
  emit("jnea %s %s %s", skip, a.name, b.name);
  emit("ja %s", target);
  emit_label(skip);
}

// Jumps to `target` if the truth value of `node` equals `jump_if`, and
// falls through otherwise. This never materializes a 0/1 value.
static void gen_cond(Node *node, bool jump_if, char *target) {
  switch (node->kind) {
  case ND_NUM:
    if ((node->val != 0) == jump_if)
      emit("ja %s", target);
    return;
  case ND_NOT:
    gen_cond(node->lhs, !jump_if, target);
    return;
  case ND_CAST:
    // Casts between integer types (and to _Bool) keep the truth value.
    if (node->ty->kind != TY_VOID && node->ty->kind != TY_STRUCT &&
        node->ty->kind != TY_UNION) {
      gen_cond(node->lhs, jump_if, target);
      return;
    }
    break;
  case ND_LOGAND:
    if (jump_if) {
      char *skip = new_label("skip");
      gen_cond(node->lhs, false, skip);
      gen_cond(node->rhs, true, target);
      emit_label(skip);
    } else {
      gen_cond(node->lhs, false, target);
      gen_cond(node->rhs, false, target);
    }
    return;
  case ND_LOGOR:
    if (jump_if) {
      gen_cond(node->lhs, true, target);
      gen_cond(node->rhs, true, target);
    } else {
      char *skip = new_label("skip");
      gen_cond(node->lhs, true, skip);
      gen_cond(node->rhs, false, target);
      emit_label(skip);
    }
    return;
  case ND_EQ:
  case ND_NE:
  case ND_LT:
  case ND_LE: {
    Operand a = gen_expr(node->lhs, NULL);
    Operand b = gen_expr(node->rhs, NULL);
    if (a.is_imm && b.is_imm) {
      if ((fold(node, a.imm, b.imm) != 0) == jump_if)
        emit("ja %s", target);
      return;
    }
    // `cmp` gives 1, 0 or -1. "a < b" (cmp == 1) can only be tested
    // directly for false, and "a <= b" (cmp != -1) only for true; the
    // other direction needs two jumps. Against a constant, pick the form
    // that suits the jump: a < k is a <= k - 1, and a <= k is a < k + 1.
    NodeKind kind = node->kind;
    if (kind == ND_LT && jump_if) {
      if (b.is_imm && !b.sym && b.imm != INT32_MIN) {
        b = imm(b.imm - 1);
        kind = ND_LE;
      } else if (a.is_imm && !a.sym && a.imm != INT32_MAX) {
        a = imm(a.imm + 1);
        kind = ND_LE;
      }
    } else if (kind == ND_LE && !jump_if) {
      if (b.is_imm && !b.sym && b.imm != INT32_MAX) {
        b = imm(b.imm + 1);
        kind = ND_LT;
      } else if (a.is_imm && !a.sym && a.imm != INT32_MIN) {
        a = imm(a.imm - 1);
        kind = ND_LT;
      }
    }

    a = to_slot(a);
    b = to_slot(b);
    free_op(a);
    free_op(b);

    if (node->kind == ND_EQ || node->kind == ND_NE) {
      if ((node->kind == ND_EQ) == jump_if)
        jump_if_equal(target, a, b);
      else
        emit("jnea %s %s %s", target, a.name, b.name);
      return;
    }

    Operand t = new_temp();
    emit("cmp %s %s %s", t.name, a.name, b.name);
    free_op(t);

    if (kind == ND_LT) {
      // a < b iff cmp == 1
      if (jump_if)
        jump_if_equal(target, t, slot(SLOT_ONE));
      else
        emit("jnea %s %s %s", target, t.name, SLOT_ONE);
    } else {
      // a <= b iff cmp != -1
      if (jump_if)
        emit("jnea %s %s %s", target, t.name, SLOT_MINUS_ONE);
      else
        jump_if_equal(target, t, slot(SLOT_MINUS_ONE));
    }
    return;
  }
  }

  Operand x = gen_expr(node, NULL);
  if (x.is_imm) {
    if ((x.imm != 0) == jump_if)
      emit("ja %s", target);
    return;
  }
  free_op(x);
  if (jump_if)
    emit("jnea %s %s %s", target, x.name, SLOT_ZERO);
  else if (is_zero_or_one(node))
    emit("jnea %s %s %s", target, x.name, SLOT_ONE); // false iff x != 1
  else
    jump_if_equal(target, x, slot(SLOT_ZERO));
}

//
// Statements
//

static void gen_stmt(Node *node) {
  switch (node->kind) {
  case ND_IF: {
    emit_source_line(node->tok);
    char *end = new_label("end");
    if (!node->els) {
      gen_cond(node->cond, false, end);
      gen_stmt(node->then);
      emit_label(end);
      return;
    }
    char *els = new_label("else");
    gen_cond(node->cond, false, els);
    gen_stmt(node->then);
    emit("ja %s", end);
    emit_label(els);
    gen_stmt(node->els);
    emit_label(end);
    return;
  }
  case ND_FOR: {
    // The condition is tested at the bottom, so each iteration takes one
    // conditional jump instead of a conditional and an unconditional one.
    emit_source_line(node->tok);
    char *body = new_label("loop");
    char *cond = new_label("cond");
    if (node->init)
      gen_stmt(node->init);
    if (node->cond)
      emit("ja %s", cond);
    emit_label(body);
    gen_stmt(node->then);
    emit_label(chibicc_label(node->cont_label));
    if (node->inc)
      gen_void(node->inc);
    emit_label(cond);
    if (node->cond)
      gen_cond(node->cond, true, body);
    else
      emit("ja %s", body);
    emit_label(chibicc_label(node->brk_label));
    return;
  }
  case ND_DO: {
    char *loop = new_label("loop");
    emit_label(loop);
    gen_stmt(node->then);
    emit_label(chibicc_label(node->cont_label));
    emit_source_line(node->cond->tok);
    gen_cond(node->cond, true, loop);
    emit_label(chibicc_label(node->brk_label));
    return;
  }
  case ND_SWITCH: {
    emit_source_line(node->tok);
    Operand v = to_slot(gen_expr(node->cond, NULL));

    // chibicc lists the cases last first.
    int ncases = 0;
    for (Node *c = node->case_next; c; c = c->case_next)
      ncases++;
    Node **cases = calloc(ncases, sizeof(Node *));
    int i = ncases;
    for (Node *c = node->case_next; c; c = c->case_next)
      cases[--i] = c;

    for (i = 0; i < ncases; i++) {
      Node *cs = cases[i];
      char *label = chibicc_label(cs->label);
      if (cs->begin == cs->end) {
        Operand k = to_slot(imm(cs->begin));
        jump_if_equal(label, v, k);
        free_op(k);
        continue;
      }

      // [GNU] case begin ... end
      char *next = new_label("next");
      Operand t = new_temp();
      Operand lo = to_slot(imm(cs->begin));
      emit("cmp %s %s %s", t.name, v.name, lo.name);
      free_op(lo);
      jump_if_equal(next, t, slot(SLOT_ONE)); // v < begin
      Operand hi = to_slot(imm(cs->end));
      emit("cmp %s %s %s", t.name, v.name, hi.name);
      free_op(hi);
      emit("jnea %s %s %s", label, t.name, SLOT_MINUS_ONE); // v <= end
      emit_label(next);
      free_op(t);
    }

    free_op(v);
    if (node->default_case)
      emit("ja %s", chibicc_label(node->default_case->label));
    else
      emit("ja %s", chibicc_label(node->brk_label));

    gen_stmt(node->then);
    emit_label(chibicc_label(node->brk_label));
    return;
  }
  case ND_CASE:
    emit_label(chibicc_label(node->label));
    gen_stmt(node->lhs);
    return;
  case ND_BLOCK:
    for (Node *n = node->body; n; n = n->next)
      gen_stmt(n);
    return;
  case ND_GOTO:
    emit("ja %s", chibicc_label(node->unique_label));
    return;
  case ND_LABEL:
    emit_label(chibicc_label(node->unique_label));
    gen_stmt(node->lhs);
    return;
  case ND_RETURN:
    emit_source_line(node->tok);
    if (node->lhs) {
      if (current->fn->ty->return_ty->kind == TY_VOID)
        gen_void(node->lhs);
      else
        gen_expr_to(node->lhs, slot(SLOT_RET));
    }
    emit("ret");
    return;
  case ND_EXPR_STMT:
    emit_source_line(node->tok);
    gen_void(node->lhs);
    return;
  }

  error_tok(node->tok, "invalid statement");
}

//
// Peephole optimizer
//
// Works on the lines of one function. Most rules only concern
// temporaries (t_*), which are local to the function and never have
// their address taken, so their liveness can be computed exactly.

static bool is_insn(Line *l, char *op) {
  return l->kind == LINE_INSN && !strcmp(l->op, op);
}

static bool is_jump(Line *l) {
  return is_insn(l, "ja") || is_insn(l, "jnea");
}

static bool is_temp(char *name) {
  return !strncmp(name, "t_", 2);
}

// Labels that other code (the entry stub, calls) refers to.
static bool is_protected_label(char *name) {
  return !strncmp(name, "fn_", 3) || !strncmp(name, "__g2_", 5);
}

// Which operands an instruction reads and writes, as bit masks.
static void operand_use(Line *l, int *reads, int *writes) {
  char *op = l->op;
  *reads = *writes = 0;
  if (!strcmp(op, "ldi")) {
    *writes = 1;
  } else if (!strcmp(op, "rdi") || !strcmp(op, "cpy") || !strcmp(op, "addi") ||
             !strcmp(op, "subi") || !strcmp(op, "muli") || !strcmp(op, "divi") ||
             !strcmp(op, "modi")) {
    *writes = 1;
    *reads = 2;
  } else if (!strcmp(op, "add") || !strcmp(op, "sub") || !strcmp(op, "mul") ||
             !strcmp(op, "div") || !strcmp(op, "mod") || !strcmp(op, "cmp")) {
    *writes = 1;
    *reads = 6;
  } else if (!strcmp(op, "inc") || !strcmp(op, "dec")) {
    *writes = 1;
    *reads = 1;
  } else if (!strcmp(op, "jnea")) {
    *reads = 6;
  } else if (!strcmp(op, "ja") || !strcmp(op, "call") || !strcmp(op, "ret")) {
    // no slot operands
  } else {
    // sti, col, pix: all operands are read
    *reads = (1 << l->nargs) - 1;
  }
}

static bool reads_operand(Line *l, int i) {
  int r, w;
  operand_use(l, &r, &w);
  return r & (1 << i);
}

// A "pure" write only writes its first operand, without reading it.
static bool is_pure_write(Line *l) {
  int r, w;
  operand_use(l, &r, &w);
  return w == 1 && !(r & 1);
}

static bool reads_slot(Line *l, char *name) {
  for (int i = 0; i < l->nargs; i++)
    if (reads_operand(l, i) && !strcmp(l->args[i], name))
      return true;
  return false;
}

static int find_label(char *name) {
  for (int i = 0; i < nlines; i++)
    if (lines[i].kind == LINE_LABEL && !strcmp(lines[i].text, name))
      return i;
  return -1;
}

// The first instruction at or after line i, or -1.
static int next_insn(int i) {
  for (; i < nlines; i++)
    if (lines[i].kind == LINE_INSN)
      return i;
  return -1;
}

static void delete_line(int i) {
  memmove(&lines[i], &lines[i + 1], sizeof(Line) * (nlines - i - 1));
  nlines--;
}

// Liveness of temporaries. live_out[i] is a bit set over temporary
// indexes, for instruction line i.
typedef struct {
  int ntemps;
  int words;
  uint64_t *live_out;
  HashMap index; // temp name -> index + 1
} Liveness;

static int temp_index(Liveness *lv, char *name) {
  return (int)(intptr_t)hashmap_get(&lv->index, name) - 1;
}

static void set_bit(uint64_t *set, int i) { set[i / 64] |= 1ull << (i % 64); }
static void clear_bit(uint64_t *set, int i) { set[i / 64] &= ~(1ull << (i % 64)); }
static bool test_bit(uint64_t *set, int i) { return set[i / 64] & (1ull << (i % 64)); }

static void compute_liveness(Liveness *lv) {
  *lv = (Liveness){};
  for (int i = 0; i < nlines; i++) {
    if (lines[i].kind != LINE_INSN)
      continue;
    for (int j = 0; j < lines[i].nargs; j++) {
      char *a = lines[i].args[j];
      if (is_temp(a) && !hashmap_get(&lv->index, a))
        hashmap_put(&lv->index, a, (void *)(intptr_t)(++lv->ntemps));
    }
  }
  lv->words = (lv->ntemps + 63) / 64 + 1;

  uint64_t *live_in = calloc(nlines * lv->words, sizeof(uint64_t));
  lv->live_out = calloc(nlines * lv->words, sizeof(uint64_t));
  int *target = calloc(nlines, sizeof(int));
  for (int i = 0; i < nlines; i++)
    if (is_jump(&lines[i]))
      target[i] = next_insn(find_label(lines[i].args[0]));

  for (bool changed = true; changed;) {
    changed = false;
    for (int i = nlines - 1; i >= 0; i--) {
      Line *l = &lines[i];
      if (l->kind != LINE_INSN)
        continue;
      uint64_t *out = &lv->live_out[i * lv->words];
      uint64_t *in = &live_in[i * lv->words];

      // Successors: the next instruction unless the flow stops here, and
      // the target of a jump.
      uint64_t tmp[lv->words];
      memset(tmp, 0, sizeof(tmp));
      if (!is_insn(l, "ja") && !is_insn(l, "ret")) {
        int n = next_insn(i + 1);
        if (n >= 0)
          for (int w = 0; w < lv->words; w++)
            tmp[w] |= live_in[n * lv->words + w];
      }
      if (is_jump(l) && target[i] >= 0)
        for (int w = 0; w < lv->words; w++)
          tmp[w] |= live_in[target[i] * lv->words + w];
      memcpy(out, tmp, sizeof(tmp));

      int r, wr;
      operand_use(l, &r, &wr);
      for (int j = 0; j < l->nargs; j++)
        if ((wr & (1 << j)) && is_temp(l->args[j]))
          clear_bit(tmp, temp_index(lv, l->args[j]));
      for (int j = 0; j < l->nargs; j++)
        if ((r & (1 << j)) && is_temp(l->args[j]))
          set_bit(tmp, temp_index(lv, l->args[j]));

      if (memcmp(in, tmp, sizeof(tmp))) {
        memcpy(in, tmp, sizeof(tmp));
        changed = true;
      }
    }
  }
}

static bool live_after(Liveness *lv, int i, char *temp) {
  return test_bit(&lv->live_out[i * lv->words], temp_index(lv, temp));
}

// The instruction right after line i, if no label is in between.
static int adjacent_insn(int i) {
  for (int j = i + 1; j < nlines; j++) {
    if (lines[j].kind == LINE_LABEL)
      return -1;
    if (lines[j].kind == LINE_INSN)
      return j;
  }
  return -1;
}

static bool peephole_pass(void) {
  bool changed = false;

  // Unreachable code after an unconditional jump or return.
  for (int i = 0; i < nlines; i++) {
    if (!is_insn(&lines[i], "ja") && !is_insn(&lines[i], "ret"))
      continue;
    while (i + 1 < nlines && lines[i + 1].kind != LINE_LABEL) {
      delete_line(i + 1);
      changed = true;
    }
  }

  // Jumps to the next instruction.
  for (int i = 0; i < nlines; i++) {
    if (!is_jump(&lines[i]))
      continue;
    int j = i + 1;
    bool next = false;
    for (; j < nlines && lines[j].kind != LINE_INSN; j++)
      if (lines[j].kind == LINE_LABEL && !strcmp(lines[j].text, lines[i].args[0]))
        next = true;
    if (next) {
      delete_line(i--);
      changed = true;
    }
  }

  // Jumps to a jump or a return.
  for (int i = 0; i < nlines; i++) {
    if (!is_jump(&lines[i]))
      continue;
    for (int hops = 0; hops < 16; hops++) {
      int t = next_insn(find_label(lines[i].args[0]) + 1);
      if (t < 0)
        break;
      if (is_insn(&lines[t], "ja") && strcmp(lines[t].args[0], lines[i].args[0])) {
        lines[i].args[0] = lines[t].args[0];
        changed = true;
        continue;
      }
      if (is_insn(&lines[i], "ja") && is_insn(&lines[t], "ret")) {
        lines[i].op = "ret";
        lines[i].nargs = 0;
        changed = true;
      }
      break;
    }
  }

  // Labels that nothing jumps to.
  for (int i = 0; i < nlines; i++) {
    if (lines[i].kind != LINE_LABEL || is_protected_label(lines[i].text))
      continue;
    bool used = false;
    for (int j = 0; j < nlines && !used; j++)
      used = is_jump(&lines[j]) && !strcmp(lines[j].args[0], lines[i].text);
    if (!used) {
      delete_line(i--);
      changed = true;
    }
  }

  // Copies of a slot to itself.
  for (int i = 0; i < nlines; i++) {
    if (is_insn(&lines[i], "cpy") && !strcmp(lines[i].args[0], lines[i].args[1])) {
      delete_line(i--);
      changed = true;
    }
  }

  // Rules that need the liveness of temporaries. Each rewrite only
  // changes the instructions it looks at, so several can be done with
  // the same liveness information as long as they don't overlap.
  Liveness lv;
  compute_liveness(&lv);
  bool *dead = calloc(nlines, sizeof(bool));
  bool *touched = calloc(nlines, sizeof(bool));

  for (int i = 0; i < nlines; i++) {
    Line *l = &lines[i];
    if (l->kind != LINE_INSN || touched[i])
      continue;

    int r, w;
    operand_use(l, &r, &w);
    char *dst = (w & 1) ? l->args[0] : NULL;

    // A temporary that is written but never read.
    if (dst && is_temp(dst) && !live_after(&lv, i, dst)) {
      dead[i] = touched[i] = true;
      continue;
    }

    int j = adjacent_insn(i);
    if (j < 0 || touched[j])
      continue;
    Line *next = &lines[j];

    // `op t, ...; cpy y, t` -> `op y, ...` if t isn't used afterwards
    if (dst && is_temp(dst) && is_pure_write(l) && is_insn(next, "cpy") &&
        !strcmp(next->args[1], dst) && strcmp(next->args[0], dst) &&
        !live_after(&lv, j, dst)) {
      l->args[0] = next->args[0];
      dead[j] = true;
      touched[i] = touched[j] = true;
      continue;
    }

    // `cpy t, x; op ..., t, ...` -> `op ..., x, ...` if t isn't used afterwards
    if (is_insn(l, "cpy") && is_temp(dst) && reads_slot(next, dst) &&
        !live_after(&lv, j, dst)) {
      int nr, nw;
      operand_use(next, &nr, &nw);
      if (!((nw & 1) && !strcmp(next->args[0], dst))) {
        for (int k = 0; k < next->nargs; k++)
          if ((nr & (1 << k)) && !strcmp(next->args[k], dst))
            next->args[k] = l->args[1];
        dead[i] = true;
        touched[i] = touched[j] = true;
        continue;
      }
    }
  }

  for (int i = nlines - 1; i >= 0; i--) {
    if (dead[i]) {
      delete_line(i);
      changed = true;
    }
  }
  return changed;
}

static void peephole(void) {
  if (!opt_peephole)
    return;
  while (peephole_pass())
    ;
}

// Prints the collected lines.
static void print_lines(FILE *o) {
  for (int i = 0; i < nlines; i++) {
    Line *l = &lines[i];
    if (l->kind == LINE_LABEL) {
      fprintf(o, "%s:\n", l->text);
      continue;
    }
    if (l->kind == LINE_COMMENT) {
      fprintf(o, "    ; %s\n", l->text);
      continue;
    }

    fprintf(o, "    %s", l->op);
    for (int j = 0; j < l->nargs; j++)
      fprintf(o, " %s", l->args[j]);
    char *comment = l->comment ? l->comment : address_comment(l->op, l->args, l->nargs);
    if (comment)
      fprintf(o, " ; %s", comment);
    fprintf(o, "\n");
  }
}

//
// Program
//

static FnInfo *new_fn_info(Obj *fn) {
  FnInfo *info = calloc(1, sizeof(FnInfo));
  info->fn = fn;
  hashmap_put(&fn_infos, fn->name, info);
  fns = realloc(fns, sizeof(FnInfo *) * (nfns + 1));
  fns[nfns++] = info;
  return info;
}

// Returns the locals of `fn` in declaration order, parameters first.
// The pointers that chibicc creates for `x op= y` are left out: the
// backend binds them to the l-value of `x` instead (gen_compound_assign).
static Obj **frame_vars(Obj *fn, int *n) {
  int nparams = 0, nlocals = 0;
  for (Obj *var = fn->params; var; var = var->next)
    nparams++;
  for (Obj *var = fn->locals; var != fn->params; var = var->next)
    if (!var->is_addr_tmp)
      nlocals++;

  Obj **vars = calloc(nparams + nlocals, sizeof(Obj *));
  int i = 0;
  for (Obj *var = fn->params; var; var = var->next)
    vars[i++] = var;

  // fn->locals lists the newest variable first and ends with the params.
  int j = nparams + nlocals;
  for (Obj *var = fn->locals; var != fn->params; var = var->next)
    if (!var->is_addr_tmp)
      vars[--j] = var;

  *n = nparams + nlocals;
  return vars;
}

static void name_frame(Obj *fn) {
  int n;
  Obj **vars = frame_vars(fn, &n);
  for (int i = 0; i < n; i++) {
    char *name = *vars[i]->name ? c_ident(vars[i]->name) : "anon";
    vars[i]->asm_name = unique_asm_name(format("l_%s__%s", fn->name, name));
  }
}

// Assigns addresses to the parameters and locals of a function. With
// overlaying, locals of sibling block scopes share memory: a scope's
// variables are placed after those of all enclosing scopes, and sibling
// scopes start at the same offset.
static void layout_locals(FnInfo *info) {
  Obj *fn = info->fn;
  int n;
  Obj **vars = frame_vars(fn, &n);

  if (!opt_overlay) {
    int offset = 0;
    for (int i = 0; i < n; i++) {
      vars[i]->addr = info->base + offset;
      offset += vars[i]->ty->size;
    }
    info->nlocals = offset;
    return;
  }

  int ns = fn->nscopes;
  int *size = calloc(ns, sizeof(int));
  int *start = calloc(ns, sizeof(int));
  int *next = calloc(ns, sizeof(int));
  for (int i = 0; i < n; i++)
    size[vars[i]->scope] += vars[i]->ty->size;

  // A parent scope always has a smaller index than its children.
  info->nlocals = 0;
  for (int i = 0; i < ns; i++) {
    int parent = fn->scope_parents[i];
    start[i] = next[i] = (parent < 0) ? 0 : start[parent] + size[parent];
    info->nlocals = MAX(info->nlocals, start[i] + size[i]);
  }

  for (int i = 0; i < n; i++) {
    vars[i]->addr = info->base + next[vars[i]->scope];
    next[vars[i]->scope] += vars[i]->ty->size;
  }
}

static void gen_function(FnInfo *info, int base) {
  Obj *fn = info->fn;
  current = info;
  info->base = base;
  info->ntemps = 0;
  info->ncallees = 0;
  layout_locals(info);

  if (temp_cap)
    memset(temp_used, 0, temp_cap * sizeof(bool));
  last_comment_file = NULL;

  nlines = 0;
  emit_label(fn->asm_name);
  gen_stmt(fn->body);
  emit("ret");
  peephole();

  FILE *o = open_memstream(&info->code, &info->code_len);
  print_lines(o);
  fclose(o);
}

// Reports recursion (a cycle in the call graph) and computes the longest
// call chain from each function.
static void check_calls(FnInfo *info, FnInfo **stack, int depth) {
  if (info->visit == 2)
    return;
  info->visit = 1;
  stack[depth] = info;

  for (int i = 0; i < info->ncallees; i++) {
    FnInfo *callee = info->callees[i];
    if (callee->visit == 1) {
      char *chain = "";
      for (int j = 0; j <= depth; j++)
        chain = format("%s%s -> ", chain, stack[j]->fn->name);
      error_tok(info->call_toks[i], "recursion is not supported on g2: %s%s",
                chain, callee->fn->name);
    }
    check_calls(callee, stack, depth + 1);
    info->depth = MAX(info->depth, callee->depth + 1);
  }

  info->visit = 2;
}

static char *call_chain(FnInfo *info) {
  char *chain = info->fn->name;
  while (info->ncallees) {
    FnInfo *next = info->callees[0];
    for (int i = 1; i < info->ncallees; i++)
      if (info->callees[i]->depth > next->depth)
        next = info->callees[i];
    info = next;
    chain = format("%s -> %s", chain, info->fn->name);
  }
  return chain;
}

static bool has_initializer(Obj *var) {
  if (var->rel)
    return true;
  if (!var->init_data)
    return false;
  for (int i = 0; i < var->ty->size; i++)
    if (var->init_data[i])
      return true;
  return false;
}

// The slot of `var` at `offset`, as an operand.
static char *slot_name(Obj *var, int offset) {
  if (offset == 0 && var->ty->size == 1)
    return var->asm_name;
  return format("%d", var->addr + offset);
}

// Emits code that sets the initial values of global variables. Memory
// starts zeroed, so only non-zero slots need an instruction.
static void emit_global_init(Obj **globals, int nglobals) {
  for (int i = 0; i < nglobals; i++) {
    Obj *var = globals[i];
    if (!has_initializer(var))
      continue;

    Relocation *rel = var->rel;
    for (int j = 0; j < var->ty->size; j++) {
      char *comment = (var->ty->size == 1) ? "" : format(" ; %s+%d", var->asm_name, j);

      if (rel && rel->offset == j) {
        Obj *target = hashmap_get(&global_names, *rel->label);
        if (!target)
          error("'%s' can't be used in an initializer: function pointers are"
                " not supported on g2, and other symbols must be defined",
                *rel->label);
        if (rel->addend == 0)
          emit("ldi %s %s%s", slot_name(var, j), target->asm_name, comment);
        else
          emit("ldi %s %ld%s ; %s%+ld", slot_name(var, j), target->addr + rel->addend,
               comment, target->asm_name, rel->addend);
        rel = rel->next;
        continue;
      }

      if (var->init_data && var->init_data[j])
        emit("ldi %s %d%s", slot_name(var, j), var->init_data[j], comment);
    }
  }
}

void codegen(Obj *prog, int width, int height, FILE *outfile) {
  // chibicc lists the newest declaration first. Process them in source order.
  int nobjs = 0;
  for (Obj *obj = prog; obj; obj = obj->next)
    nobjs++;
  Obj **objs = calloc(nobjs, sizeof(Obj *));
  int k = nobjs;
  for (Obj *obj = prog; obj; obj = obj->next)
    objs[--k] = obj;

  // Lower bitwise operators to helper calls. Repeat while new helpers
  // become live, since they need lowering too.
  for (int i = 0; i < nobjs; i++)
    if (objs[i]->is_function && objs[i]->is_definition && !strncmp(objs[i]->name, "__g2_", 5))
      hashmap_put(&helper_fns, objs[i]->name, objs[i]);

  for (bool changed = true; changed;) {
    changed = false;
    for (int i = 0; i < nobjs; i++) {
      Obj *fn = objs[i];
      if (fn->is_function && fn->is_definition && fn->is_live)
        changed |= lower_bitwise(fn->body);
    }
  }

  // Find the globals that are used, by live code or by the initializers
  // of used globals.
  HashMap all_globals = {};
  for (int i = 0; i < nobjs; i++)
    if (!objs[i]->is_function)
      hashmap_put(&all_globals, objs[i]->name, objs[i]);

  for (int i = 0; i < nobjs; i++)
    if (objs[i]->is_function && objs[i]->is_definition && objs[i]->is_live)
      mark_used(objs[i]->body);

  for (bool changed = true; changed;) {
    changed = false;
    for (int i = 0; i < nobjs; i++) {
      Obj *var = objs[i];
      if (var->is_function || (droppable(var) && !var->is_used))
        continue;
      for (Relocation *rel = var->rel; rel; rel = rel->next) {
        Obj *target = hashmap_get(&all_globals, *rel->label);
        if (target && !target->is_used) {
          target->is_used = true;
          changed = true;
        }
      }
    }
  }

  // Globals that need memory, in source order.
  Obj **globals = calloc(nobjs + 1, sizeof(Obj *));
  int nglobals = 0;
  bool need_init = false;
  Obj *start = NULL, *tick = NULL;

  for (int i = 0; i < nobjs; i++) {
    Obj *obj = objs[i];
    if (obj->is_function) {
      if (!obj->is_definition)
        continue;
      if (!strcmp(obj->name, "start"))
        start = obj;
      if (!strcmp(obj->name, "tick"))
        tick = obj;
      continue;
    }
    if (obj->has_fixed_addr || obj->is_load || !obj->is_definition)
      continue;
    if (droppable(obj) && !obj->is_used)
      continue;
    globals[nglobals++] = obj;
    need_init = need_init || has_initializer(obj);
  }

  Obj *entry_fns[] = {start, tick};
  for (int i = 0; i < 2; i++)
    if (entry_fns[i] && entry_fns[i]->params)
      error_tok(entry_fns[i]->params->ty->name, "%s() must not have parameters",
                entry_fns[i]->name);
  if (!tick)
    fprintf(stderr, "%s: warning: no tick() function; the program does nothing"
            " after start()\n", base_file);

  // Allocate global memory.
  int addr = FIRST_FREE_ADDR;
  bool has_once = need_init || start;
  char *started_flag = has_once ? unique_asm_name("__g2_started") : NULL;
  int started_addr = has_once ? addr++ : 0;

  int nstr = 0, nanon = 0;
  for (int i = 0; i < nglobals; i++) {
    Obj *var = globals[i];
    if (var->is_str_lit)
      var->asm_name = unique_asm_name(format("__str%d", nstr++));
    else if (is_anonymous(var))
      var->asm_name = unique_asm_name(format("__anon%d", nanon++));
    else
      var->asm_name = unique_asm_name(format("g_%s", c_ident(var->name)));
    var->addr = addr;
    addr += var->ty->size;
    if (!hashmap_get(&global_names, var->name))
      hashmap_put(&global_names, var->name, var);
  }

  // Data loaded by the assembler. It is placed after #memory, and the
  // assembler binds its name to the address.
  Obj **loads = calloc(nobjs + 1, sizeof(Obj *));
  int nloads = 0;
  for (int i = 0; i < nobjs; i++) {
    Obj *var = objs[i];
    if (var->is_function || !var->is_load)
      continue;
    Obj *prev = hashmap_get(&global_names, var->name);
    if (prev) {
      if (!prev->is_load || strcmp(prev->load_op, var->load_op) ||
          strcmp(prev->load_type, var->load_type) || strcmp(prev->load_data, var->load_data))
        error("conflicting declarations of loaded data '%s'", var->name);
      var->asm_name = prev->asm_name;
      continue;
    }
    var->asm_name = unique_asm_name(format("g_%s", c_ident(var->name)));
    var->addr = -1;
    hashmap_put(&global_names, var->name, var);
    loads[nloads++] = var;
  }

  // Variables bound to VM-managed memory, such as the key states.
  for (int i = 0; i < nobjs; i++) {
    Obj *var = objs[i];
    if (var->is_function || !var->has_fixed_addr || hashmap_get(&global_names, var->name))
      continue;
    var->asm_name = unique_asm_name(format("g_%s", c_ident(var->name)));
    var->addr = var->fixed_addr;
    hashmap_put(&global_names, var->name, var);
  }
  for (int i = 0; i < nobjs; i++) {
    Obj *var = objs[i];
    if (!var->is_function && var->has_fixed_addr && !var->asm_name)
      resolve_global(var, var->tok);
  }

  // Name every live function and its locals first, so that calls can
  // refer to functions that are compiled later.
  for (int i = 0; i < nobjs; i++) {
    Obj *fn = objs[i];
    if (!fn->is_function || !fn->is_definition || !fn->is_live)
      continue;
    fn->asm_name = unique_asm_name(format("fn_%s", c_ident(fn->name)));
    name_frame(fn);
    new_fn_info(fn);
  }

  // Compile start() and tick() first, then the rest in source order.
  // Frames are laid out one after another.
  FnInfo **order = calloc(nfns, sizeof(FnInfo *));
  int norder = 0;
  for (int i = 0; i < nfns; i++)
    if (fns[i]->fn == start)
      order[norder++] = fns[i];
  for (int i = 0; i < nfns; i++)
    if (fns[i]->fn == tick)
      order[norder++] = fns[i];
  for (int i = 0; i < nfns; i++)
    if (fns[i]->fn != start && fns[i]->fn != tick)
      order[norder++] = fns[i];

  // First pass: frames laid out one after another. This gives the size
  // of every frame and the call graph.
  int frames_begin = addr;
  int saved_label_count = label_count;
  for (int i = 0; i < norder; i++) {
    gen_function(order[i], addr);
    addr += order[i]->nlocals + order[i]->ntemps;
  }

  // The call graph must be acyclic, and every call chain must fit in the
  // VM's call stack. The entry stub's call to start() or tick() takes
  // one entry.
  FnInfo **stack = calloc(nfns + 1, sizeof(FnInfo *));
  for (int i = 0; i < norder; i++)
    check_calls(order[i], stack, 0);

  // Overlay frames. Functions that can't be active at the same time share
  // memory: every frame is placed above the frames of all its callers,
  // and start() and tick() (which the entry stub calls one after the
  // other) both begin at the bottom. Local addresses are compiled into the
  // code, so the functions are compiled again with their final addresses.
  // If a frame comes out bigger than in the previous pass (temporaries
  // can differ in rare cases), the layout is redone.
  if (opt_overlay) {
    int *planned = calloc(norder, sizeof(int));
    for (int i = 0; i < norder; i++)
      planned[i] = order[i]->nlocals + order[i]->ntemps;

    for (int round = 0;; round++) {
      if (round == 8)
        error("internal error: frame layout doesn't converge");

      int *base = calloc(norder, sizeof(int));
      for (int i = 0; i < norder; i++)
        base[i] = frames_begin;
      for (bool changed = true; changed;) {
        changed = false;
        for (int i = 0; i < norder; i++) {
          for (int j = 0; j < order[i]->ncallees; j++) {
            int k = 0;
            while (order[k] != order[i]->callees[j])
              k++;
            if (base[k] < base[i] + planned[i]) {
              base[k] = base[i] + planned[i];
              changed = true;
            }
          }
        }
      }

      label_count = saved_label_count;
      npool = 0;
      pool_names = (HashMap){};
      addr_comments = (HashMap){};
      for (int i = 0; i < norder; i++)
        gen_function(order[i], base[i]);

      bool fits = true;
      addr = frames_begin;
      for (int i = 0; i < norder; i++) {
        int size = order[i]->nlocals + order[i]->ntemps;
        if (size > planned[i]) {
          planned[i] = size;
          fits = false;
        }
        addr = MAX(addr, base[i] + planned[i]);
      }
      if (fits)
        break;
    }
  }
  for (int i = 0; i < norder; i++) {
    FnInfo *root = order[i];
    if (root->fn != start && root->fn != tick)
      continue;
    if (root->depth + 1 > CALL_STACK_SIZE)
      error("call depth %d exceeds the g2 call stack (%d): %s", root->depth + 1,
            CALL_STACK_SIZE, call_chain(root));
  }

  // The constant pool goes after the frames.
  for (int i = 0; i < npool; i++)
    pool[i].addr = addr++;
  if (npool && !has_once) {
    has_once = true;
    started_flag = unique_asm_name("__g2_started");
    started_addr = addr++;
  }

  int memory = addr;
  if (opt_memory) {
    if (opt_memory < memory)
      error("-mmemory=%d is too small: the program needs %d slots", opt_memory, memory);
    memory = opt_memory;
  }

  // Header
  FILE *o = outfile;
  fprintf(o, "; Generated by chibicc-g2 from %s\n", base_file);
  fprintf(o, "#width %d\n", width);
  fprintf(o, "#height %d\n", height);
  fprintf(o, "#memory %d\n", memory);

  // The assembler reads quoted data with the pattern (['"`])(.*)\1, so
  // use a quote character that the data doesn't contain.
  if (nloads)
    fprintf(o, "\n");
  for (int i = 0; i < nloads; i++) {
    Obj *var = loads[i];
    char *q = "\"";
    if (strchr(var->load_data, *q))
      q = "'";
    if (strchr(var->load_data, *q))
      q = "`";
    if (strchr(var->load_data, *q))
      error("loaded data for '%s' contains all of \", ' and `, which the g2"
            " assembler can't quote", var->name);
    fprintf(o, "load %s %s %s %s%s%s\n", var->asm_name, var->load_op, var->load_type,
            q, var->load_data, q);
  }

  // Memory map
  bool has_fixed = false;
  for (int i = 0; i < nobjs; i++)
    has_fixed = has_fixed || (!objs[i]->is_function && objs[i]->has_fixed_addr);

  if (has_once || nglobals || has_fixed) {
    fprintf(o, "\n; ---- globals ----\n");
    if (has_once)
      fprintf(o, "define %s %d ; set once start() has run\n", started_flag, started_addr);
    for (int i = 0; i < nglobals; i++) {
      Obj *var = globals[i];
      if (var->ty->size == 1)
        fprintf(o, "define %s %d\n", var->asm_name, var->addr);
      else
        fprintf(o, "define %s %d ; %d slots (%d..%d)\n", var->asm_name, var->addr,
                var->ty->size, var->addr, var->addr + var->ty->size - 1);
    }
    for (int i = 0; i < nobjs; i++) {
      Obj *var = objs[i];
      if (!var->is_function && var->has_fixed_addr && hashmap_get(&global_names, var->name) == var)
        fprintf(o, "define %s %d ; fixed address\n", var->asm_name, var->addr);
    }
  }

  for (int i = 0; i < norder; i++) {
    FnInfo *info = order[i];
    int size = info->nlocals + info->ntemps;
    if (size == 0)
      continue;
    fprintf(o, "\n; ---- frame: %s (%d..%d) ----\n", info->fn->name, info->base,
            info->base + size - 1);

    int n;
    Obj **vars = frame_vars(info->fn, &n);
    for (int j = 0; j < n; j++) {
      bool is_param = false;
      for (Obj *p = info->fn->params; p; p = p->next)
        is_param = is_param || p == vars[j];
      fprintf(o, "define %s %d%s\n", vars[j]->asm_name, vars[j]->addr,
              is_param ? " ; parameter" : "");
    }
    for (int j = 0; j < info->ntemps; j++)
      fprintf(o, "define t_%s__%d %d\n", info->fn->name, j,
              info->base + info->nlocals + j);
  }

  if (npool) {
    fprintf(o, "\n; ---- constant pool ----\n");
    for (int i = 0; i < npool; i++)
      fprintf(o, "define %s %d\n", pool[i].name, pool[i].addr);
  }

  // Entry stub. The VM runs the program from instruction 0 every frame.
  // On the first frame, the stub initializes globals and calls start().
  fprintf(o, "\n; ---- entry stub: runs every frame from instruction 0 ----\n");
  nlines = 0;
  unreachable = false;
  if (has_once) {
    emit("jnea __g2_entry_first %s 1", started_flag);
    emit("ja __g2_entry_tick");
    emit_label("__g2_entry_first");
    emit("ldi %s 1", started_flag);
    for (int i = 0; i < npool; i++)
      emit("ldi %s %s", pool[i].name, pool[i].value);
    emit_global_init(globals, nglobals);
    if (start)
      emit("call %s", start->asm_name);
    emit_label("__g2_entry_tick");
  }
  if (tick)
    emit("call %s", tick->asm_name);
  emit("ja __g2_end");
  print_lines(o);

  for (int i = 0; i < norder; i++) {
    fprintf(o, "\n");
    fwrite(order[i]->code, order[i]->code_len, 1, o);
  }

  // Jumping past the last instruction ends the frame.
  fprintf(o, "\n__g2_end:\n");
}
