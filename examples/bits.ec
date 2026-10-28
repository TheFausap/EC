/*
 * bits - single bit access with the '@' operator.
 *
 *   x@N        read bit N                -> (((x) >> (N)) & 1)
 *   x@N = v    write v (truth value)     -> (x) = ((x) & ~(1ULL << N)) | (!!v << N)
 *   x@N |= v   set the bit if v is true  -> (x) |= (!!v << N)
 *   x@N &= v   clear it if v is false    -> (x) &= ~(!v << N)
 *   x@N ^= v   flip it if v is true      -> (x) ^= (!!v << N)
 *   x@N++      flip it                   -> (x) ^= (1ULL << N)
 *
 * N is one term: a number, a name, or a parenthesised expression (x@(i + 1)).
 */
#include <stdio.h>
#include <string.h>

typedef unsigned long long ULL;

struct uart {
    unsigned char cr;      /* control register */
    unsigned char sr;      /* status register  */
};

int main(void)
{
    ULL x = 0;
    int i;

    for (i = 0; i < 64; i += 7) {
        x@i = 1;
        printf("bit %2d set   -> x = %llu\n", i, x);
        x@i = 0;
    }

    x = 0;
    x@0 = 1;
    x@2 = 1;
    printf("x = %llu, popcount-ish x@0=%llu x@1=%llu x@2=%llu\n",
           x, x@0, x@1, x@2);

    /* toggle a bit in a loop */
    for (i = 0; i < 3; i++)
        x@0++;
    printf("after three toggles x@0 = %llu\n", x@0);

    /* bits of a struct member, through a pointer, and of an array element */
    struct uart u;
    memset(&u, 0, sizeof u);
    u.cr@7 = 1;                     /* enable */
    u.cr@1 = 1;                     /* interrupt on tx empty */
    printf("u.cr = 0x%02x (bit7=%d bit1=%d)\n", u.cr, u.cr@7, u.cr@1);

    struct uart *p = &u;
    p->cr@1 = 0;
    printf("u.cr = 0x%02x\n", u.cr);

    unsigned char frame[4] = { 0, 0, 0, 0 };
    frame[2]@3 = 1;
    printf("frame[2] = 0x%02x\n", frame[2]);

    /* the index can be any single term, including a variable */
    for (i = 0; i < 8; i++)
        if (u.cr@i)
            printf("u.cr bit %d is set\n", i);

    return 0;
}
