/*
 * cmp2 - two's complement of an unsigned long long, in one line of EC.
 *
 *   ASM(name, type, instructions)
 *
 * becomes a naked function `type name(type)`: one argument, one result, no
 * prologue and no epilogue.  EC knows the x86-64 convention, so ARG1 is the
 * argument and RESULT the return register - no %rdi, no %rax.
 */
#include <stdio.h>

typedef unsigned long long ULL;

ASM(cmp2, ULL, neg ARG1; mov ARG1, RESULT; ret)

int main(void)
{
    printf("cmp2(21) = %lld\n", (long long)cmp2(21));
    printf("cmp2(1)  = %lld\n", (long long)cmp2(1));
    return 0;
}
