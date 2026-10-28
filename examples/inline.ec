/*
 * inline.ec - assembler in the middle of C, the BBC BASIC way.
 *
 *   int a = 4
 *
 *   [[ mov a, ARG1
 *      add #3, ARG1 ]]
 *
 * is a += 3.  Everything in the block that is not a register, a label or a
 * mnemonic is taken to be a C variable of the function around it: loaded
 * before the block, handed back afterwards, and a register that stands in
 * for a variable ('mov a, ARG1') is written back at the end.
 */
#include <stdio.h>

struct pair { int n; int m; };

int main(void)
{
    int a = 4;

    [[ mov a, ARG1
       add #3, ARG1 ]]
    printf("a is %d\n", a);

    /* the short form of the same thing */
    [[ add #3, a ]]
    printf("a is now %d\n", a);

    /* a scratch register of our own, written back by hand */
    [[ mov a, SCRATCH1
       dec SCRATCH1
       mov SCRATCH1, a ]]
    printf("a is now %d\n", a);

    /* two variables at once: gcc gives each one a register */
    int b = 0;
    [[ mov a, b ]]
    printf("b is %d\n", b);

    /* members are variables too, and (name) is the address of name */
    struct pair p;
    p.n = 10; p.m = 0;
    [[ mov p.n, p.m ]]
    char buf[8] = { 0 };
    [[ movb #65, (buf) ]]
    printf("p.m is %d and buf holds \"%s\"\n", p.m, buf);

    /* a loop over a C variable, with a BBC-style label */
    int n = 3, acc = 0;
    [[ mov n, RCX
    .top
       add RCX, acc
       dec RCX
       jnz top
    ]]
    printf("1+2+3 is %d, and n is %d: RCX counted down and came back\n",
           acc, n);

    return 0;
}
