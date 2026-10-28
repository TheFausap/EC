/*
 * Registers and the x86-64 calling convention, without gas punctuation.
 * (SysV AMD64: arguments in RDI RSI RDX RCX R8 R9, floats in XMM0..7,
 *  result in RAX, and R10/R11 belong to nobody.)
 *
 * AT&T operand order still applies here - see t_intel.ec for the other way.
 */
#include <stdio.h>

typedef unsigned long long ULL;

/* ARG1 is the first integer argument, RESULT the return register */
ASM(cmp2, ULL, neg ARG1; mov ARG1, RESULT; ret)

/* a naked function's own parameter names are the registers they arrive in */
ASM ULL add3(ULL x)
{
    mov x, RESULT
    add #3, RESULT                  /* #3 is an immediate, BBC style */
    ret
}

ASM ULL sub_xy(ULL a, ULL b)
{
    mov a, RESULT
    sub b, RESULT
    ret
}

/* SCRATCH1 and SCRATCH2 (R10, R11) are used by neither arguments nor result */
ASM ULL sum4(ULL a, ULL b, ULL c, ULL d)
{
    mov a, SCRATCH1
    add b, SCRATCH1
    add c, SCRATCH1
    add d, SCRATCH1
    mov SCRATCH1, RESULT
    ret
}

/* floating point arguments take the SSE registers, and FARG1 is XMM0 */
ASM double twice(double x)
{
    movsd x, FRESULT
    addsd x, FRESULT
    ret
}

/* 32-bit sub-register names, mixed with plain gas syntax on the next line */
ASM unsigned low32(ULL v)
{
    mov v, RAX
    mov EAX, EAX                    /* writing the low half clears the rest */
    ret
}

/* the same inside a [ ] block: extended asm, with '%' doubling done for us */
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
        jnz loop                    # '#' with a space after it is still a comment
        mov RAX, %[acc]
    ];
    return acc;
}

int main(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

    CHECK(cmp2(21) == (ULL)-21);
    CHECK(cmp2(0) == 0);
    CHECK(add3(4) == 7);
    CHECK(sub_xy(10, 3) == 7);
    CHECK(sum4(1, 2, 3, 4) == 10);
    CHECK(twice(1.5) == 3.0);
    CHECK(low32(0x123456789ABCDEF0ULL) == 0x9ABCDEF0u);
    CHECK(triangle(10) == 55);
    CHECK(triangle(100) == 5050);

    if (!fails) printf("regs: all checks passed\n");
    return fails ? 1 : 0;
}
