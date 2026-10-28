/* bit access: every form of '@', and the contexts it has to survive in */
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

struct regs {
    unsigned char cr;
    unsigned char sr;
};

static unsigned get(void) { return 6; }

/* '@' works in macros too - directives are translated like any other line */
#define TOGGLE(v, n) ((v)@(n) ^= 1)
#define SET(v, n)    ((v)@(n) = 1)

int main(void)
{
    ULL x = 0;
    int i;

    /* read */
    CHECK(x@0 == 0);
    CHECK(x@63 == 0);

    /* write */
    x@3 = 1;
    CHECK(x@3 == 1);
    CHECK(x == 8);
    x@3 = 0;
    CHECK(x == 0);
    CHECK(x@3 == 0);

    /* set / clear / flip / toggle */
    x@5 |= 1;   CHECK(x@5 == 1);
    x@5 &= 0;   CHECK(x@5 == 0);
    x@5 ^= 1;   CHECK(x@5 == 1);
    x@5++;      CHECK(x@5 == 0);
    ++x@5;      CHECK(x@5 == 1);
    TOGGLE(x, 5); CHECK(x@5 == 0);

    /* index by a variable, and by a parenthesised expression */
    for (i = 0; i < 64; i++) {
        SET(x, i);
        CHECK(x@i == 1);
        CHECK(x@(i) == 1);
        x@i = 0;
        CHECK(x@i == 0);
    }
    x@(1 + 2) = 1;
    CHECK(x == 8);
    x = 0;

    /* structs, pointers, arrays, calls */
    struct regs r;
    memset(&r, 0, sizeof r);
    r.cr@7 = 1;
    CHECK(r.cr@7 == 1);
    CHECK(r.cr == 128);
    struct regs *p = &r;
    CHECK(p->cr@7 == 1);
    p->cr@7 = 0;
    CHECK(r.cr == 0);

    unsigned char frame[4];
    memset(frame, 0, sizeof frame);
    frame[2]@3 = 1;
    CHECK(frame[2] == 8);
    CHECK(frame[2]@3 == 1);
    CHECK(frame[2]@2 == 0);

    CHECK(get()@0 == 0);
    CHECK(get()@1 == 1);
    CHECK(get()@2 == 1);

    /* of an expression, and nested */
    unsigned v = 0xA5;
    CHECK((v | 0x100)@8 == 1);
    CHECK((v >> 4)@0 == 0);
    v@1 = 1;
    CHECK((v@1)@0 == 1);            /* bit 0 of the bit we just read */

    /* as a value, in conditions, without braces round the statement */
    if (x@0)
        CHECK(0);
    for (i = 0; i < 4; i++)
        x@i = 1;
    CHECK(x == 15);
    for (i = 0; i < 4; i++)
        x@i = 0;

    /* an assignment is an expression */
    CHECK((x@2 = 1) != 0);
    CHECK(x == 4);
    x = 0;

    /* '@' inside strings and comments is left alone */
    CHECK(strcmp("a@b", "a@b") == 0);
    /* x@7 in a comment, and [ and ASM(f, T, nop) too */

    if (!fails) printf("bits: all checks passed\n");
    return fails ? 1 : 0;
}
