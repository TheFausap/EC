# EC Language Reference

*EC 0.2.0 — C with single-bit access and inline assembler, BBC BASIC style.
`README.md` is the guided tour; this is the reference.*

EC is a **source-to-source translator**, not a compiler.  `ec` reads a `.ec`
file and writes an ordinary `.c` file that uses nothing but standard GNU C
constructs — `__attribute__((naked))` and `__asm__` — which you then compile
with gcc or clang as usual.  Nothing in the generated code depends on EC.

```
prog.ec  ──ec──►  prog.c  ──cc──►  prog
```

Everything in your file that is not EC syntax is copied through byte for byte,
and `#line` directives in the generated C keep compiler, assembler and
debugger messages pointing at your `.ec` source.

`examples/` has a runnable program per feature and `tests/` doubles as a set
of regression fixtures; this manual was written against EC 0.2.0 with gcc 12
on x86-64.

**Contents**

1. [Running EC](#1-running-ec)
2. [Lexical rules](#2-lexical-rules)
3. [Bits: `x@N`](#3-bits-xn)
4. [Assembler: the four forms](#4-assembler-the-four-forms)
5. [`ASM(name, type, insns)`](#5-asmname-type-insns)
6. [`ASM` declaration block](#6-asm-declaration-block)
7. [The bracket block](#7-the-bracket-block)
8. [The double-bracket block](#8-the-double-bracket-block)
9. [Labels](#9-labels)
10. [Registers, immediates and the ABI](#10-registers-immediates-and-the-abi)
11. [How a body line is rewritten](#11-how-a-body-line-is-rewritten)
12. [Diagnostics](#12-diagnostics)
13. [Implementation limits](#13-implementation-limits)
14. [Grammar summary](#14-grammar-summary)
15. [Differences from C](#15-differences-from-c)

---

## 1. Running EC

### 1.1 `ec`

```
ec [options] input.ec
```

| option | meaning |
|--------|---------|
| `-o FILE` | write the generated C to `FILE` (`-` means standard output).  Default: `input.ec` → `input.c`; any other name gets `.c` appended |
| `-E` | write to standard output |
| `-` (as the file) | read standard input; implies `-E` |
| `-v`, `--verbose` | report the translation and the number of warnings |
| `--no-lines` | do not emit `#line` directives |
| `--no-pct` | do not double `%` in extended asm (see [§7.3](#73-percent-doubling)) |
| `--no-regs` | do not rewrite register/argument names in asm bodies (see [§10.6](#106---no-regs)) |
| `--abi=NAME` | calling convention for `ARGn`…: `sysv` (default) or `win64` |
| `--intel` | write asm bodies in Intel syntax (see [§10.4](#104---intel)) |
| `--help`, `-h` | print the usage summary |
| `--version` | print the version |

Options may appear before or after the input file; one input file only.

### 1.2 Exit status

| code | meaning |
|------|---------|
| 0 | translated, no errors (warnings do not change this) |
| 1 | one or more errors; nothing is written |
| 2 | bad command line, unreadable input, unwritable output |

### 1.3 `ecc`

`ecc` translates and compiles in one step.  Compiler options are passed
straight through to `$CC` (`cc` by default) and EC options are forwarded to
`ec`:

```
./ecc -O2 -o prog prog.ec             # generated C is a temporary file
./ecc -d -g -O0 -o prog prog.ec       # -d keeps prog.c beside the source
./ecc --intel -O2 -o prog prog.ec     # --intel and friends go to ec
```

Recognised EC options: `-d`/`--keep-c`, `--intel`, `--no-regs`, `--no-lines`,
`--no-pct`, `--abi=…`.  Exactly one `.ec` argument is allowed.

### 1.4 With make

The `Makefile` has pattern rules for both steps, so

```
make prog          # prog.ec -> prog.c -> prog
```

works, and `make examples` builds and runs everything in `examples/`.  An
example (or test) may ask for EC flags itself by putting them in the first
line:

```c
// ec: --intel
```

which `make examples` and `tests/run.sh` both honour.

---

## 2. Lexical rules

EC scans your file as text; it does not parse C.  These rules decide what is
yours and what is EC's.

### 2.1 Never looked into

String literals, character constants and comments (`//`, `/* */`) are copied
through whole and are never searched for EC syntax, so `"a@b"`, `'@'` and
`/* x@3 */` are all safe.  A file containing no EC syntax comes out unchanged
byte for byte — there is a test for exactly that.

### 2.2 Preprocessor lines

A line whose first non-blank character is `#` is a **directive line**.
Inside one, line-based EC constructs (`ASM`, `[`, `[[`) are not recognised,
but `@` still is, so macro bodies work:

```c
#define SET(v, n)    ((v)@(n) = 1)
#define TOGGLE(v, n) ((v)@(n) ^= 1)
```

### 2.3 Constructs that must start a line

`ASM`, the `[` of an assembler block and the `[[` of a variable-binding block
are recognised only as the first token on a line (whitespace allowed before
them).  That single rule is what keeps `a[i]`, `x = v[0] * 2;`, designated
initialisers (`[0] = 1,`) and C23 attributes (`[[nodiscard]]`, where an
identifier follows the brackets) meaning what C says they mean.

`@` is an operator wherever it appears, outside strings and comments.

### 2.4 Inside an assembler body

The whole line belongs to EC.  Comments on a body line are rewritten too — in
`add x, RESULT   /* x arrives in rdi */`, that `x` in the comment is
substituted as well.  Harmless, but worth knowing.

Three comment conventions end the part of the line EC analyses: `//`, `/*`
and a `#` followed by whitespace (so `#3` stays an immediate while `# note`
is a comment, exactly as in gas).

`\NAME` leaves the following word alone: `\RAX` in a body comes out as `RAX`.

---

## 3. Bits: `x@N`

### 3.1 Syntax

```
bit-access := term '@' index [ assign-op value | '++' | '--' ]
            | ('++' | '--') term '@' index
```

`term` is any lvalue-ish C term: a variable, `s.flags`, `p->cr`, `frame[2]`,
`(v | 0x100)`, a function call.  `index` is one term — a number, a name, or a
parenthesised expression (`x@(i + 1)`); it is translated recursively, so it
may itself contain EC syntax.

### 3.2 Translation

| EC | becomes |
|----|---------|
| `x@N` | `(((x) >> (N)) & 1)` |
| `x@N = v` | `((x) = ((x) & ~(1ULL << (N))) \| ((unsigned long long)!!(v) << (N)))` |
| `x@N \|= v` | `((x) \|= ((unsigned long long)!!(v) << (N)))` |
| `x@N &= v` | `((x) &= ~((unsigned long long)!(v) << (N)))` |
| `x@N ^= v` | `((x) ^= ((unsigned long long)!!(v) << (N)))` |
| `x@N++`, `++x@N`, `x@N--`, `--x@N` | `((x) ^= (1ULL << (N)))` |

The shift is `1ULL`, so bit indices 0…63 are usable on 64-bit objects; the
value written is reduced to a truth value (`!!v`, or `!v` for `&=`).

### 3.3 Semantics

* A read is an expression of type `int`-ish (`0` or `1`) and may be nested:
  `(x@1)@0`.
* An assignment is an expression too, so `if ((x@3 = 1)) …` compiles.
* `|=`/`&=`/`^=` do what the bit wants rather than what C bitwise logic would:
  `x@5 &= 0` clears bit 5, `x@5 ^= 1` flips it, `x@5 |= 1` sets it.
* `++` and `--` on a bit **toggle** it (there is no carry into the next bit).
* Index binds tighter than surrounding arithmetic, exactly as C's `>>` does:
  `x@3 + 1` is `(x@3) + 1`.  Write `x@(3 + 1)` for the other reading.

### 3.4 Caveats

* A write evaluates the left side twice (`x = (x & ~bit) | …`), so keep it a
  plain lvalue and not something with side effects.
* `x@N` does not check that `N` is in range of `x`.
* `&=` uses `!v`, not `!!v`: `x@N &= v` clears the bit when `v` is false and
  leaves it alone when `v` is true.
* The optional `ec.h` header defines `ULL` and friends plus plain-C spellings
  of the same operations (`BIT`, `BITSET`, `BITCLEAR`, `BITFLIP`, `BITPUT`),
  for a file that sometimes has to build with a bare C compiler.

---

## 4. Assembler: the four forms

| form | what it makes | knows C variables | operands |
|------|---------------|-------------------|----------|
| `ASM(name, type, insn; insn)` | a naked function of one argument | — | — |
| `ASM` *declaration* `{ … }` | a naked function with real parameters | parameter names | — |
| `[ … ]` | an asm statement inside a C function | no (you write the operand list) | optional, yours |
| `[[ … ]]` | an asm statement inside a C function | **yes, by name** | generated for you |

All four take the same body syntax: one instruction per line, labels as in
[§9](#9-labels), register and argument names as in [§10](#10-registers-immediates-and-the-abi).

---

## 5. `ASM(name, type, insns)`

```
ASM(cmp2, ULL, neg ARG1; mov ARG1, RESULT; ret)
```

becomes

```c
__attribute__((naked)) ULL cmp2(ULL) {
__asm__ volatile(
    "neg %rdi\n\t"
    "mov %rdi, %rax\n\t"
    "ret\n\t"
);
}
```

* `name` — the function's name.
* `type` — used for both the result and the single argument.  `void` gives
  `void name(void)`.  Any other type gives `type name(type)`: **the argument
  is deliberately left unnamed**, because a naked function has no prologue, so
  the argument is still sitting in its register (`ARG1`) and the result must
  be left in `RESULT`.
* `insns` — instructions separated by `;` at bracket depth 0 (`;` inside
  parentheses, brackets, braces or literals does not split).  Empty pieces
  are dropped.

The construct ends at the end of the line; a trailing `;` is consumed.

---

## 6. `ASM` declaration block

```c
ASM ULL add3(ULL x)
{
    mov x, RESULT
    add #3, RESULT
    ret
}
```

becomes

```c
__attribute__((naked)) ULL add3(ULL x __attribute__((unused))) {
__asm__ volatile(
    "mov %rdi, %rax\n\t"
    "add $3, %rax\n\t"
    "ret\n\t"
);
}
```

### 6.1 Naked function rules

A naked function gets no prologue or epilogue: **all** arguments are in
registers, the return address is where the ABI left it, and you must not fall
off the end — end with `ret` (or `jmp`).  Local variables do not exist; use
registers, or the caller's stack above the return address.

Parameters are given `__attribute__((unused))` so that gcc does not complain
about names that live only in registers.

### 6.2 Parameters and their registers

EC reads the parameter list and hands each parameter the register the ABI
gives it, counting integer/pointer and floating-point parameters separately:

* integer/pointer → `ARG1`, `ARG2`, … (the first six: `RDI RSI RDX RCX R8 R9`)
* `float`, `double`, `_Float*`, `__m*` → `FARG1`, `FARG2`, … (`XMM0`…)
* `struct`, `union`, `long double`, and anything past the register count →
  **EC warns and leaves the name alone** rather than guessing:

  ```
  args.ec:12: warning: parameter 's' is not passed in a register; use its
  stack slot by hand
  ```

  Use the raw register (`ARG1`) or the stack slot yourself in that case.

Storage classes (`static`, `extern`, `inline`, in any order) are hoisted in
front of the declaration, and the closing `}` must start its line.

---

## 7. The bracket block

An asm statement inside an ordinary function.  The `[` must be the first token
on its line and the `]` the first token on the line that ends the block:

```c
static ULL triangle(ULL n)
{
    ULL acc = 0;
    [
      : [acc] "=r"(acc) : [n] "r"(n) : "cc", "rcx"
        mov %[n], RCX
        xor RAX, RAX
    .loop
        add RCX, RAX
        dec RCX
        jnz loop
        mov RAX, %[acc]
    ];
    return acc;
}
```

### 7.1 Basic or extended

With no operand list the block becomes **basic** asm (`__asm__ volatile("…")`);
with one it becomes **extended** asm and the operand list is emitted after the
strings.  Basic asm is the honest choice when the block touches nothing but
registers you have listed as clobbered; extended asm is how you talk to C
variables — but see [`[[ … ]]`](#8-the-double-bracket-block), which does that for you.

### 7.2 Where the operand list goes

Any of three places, whichever reads best:

1. on the `[` line, after the bracket;
2. as the first line of the body;
3. as the last line of the body, the way gcc writes extended asm.

An operand list is recognised by starting with `"` or `:`.  Text after `[` on
the opening line that is neither produces a warning and is ignored.

### 7.3 Percent doubling

Extended asm treats `%` specially, so a bare `%rax` in the body has to be
written `%%rax`.  EC does that for you: in an extended block every `%` that is
not already part of an operand reference (`%[name]`, `%{`, `%}`, `%|`, `%=`,
`%%`, `%l`, `%0`…) is doubled.  With basic asm (no operands) nothing is
doubled.  `--no-pct` switches the whole thing off.

---

## 8. The double-bracket block

Assembler between two ordinary C statements, reaching the variables around it
— the BBC BASIC thing:

```c
int a = 4

[[ mov a, ARG1
   add #3, ARG1 ]]
```

is `a += 3`, and comes out as

```c
    {
        unsigned long long _ec_t0 = (unsigned long long)(a);
        __asm__ volatile(
            "mov %[_ec_v0], %%rdi\n\t"
            "add $3, %%rdi\n\t"
            "mov %%rdi, %[_ec_v0]\n\t"
            : [_ec_v0] "+r"(_ec_t0) :  : "cc", "rdi"
        );
        a = (__typeof__(a))_ec_t0;
    }
```

### 8.1 Shape

`[[` must start a line; the body runs to the next `]]`, which may end the last
instruction line, sit on a line of its own, or — for a one-line block —
follow the instruction on the same line:

```c
[[ add #3, a ]]
[[ mov a, ARG1
   add #3, ARG1 ]]
[[
    mov a, SCRATCH1
    add #3, SCRATCH1
    mov SCRATCH1, a
]]
```

A `;` after the closing `]]` is consumed, and comments (`//`, `/* */`, `#`)
are allowed inside the body.

### 8.2 How a name is classified

For each line (analysis stops at `//`, `/*` or `#` followed by a space) EC
tokenises the line, keeps the depth of `(`/`[`, and reads the first token as
the mnemonic.  Every later token is then:

1. a **register or ABI name** — `RAX`, `ARG1`, `SCRATCH1`, `RESULT`, … as
   listed in [§10](#10-registers-immediates-and-the-abi);
2. a **label** of this block ([§9](#9-labels)), or a gas register written in
   lower case inside a memory operand — `8(%rip)`, `(%rsp)` — or after a `%`;
3. otherwise a **C variable** of the enclosing function.

Lines whose operands are code rather than data are skipped by step 3:
directives (`.byte`, `.align`, …), jumps (`j*`), `call`, `ret`, `leave`,
`nop`, `loop*`, `int`, `syscall`, `ud2`, `hlt`.

A name may be a member or a pointer dereference: `p.n`, `p->n`.

### 8.3 Binding

The first token of a line is the mnemonic and is never a variable.  Each
distinct variable becomes one operand — and the same name counts twice if it
is used both bare and in parentheses, once as a value and once as an address.
EC emits

```c
unsigned long long _ec_tN = (unsigned long long)(expr);   /* before */
expr = (__typeof__(expr))_ec_tN;                          /* after  */
```

and refers to it in the body as `%[_ec_vN]`, with the constraint `"+r"`.  The
temporary is always 64 bits wide whatever the variable is, so a bound name is
a **64-bit operand**: `[[ add #3, a ]]` on an `int` is a 64-bit add whose
result is stored back into an `int`.  A suffix or a 32-bit register name
cannot be mixed with one, because gcc substitutes a 64-bit register —
`addl $3, %[_ec_v0]` comes out as `addl $3, %rsi` and `mov a, EAX` as
`mov %rsi, %eax`, neither of which assembles.  When the width matters, write a
[`[ … ]`](#7-the-bracket-block) block with an operand list of your own: `: [a] "+r"(a)`
on an `int` gives you `addl $3, %[a]`.

If the name is declared `float` or `double` anywhere in the file, the
temporary is a `double`, the constraint is `"+x"` and the value travels in an
xmm register: `[[ addsd d, d ]]`.

The block is wrapped in `{ … }` when it has at least one value variable; a
block with only addresses (below) is a bare statement.

### 8.4 Borrows: a register standing in for a variable

`mov a, ARG1` loads a register *from* a variable: that register now stands in
for the variable, so EC appends the matching move at the end of the body
(`mov ARG1, a` in AT&T order, `mov a, ARG1` under `--intel`) — which is what
turns the opening example into `a += 3` rather than `a = a`.  Write the move
back yourself and EC does not add a second one.

It follows that the borrowed register's **final** value is what the variable
gets:

```c
int n = 3, acc = 0;
[[ mov n, RCX
.top
   add RCX, acc
   dec RCX
   jnz top
]]
```

leaves `acc == 6` and `n == 0`, because RCX counted itself down and was handed
back.

### 8.5 Addresses: `(name)`

A variable used inside parentheses is bound as its **address**:

```c
[[ movb #65, (buf) ]]        /* buf[0] = 'A' */
```

```c
:  : [_ec_a0] "r"(&buf) : "cc", "memory"
```

The block also gets a `"memory"` clobber, because the compiler cannot see a
write made through a pointer operand.

### 8.6 Clobbers

Every register named in the body is added to the clobber list (as `rdi`,
`rcx`, `xmm0`, …) except `rsp` and `rbp`, which may not be clobbered.  `"cc"`
is always there, and `"memory"` is added when the block uses an address.

---

## 9. Labels

Inside **any** asm body, a line that is only

```
.name          BBC BASIC style
name:          gas style
```

defines a label, and references to that name elsewhere in the same block are
rewritten to a **gas numeric local label** (`1:`, jumped to as `1b` / `1f`).
Numeric labels are not a stylistic choice: gcc is free to clone or inline the
function holding the block, and a named symbol would then be defined twice.

* `.text`, `.byte`, `.align` and friends are gas directives, not labels, and
  are left alone.
* Numeric labels you write yourself (`1:`, `2:`) are left alone, and their
  numbers are reserved so that EC's own labels never collide with them.
* Labels are scoped to the block, and their numbering restarts with each
  block.

---

## 10. Registers, immediates and the ABI

Inside an assembler body EC knows the machine, so gas punctuation can go.
Only **upper-case** words are recognised, and only inside a body, so no C
identifier is ever at risk.

### 10.1 Registers

| written | is |
|---------|----|
| `RAX` `RBX` `RCX` `RDX` `RSI` `RDI` `RBP` `RSP` `R8`…`R15` | 64-bit registers |
| `EAX` … `R15D` | 32-bit |
| `AX` … `R15W` | 16-bit |
| `AL` … `R15B`, `AH` `BH` `CH` `DH` | 8-bit |
| `XMM0`…`XMM15`, `YMM0`…`YMM15`, `RIP` | vector and rip |

### 10.2 Arguments, results, temporaries

| written | SysV AMD64 | Windows x64 |
|---------|-----------|-------------|
| `ARG1`…`ARG6` | `RDI RSI RDX RCX R8 R9` | `RCX RDX R8 R9` — only `ARG1`…`ARG4` |
| `FARG1`…`FARG8` | `XMM0`…`XMM7` | `XMM0`…`XMM3` — only `FARG1`…`FARG4` |
| `SCRATCH1`, `SCRATCH2` | `R10`, `R11` | `R10`, `R11` |
| `RESULT` | `RAX` | `RAX` |
| `FRESULT` | `XMM0` | `XMM0` |

`SCRATCH1`/`SCRATCH2` are the registers used by neither arguments nor
results — the ones a block can freely use.

### 10.3 Immediates and escapes

| written | becomes |
|---------|---------|
| `#255` | `$255` (`255` under `--intel`) |
| `\RAX` | `RAX` — one word left alone |
| `%rax` | `%rax`, doubled to `%%rax` in extended asm |

A `#` followed by whitespace starts a comment instead, as in gas.

### 10.4 `--intel`

Gas takes the source operand first, the least BASIC-like thing about it.  With
`--intel` EC brackets every body with `.intel_syntax noprefix` and
`.att_syntax`, and then the destination comes first, immediates are bare and
registers carry no `%`:

```c
ASM ULL gcd(ULL a, ULL b)
{
    mov RESULT, a
    mov SCRATCH1, b
.top
    test SCRATCH1, SCRATCH1
    jz done
    …
}
```

One caveat: gcc substitutes extended-asm operands in AT&T syntax whatever the
body is written in, so with `--intel` use register constraints (`"r"`), not
memory ones (`"m"`) — EC warns if it sees one.

### 10.5 `--abi=sysv` / `--abi=win64`

Chooses the table used for `ARGn`/`FARGn`.  SysV (Linux, macOS, BSD) is the
default; `win64` gives four integer argument registers.

### 10.6 `--no-regs`

Turns the register, argument and immediate rewriting of §10 off — bodies are
otherwise handled as before (labels, `%` doubling and `[[ ]]` variable binding
still happen), which is what you want if you would rather write gas by hand.

---

## 11. How a body line is rewritten

For each line of a body, in this order:

1. **Label line?**  A whole line that is `.name` or `name:` becomes
   `N:` — no other rewriting.
2. **Machine names** (`asm_subst`, skipped under `--no-regs`): `\NAME`
   escapes one word; `#imm` becomes `$imm`; upper-case register, `ARGn`,
   `FARGn`, `SCRATCHn`, `RESULT`, `FRESULT` and naked-function parameter names
   become registers (`%rdi`, or `rdi` under `--intel`).  Words are recognised
   whole, so the `d` in `addsd` is never read as a variable named `d`.
3. **To a C string**: `"` and `\` are escaped, `%` is doubled in extended asm,
   label definitions and references become numeric ones, and — in a `[[ ]]`
   block — bound variables become `%[_ec_vN]` / `%[_ec_aN]`.  `"\n\t"` is
   appended.
4. Under `--intel`, `.intel_syntax noprefix` and `.att_syntax` bracket the
   whole set of strings.
5. The operand list, if any, is emitted last (`: … : … : …` for `[ ]` blocks;
   the generated list for `[[ ]]` blocks).

---

## 12. Diagnostics

```
file.ec:12: error: message
file.ec:12: warning: message
```

Errors are counted; when there is at least one, `ec` prints
`ec: N error(s)` and exits 1 without writing output.  Warnings do not stop the
translation (only `-v` reports how many there were).

`#line` directives keep the *compiler's* errors pointing at the `.ec` file as
well, and are emitted only when the generated line numbering has drifted from
the source's.  `--no-lines` suppresses them.

### 12.1 Errors

| message | cause |
|---------|-------|
| `expected a bit index after '@' (try x@(expr))` | `x@` with no index |
| `a single bit does not support 'OP' (use '=', '\|=', '&=', '^=' or '++')` | e.g. `x@3 += 1` |
| `'@' with no expression on the left` | `@3` at the start of an expression |
| `expected '(' after ASM` | `ASM` not followed by `(` |
| `unterminated ASM(...) construct` | missing `)` |
| `ASM() needs three parts: ASM(name, type, instructions)` | fewer than two commas at depth 0 |
| `ASM() needs a name and a type` | empty name or type |
| `expected a '{' after the ASM function declaration` | `ASM` block without a body |
| `ASM function body is never closed by a line starting with '}'` | missing `}` |
| `'[' assembler block is never closed by a line starting with ']'` | missing `]` |
| `'[[' block is never closed by ']]'` | missing `]]` |

### 12.2 Warnings

| message | cause |
|---------|-------|
| `parameter 'x' is not passed in a register; use its stack slot by hand` | struct/union/`long double`, or an argument past the register count |
| `ignoring text after '[' (asm operands must start with '"' or ':')` | junk on the opening line of a `[` block |
| `intel syntax with a memory operand: gcc writes those in AT&T syntax, so 'm' constraints will not assemble` | `--intel` with an `"m"` constraint |

---

## 13. Implementation limits

| limit | value |
|-------|-------|
| labels per block | 128 |
| bound variables per `[[ ]]` block | 32 |
| borrows per block | 32 |
| registers collected for the clobber list | 64 |
| parameters of a naked function mapped to registers | 16 |
| tokens analysed per body line | 24 |
| a variable name / register name | 128 / 24 characters |

Exceeding one is not an error; the extra names are simply left as
written.

---

## 14. Grammar summary

```ebnf
program        = { item } ;

item           = bit-access
               | asm-oneliner
               | asm-block
               | bracket-block
               | inline-block
               | any-c-text ;

bit-access     = term "@" index [ assign-op value | "++" | "--" ] ;
assign-op      = "=" | "|=" | "&=" | "^=" ;

asm-oneliner   = "ASM" "(" name "," type ";"… ")" ;
asm-block      = "ASM" declaration "{" body "}" ;
bracket-block  = "[" [ operands ] body "]" ;
inline-block   = "[[" body "]]" ;

body           = { line } ;
line           = label-def | instruction ;
label-def      = "." name | name ":" ;
instruction    = mnemonic { operand } [ comment ] ;

operand        = register | immediate | label-ref | c-variable
               | "(" c-variable ")" | gas-text ;
register       = UPPER-CASE-WORD ;        (* RAX, ARG1, RESULT, … *)
immediate      = "#" number ;
```

`ASM`, the `[` of a bracket block and the `[[` of an inline block are
recognised only as the first token on a line.

---

## 15. Differences from C

Everything not listed here is C, unchanged.

* **`ASM` is a keyword** at the start of a line.  Rename any identifier called
  `ASM`.
* **A `[` that starts a line** opens an assembler block when nothing (or only
  an operand list starting with `"` or `:`) follows it.  `[0] = 1,` inside an
  initialiser, which does not start a line, is unaffected.
* **A `[[` that starts a line** opens a variable-binding block unless an
  identifier follows immediately — so `[[nodiscard]]` is left alone.
* **`@` is an operator** everywhere outside strings, character constants and
  comments.  C has no `@`, so nothing else can be affected.
* **Comments on an assembler body line are rewritten** along with the
  instruction.
* **The generated C depends on GNU C** (`__attribute__((naked))`,
  `__asm__`, `__typeof__`) and the *assembly* depends on x86-64.  The
  translator itself is plain C11 with no dependencies.
