/*
 * args - the x86-64 calling convention the way you would say it out loud.
 *
 *   ARG1..ARG6    integer arguments   (RDI RSI RDX RCX R8 R9)
 *   FARG1..FARG8  float arguments     (XMM0..XMM7)
 *   SCRATCH1/2    R10, R11: used by neither arguments nor results
 *   RESULT        RAX, the return value
 *   #255          an immediate, BBC BASIC style
 *   RAX RBX ...   any register, without the '%'
 *
 * Gas takes the source operand first, so it is "mov n, SCRATCH1".  If you
 * would rather write "mov SCRATCH1, n", see examples/gcd.ec and --intel.
 *
 * and in a naked function the declared parameter names *are* their registers.
 */
#include <stdio.h>

typedef unsigned long long ULL;

/* one-liner: ARG1 in, RESULT out */
ASM(cmp2, ULL, neg ARG1; mov ARG1, RESULT; ret)

/* block: x is the first argument, so x is %rdi */
ASM ULL add3(ULL x)
{
    mov x, RESULT
    add #3, RESULT
    ret
}

/* two arguments, a label and a loop */
ASM ULL sum_to(ULL n)
{
    xor RESULT, RESULT
    test n, n               /* 0 + 1 + ... + n */
    jz done
    mov n, SCRATCH1
.again
    add SCRATCH1, RESULT
    dec SCRATCH1
    jnz again
.done
    ret
}

/* every argument register by number */
ASM ULL six(ULL a, ULL b, ULL c, ULL d, ULL e, ULL f)
{
    mov ARG1, RESULT
    add ARG2, RESULT
    add ARG3, RESULT
    add ARG4, RESULT
    add ARG5, RESULT
    add ARG6, RESULT
    ret
}

int main(void)
{
    printf("cmp2(21)   = %lld\n", (long long)cmp2(21));
    printf("add3(4)    = %llu\n", add3(4));
    printf("sum_to(10) = %llu\n", sum_to(10));
    printf("six(...)   = %llu\n", six(1, 2, 3, 4, 5, 6));
    return 0;
}
