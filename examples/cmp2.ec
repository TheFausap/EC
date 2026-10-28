/*
 * cmp2 - two's complement of an unsigned long long, in one line of EC.
 *
 *   ASM(name, type, instructions)
 *
 * becomes a naked function `type name(type)`: one argument, one result, no
 * prologue and no epilogue, so the argument is still sitting in %rdi and the
 * result has to be left in %rax (SysV AMD64 ABI).  The instructions are split
 * on ';' and handed to the assembler verbatim.
 */
#include <stdio.h>

typedef unsigned long long ULL;

ASM(cmp2, ULL, neg %rdi; mov %rdi, %rax; ret)

int main(void)
{
    printf("cmp2(21) = %lld\n", (long long)cmp2(21));
    printf("cmp2(1)  = %lld\n", (long long)cmp2(1));
    return 0;
}
