// ec: --intel
/*
 * The same machinery with intel syntax: destination first, bare immediates,
 * no '%' on registers.  run.sh picks the flag up from the first line.
 */
#include <stdio.h>

typedef unsigned long long ULL;

ASM(cmp2, ULL, neg ARG1; mov RESULT, ARG1; ret)

ASM ULL add3(ULL x)
{
    mov RESULT, x
    add RESULT, 3
    ret
}

ASM ULL gcd(ULL a, ULL b)
{
    mov RESULT, a
    mov SCRATCH1, b
.top
    test SCRATCH1, SCRATCH1
    jz done
    mov SCRATCH2, RESULT
    xor RDX, RDX
    div SCRATCH1              /* RDX:RAX / SCRATCH1 -> quotient RAX, rest RDX */
    mov RESULT, SCRATCH1
    mov SCRATCH1, RDX
    jmp top
.done
    ret
}

static ULL triangle(ULL n)
{
    ULL acc = 0;
    [
      : [acc] "=r"(acc) : [n] "r"(n) : "cc", "rcx"
        mov RCX, %[n]
        xor RAX, RAX
    .loop
        add RAX, RCX
        dec RCX
        jnz loop
        mov %[acc], RAX
    ];
    return acc;
}

int main(void)
{
    int fails = 0;
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); fails++; } } while (0)

    CHECK(cmp2(21) == (ULL)-21);
    CHECK(add3(4) == 7);
    CHECK(gcd(12, 18) == 6);
    CHECK(gcd(1071, 462) == 21);
    CHECK(triangle(10) == 55);

    if (!fails) printf("intel: all checks passed\n");
    return fails ? 1 : 0;
}
