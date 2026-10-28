/*
 * [[ ... ]] - assembler in the middle of C, the way BBC Basic had it.
 *
 *   int a = 4
 *
 *   [[ mov a, ARG1
 *      add #3, ARG1 ]]
 *
 * is a += 3.  Every name in the block that is not a register, a label or a
 * mnemonic is taken to be a C variable of the enclosing function: its value
 * is loaded before the block and handed back afterwards, and a register that
 * stands in for a variable ('mov a, ARG1') is written back at the end.
 * A name in parentheses is the address of the object:  movb #65, (buf).
 *
 * Operand order is AT&T unless the file is translated with --intel, so
 * 'add #3, a' adds 3 to a.
 */
#include <stdio.h>
#include <string.h>

typedef unsigned long long ULL;

static int fails = 0;

#define CHECK(c)                                                          \
    do {                                                                  \
        if (!(c)) {                                                       \
            printf("FAIL line %d: %s\n", __LINE__, #c);                   \
            fails++;                                                      \
        }                                                                 \
    } while (0)

struct pair { int n; int m; };

int main(void)
{
    /* the short form: one instruction, one variable */
    int a = 4;
    [[ add #3, a ]]
    CHECK(a == 7);

    /* the same thing the long way round: a register borrows the variable */
    a = 4;
    [[ mov a, ARG1
       add #3, ARG1 ]]
    CHECK(a == 7);

    /* the block may open on a line of its own and close on one too */
    a = 4;
    [[
        mov a, SCRATCH1
        add #3, SCRATCH1
        mov SCRATCH1, a
    ]]
    CHECK(a == 7);

    /* two variables at once - gcc gives each one a register of its own */
    int b = 0;
    [[ mov a, b ]]
    CHECK(b == 7);

    /* members of a structure are variables as well */
    struct pair p;
    p.n = 10; p.m = 0;
    [[ mov p.n, p.m ]]
    CHECK(p.m == 10);

    char buf[8];
    memset(buf, 0, sizeof buf);
    [[ movb #65, (buf) ]]                   /* (name) is the address of name */
    CHECK(buf[0] == 'A');

    /* a block with no C variable in it at all is passed straight through */
    [[ nop ]]

    /* a loop over a C variable, with a BBC-style label */
    int n = 3, acc = 0;
    [[ mov n, RCX
    .top
       add RCX, acc
       dec RCX
       jnz top
    ]]
    CHECK(acc == 6);
    /* RCX counted itself down to nothing and was handed back to n */
    CHECK(n == 0);

    /* floating point: a declared double is handed over in an xmm register */
    double d = 2.5;
    [[ addsd d, d ]]
    CHECK(d == 5.0);

    /* C23 attributes are left alone - an identifier follows the brackets */
    if (!fails) printf("inline: all checks passed\n");
    return fails ? 1 : 0;
}
