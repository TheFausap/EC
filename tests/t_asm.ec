/* inline assembler: one-liners, naked blocks, [ ] blocks, labels */
#include <stdio.h>

typedef unsigned long long ULL;

/* ASM(name, type, insn; insn; ...) -> __attribute__((naked)) type name(type) */
ASM(cmp2, ULL, neg %rdi; mov %rdi, %rax; ret)

ASM(swap_nibble, ULL, mov %rdi, %rax; rol $4, %rax; ret)

/* the same idea written out as a block */
ASM ULL add3(ULL x)
{
    mov %rdi, %rax
    add $3, %rax
    ret
}

/* a BBC-BASIC style label; operands on the line after the '[' */
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

/* operands may also go last, the way gcc writes them */
static ULL downcount(ULL n)
{
    ULL acc = 0;
    [
        mov %[n], %rcx
        xor %rax, %rax
    .loop
        add %rcx, %rax
        dec %rcx
        jnz loop
        mov %rax, %[acc]
        : [acc] "=r"(acc) : [n] "r"(n) : "cc", "rcx"
    ];
    return acc;
}

/* basic asm, no operands, and the same label name again (it stays private) */
static void nops(void)
{
    [
        nop
        nop
    ];
}

/* gas style labels work too */
static ULL count_bits(ULL v)
{
    ULL c = 0;
    [
      : [c] "=r"(c) : [v] "r"(v) : "cc", "rdx"
        xor %rax, %rax
        mov %[v], %rdx
        test %rdx, %rdx
        jz out
    again:
        shr $1, %rdx
        adc $0, %rax
        test %rdx, %rdx
        jnz again
    out:
        mov %rax, %[c]
    ];
    return c;
}

int main(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

    CHECK(cmp2(21) == (ULL)-21);
    CHECK(cmp2(0) == 0);
    CHECK(add3(4) == 7);
    CHECK(swap_nibble(0x123456789ABCDEF0ULL) == 0x23456789ABCDEF01ULL);
    CHECK(triangle(10) == 55);
    CHECK(triangle(100) == 5050);
    CHECK(downcount(10) == 55);
    CHECK(count_bits(0xFF) == 8);
    CHECK(count_bits(0) == 0);
    nops();

    /* mix the two extensions: a bit of an asm result */
    ULL v = cmp2(1);
    CHECK(v@0 == 1);
    CHECK(v@63 == 1);
    CHECK(v@31 == 1);

    if (!fails) printf("asm: all checks passed\n");
    return fails ? 1 : 0;
}
