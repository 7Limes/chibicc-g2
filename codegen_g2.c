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
static FILE *out;         // current function's code buffer
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
static char *address_comment(char *line) {
  if (strchr(line, ';'))
    return NULL;

  char *buf = strdup(line);
  char *ops[4];
  int n = 0;
  char *mnemonic = strtok(buf, " ");
  for (char *t = strtok(NULL, " "); t && n < 4; t = strtok(NULL, " "))
    ops[n++] = t;

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

static void emit(char *fmt, ...) {
  // Code after an unconditional jump is never executed.
  if (unreachable)
    return;

  va_list ap;
  va_start(ap, fmt);
  char buf[1024];
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);

  char *comment = address_comment(buf);
  if (comment)
    fprintf(out, "    %s ; %s\n", buf, comment);
  else
    fprintf(out, "    %s\n", buf);

  if (!strncmp(fmt, "ret", 3) || !strncmp(fmt, "ja ", 3))
    unreachable = true;
}

static void emit_label(char *name) {
  fprintf(out, "%s:\n", name);
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

  fprintf(out, "    ; %.*s\n", (int)(end - start), start);
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

// Returns a slot that holds the value of `op`. The constants 0, 1 and -1
// live in reserved slots. Other constants are loaded into a temporary.
static Operand to_slot(Operand op) {
  if (!op.is_imm)
    return op;
  if (op.imm == 0)
    return slot(SLOT_ZERO);
  if (op.imm == 1)
    return slot(SLOT_ONE);
  if (op.imm == -1)
    return slot(SLOT_MINUS_ONE);

  Operand t = new_temp();
  emit("ldi %s %s", t.name, imm_text(op));
  return t;
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
    if ((!is_integer(node->ty) && node->ty->kind != TY_PTR) || node->ty->kind == TY_BOOL)
      return false;
    return const_int(node->lhs, val);
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
    emit("addi %s %s %d", t.name, lv->ptr.name, lv->offset);
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

static Operand gen_assign(Node *node, Operand *hint) {
  if (node->ty->kind == TY_STRUCT || node->ty->kind == TY_UNION)
    unsupported(node, "struct and union assignments");

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

// Clears a local array or struct before its initializer runs.
static void gen_memzero(Obj *var) {
  int size = var->ty->size;
  if (size <= 32) {
    for (int i = 0; i < size; i++) {
      LValue lv = {.direct = true, .addr = var->addr + i, .var = var};
      emit("ldi %s 0", direct_name(&lv));
    }
    return;
  }

  char *loop = new_label("zero");
  Operand p = new_temp();
  Operand end = new_temp();
  emit("ldi %s %s", p.name, var->asm_name);
  emit("ldi %s %d", end.name, var->addr + size);
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
  case ND_MUL: return wrap32((uint64_t)a * (uint64_t)b);
  case ND_DIV: return floor_div(node, a, b);
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

static Operand gen_arith(Node *node, Operand *hint) {
  Operand a = gen_expr(node->lhs, NULL);
  Operand b = gen_expr(node->rhs, NULL);

  if (a.is_imm && b.is_imm)
    return imm(fold(node, a.imm, b.imm));

  NodeKind kind = node->kind;

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
      else if (k < 0 && k != INT32_MIN)
        emit("subi %s %s %d", d.name, a.name, -k);
      else
        emit("addi %s %s %d", d.name, a.name, k);
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
        emit("muli %s %s %d", d.name, a.name, k);
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
        emit("divi %s %s %d", d.name, a.name, k);
      return d;
    case ND_MOD:
      if (k == 0)
        error_tok(node->rhs->tok, "division by zero");
      free_op(a);
      if (k == 1 || k == -1)
        return imm(0);
      d = dest(hint);
      emit("modi %s %s %d", d.name, a.name, k);
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
    emit("addi %s %s 1", d.name, d.name);
    emit("divi %s %s 2", d.name, d.name);
    break;
  case ND_LE: // (cmp + 2) / 2 maps 1, 0, -1 to 1, 1, 0
    emit("addi %s %s 2", d.name, d.name);
    emit("divi %s %s 2", d.name, d.name);
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
    // All other scalar types are 32-bit integers, so the cast is a no-op.
    return gen_expr(node->lhs, hint);
  case ND_COMMA:
    if (is_compound_assign(node->lhs))
      return gen_compound_assign(node, hint);
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
    gen_memzero(node->var);
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
  case ND_SHR: {
    // Constant expressions are folded. Everything else needs runtime
    // helpers, which come later.
    Operand a = gen_expr(node->lhs, NULL);
    Operand b = gen_expr(node->rhs, NULL);
    if (a.is_imm && b.is_imm)
      return imm(fold(node, a.imm, b.imm));
    unsupported(node, "bitwise and shift operators");
  }
  }

  error_tok(node->tok, "invalid expression");
}

//
// Conditions
//

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

    if (node->kind == ND_LT) {
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
    emit_source_line(node->tok);
    char *loop = new_label("loop");
    char *brk = chibicc_label(node->brk_label);
    if (node->init)
      gen_stmt(node->init);
    emit_label(loop);
    if (node->cond)
      gen_cond(node->cond, false, brk);
    gen_stmt(node->then);
    emit_label(chibicc_label(node->cont_label));
    if (node->inc)
      gen_void(node->inc);
    emit("ja %s", loop);
    emit_label(brk);
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

static void gen_function(FnInfo *info, int base) {
  Obj *fn = info->fn;
  current = info;
  info->base = base;

  int n;
  Obj **vars = frame_vars(fn, &n);
  int offset = 0;
  for (int i = 0; i < n; i++) {
    vars[i]->addr = base + offset;
    offset += vars[i]->ty->size;
  }
  info->nlocals = offset;

  if (temp_cap)
    memset(temp_used, 0, temp_cap * sizeof(bool));
  last_comment_file = NULL;

  out = open_memstream(&info->code, &info->code_len);
  emit_label(fn->asm_name);
  gen_stmt(fn->body);
  emit("ret");
  fclose(out);
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
    else if (!strncmp(var->name, ".L..", 4))
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
  for (int i = 0; i < norder; i++) {
    FnInfo *root = order[i];
    if (root->fn != start && root->fn != tick)
      continue;
    if (root->depth + 1 > CALL_STACK_SIZE)
      error("call depth %d exceeds the g2 call stack (%d): %s", root->depth + 1,
            CALL_STACK_SIZE, call_chain(root));
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

  // Entry stub. The VM runs the program from instruction 0 every frame.
  // On the first frame, the stub initializes globals and calls start().
  fprintf(o, "\n; ---- entry stub: runs every frame from instruction 0 ----\n");
  char *buf;
  size_t buflen;
  out = open_memstream(&buf, &buflen);
  unreachable = false;
  if (has_once) {
    emit("jnea __g2_entry_first %s 1", started_flag);
    emit("ja __g2_entry_tick");
    emit_label("__g2_entry_first");
    emit("ldi %s 1", started_flag);
    emit_global_init(globals, nglobals);
    if (start)
      emit("call %s", start->asm_name);
    emit_label("__g2_entry_tick");
  }
  if (tick)
    emit("call %s", tick->asm_name);
  emit("ja __g2_end");
  fclose(out);
  fwrite(buf, buflen, 1, o);

  for (int i = 0; i < norder; i++) {
    fprintf(o, "\n");
    fwrite(order[i]->code, order[i]->code_len, 1, o);
  }

  // Jumping past the last instruction ends the frame.
  fprintf(o, "\n__g2_end:\n");
}
