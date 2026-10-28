// ec: --intel
/*
 * gcd - Euclid's algorithm, in intel syntax.
 *
 * With --intel EC brackets every assembler body with .intel_syntax noprefix
 * and .att_syntax, so the destination comes first, immediates are bare and
 * registers carry no '%' - much closer to how BBC BASIC wrote it down.
 *
 *   mov RESULT, a          ; dst, src
 *   add RESULT, 3
 *
 * Run it with:  ecc --intel -O2 -o gcd examples/gcd.ec
 */
#include <stdio.h>

typedef unsigned long long ULL;

ASM ULL gcd(ULL a, ULL b)
{
    mov RESULT, a               /* parameters are their registers */
    mov SCRATCH1, b
.top
    test SCRATCH1, SCRATCH1
    jz done
    mov RAX, RESULT
    xor RDX, RDX
    div SCRATCH1                /* RDX:RAX / SCRATCH1: RAX=quotient RDX=rest */
    mov RESULT, SCRATCH1
    mov SCRATCH1, RDX
    jmp top
.done
    ret
}

int main(void)
{
    printf("gcd(12, 18)   = %llu\n", gcd(12, 18));
    printf("gcd(1071,462) = %llu\n", gcd(1071, 462));
    return 0;
}
