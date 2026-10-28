# EC — Enhanced C

EC is C with two extensions, implemented as a small source-to-source
preprocessor (`ec`, one C file, no dependencies).  Run your `.ec` file through
it and compile the generated `.c` with gcc or clang as usual.

1. **Bit access** — `x@N` is bit `N` of `x`, for reading *and* writing.
2. **Inline assembler in the middle of C**, the way BBC BASIC did it on the
   Acorn machines: a block of mnemonics where you want them, plus a one-line
   form for whole functions.

```c
typedef unsigned long long ULL;

ASM(cmp2, ULL, neg %rdi; mov %rdi, %rax; ret)      /* two's complement */

int main(void)
{
    printf("cmp2(21) = %lld\n", (long long)cmp2(21));
    return 0;
}
```

```
$ make
$ ./ecc -O2 -o cmp2 examples/cmp2.ec && ./cmp2
cmp2(21) = -21
```

## Building

```
make                # builds ./ec
make test           # runs the test suite
make examples       # builds and runs everything in examples/
```

`ec` is written in C11 and needs nothing but a C compiler:

```
cc -O2 -o ec ec.c
```

## Using it

```
ec [options] input.ec

  -o FILE    write the generated C to FILE (default: input.ec -> input.c)
  -E         write to standard output (input may be "-")
  -v         say what it is doing
  --no-lines don't emit #line directives
  --no-pct   don't escape '%' in extended asm (see below)
```

`ecc` is a thin wrapper that translates and compiles in one step:

```
./ecc -O2 -o prog prog.ec
```

Generated code carries `#line` directives, so compiler and assembler messages
point at your `.ec` file and the right line.

---

## 1. Bits: `x@N`

`N` is a single term: a number, a name, or a parenthesised expression
(`x@(i + 1)`).

| EC             | means                | generated C                                                    |
|----------------|----------------------|----------------------------------------------------------------|
| `x@N`          | read bit N           | `(((x) >> (N)) & 1)`                                           |
| `x@N = v`      | write v (truth value)| `((x) = ((x) & ~(1ULL << (N))) \| ((unsigned long long)!!(v) << (N)))` |
| `x@N \|= v`    | set if v is true     | `((x) \|= ((unsigned long long)!!(v) << (N)))`                 |
| `x@N &= v`     | clear if v is false  | `((x) &= ~((unsigned long long)!(v) << (N)))`                  |
| `x@N ^= v`     | flip if v is true    | `((x) ^= ((unsigned long long)!!(v) << (N)))`                  |
| `x@N++`, `++x@N` | flip               | `((x) ^= (1ULL << (N)))`                                       |

The left side can be any lvalue term: a variable, `s.flags@7`, `p->cr@1`,
`frame[2]@3`, `(v \| 0x100)@8`, or the result of a call.  Assignments are
expressions, so `if ((x@3 = 1)) ...` works, and bit reads nest: `(x@1)@0`.

```c
flags@3 = 1;              /* set   */
flags@3 = 0;              /* clear */
flags@3 ^= 1;             /* flip  */
flags@3++;                /* flip  */
if (status@7) ...         /* test  */
for (i = 0; i < 8; i++)
    if (port@i) ...       /* variable index */
```

Two things to know:

* The index binds tighter than the surrounding arithmetic, exactly as C's
  `>>` does — `x@3 + 1` is `(x@3) + 1`, not `x@(3 + 1)`.  Parenthesise when
  you mean the latter.
* A write evaluates the left side twice (`x = (x & ~bit) \| ...`), so keep it
  a plain lvalue rather than something with side effects.

## 2. Assembler

### `ASM(name, type, instructions)` — one-line naked functions

```c
ASM(cmp2, ULL, neg %rdi; mov %rdi, %rax; ret)
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

`type` is used for both the return value and the single argument, and the
argument is deliberately left unnamed: a naked function has no prologue, so
the argument is still in `%rdi` (SysV AMD64) and the result must be in `%rax`.
Instructions are split on `;`.  Use `void` for no argument and no result:

```c
ASM(hello, void, mov $1, %rax; syscall; ret)
```

### `ASM <declaration> { ... }` — the same as a block

```c
ASM ULL strlen_asm(const char *s)
{
    mov %rdi, %rsi
    xor %rax, %rax
.again
    cmpb $0, (%rsi,%rax)
    je done
    inc %rax
    jmp again
.done
    ret
}
```

### `[ ... ]` — a block inside an ordinary function

The `[` must be the first token on its line, and `]` the first token on the
line that ends the block (that is what keeps `a[i]` unambiguous):

```c
static ULL triangle(ULL n)
{
    ULL acc = 0;
    [
      : [acc] "=r"(acc) : [n] "r"(n) : "cc", "rcx"
        mov %[n], %rcx
        xor %rax, %rax
    .loop
        add %rcx, %rax
        dec %rcx
        jnz loop
        mov %rax, %[acc]
    ];
    return acc;
}
```

With no operand list it is *basic* asm; with one it is *extended* asm, and the
operands may sit on the line after `[` or on the last line of the block,
wherever you find it more readable.  Because extended asm treats `%` as
special, EC doubles a `%` that is not already part of an operand reference, so
you can write ordinary gas syntax (`%rax`, `%[acc]`, `%0`) and not think about
it.  `--no-pct` turns that off.

Do remember the usual extended-asm rules: name the registers you clobber
(`"cc", "rcx"` above) and do not write to an input operand.

### Labels

A line that is just `.name` (BBC BASIC style) or a leading `name:` (gas style)
defines a label, and references to it elsewhere in the same block are
rewritten.  Labels become gas **numeric** local labels (`1:`, jumped to as
`1b` / `1f`), so a block stays correct even when the compiler clones or inlines
the function around it.  Labels you write as numbers yourself are left alone.

---

## What it does *not* change

Everything else is copied through byte for byte; strings, character literals
and comments are never looked into, so `"a@b"`, `'@'` and
`/* x@3 */` are safe.  A file with no EC syntax at all comes out unchanged
(there is a test for that), and `#define` bodies are translated too, so

```c
#define SET(v, n) ((v)@(n) = 1)
```

works.

Two consequences worth knowing:

* `ASM` is a keyword at the start of a line.  If you have an identifier called
  `ASM`, rename it.
* A `[` that starts a line *and* has nothing after it (or just an operand
  list) opens an assembler block.  Array initialisers written as
  `[0] = 1,` on one line are unaffected.

## Files

```
ec.c            the preprocessor
ec.h            optional header: ULL and friends, plus plain-C BIT() macros
ecc             translate-and-compile wrapper
examples/       cmp2.ec (one-liner), bits.ec (@), hello.ec (syscalls, labels)
tests/          run.sh and the .ec test programs
```

## Status and ideas

Working today: both extensions, labels, `#line` mapping, diagnostics that
point at the `.ec` source, plain-C pass-through.  Tested with gcc 12 on
x86-64; the generated code is ordinary C with `__attribute__((naked))` and
`__asm__`, so it is not tied to one compiler, but the *assembly* obviously is.

Possible next steps:

* multi-bit fields (`x@[7:4]`) and bit ranges as lvalues
* named bits (`status.tx_empty`) via a `bitfield` declaration
* `EQU`/`DB`-style data directives inside an asm block
* a real mini-assembler backend, so EC could emit bytes without gas
* a syntax-file / tree-sitter grammar for editors
