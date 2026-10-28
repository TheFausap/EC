/*
 * Plain C: ec must copy this file through unchanged, byte for byte.
 * It is full of things that look like EC syntax but are not.
 */
#include <stdio.h>
#include <string.h>

#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define ASM       1                     /* directives are not scanned for ASM */
#define LONG_MACRO(x)                                                         \
    do {                                                                      \
        printf("%d\n", (x));                                                  \
    } while (0)

static const char *text = "a@b ASM(f, T, nop) [bracket] 100% \\ \"quoted\"\n";
static const char at_sign = '@';
static const char bracket = '[';

static int table[] = {
    [0] = 1,
    [1] = 2,
    [2] = 3,
};

static int matrix[2][2] = { { 1, 2 }, { 3, 4 } };

struct pair { int a; int b[4]; };

int main(void)
{
    struct pair p = { 1, { 1, 2, 3, 4 } };
    char buf[128];
    int i, total = 0;

    /* a comment with @ and [ and ASM(x, y, z) in it */
    // another one, with 100% and \n and "quotes"
    for (i = 0; i < 3; i++)
        total += table[i];
    for (i = 0; i < 3; i++) {
        total += p.b[i];
    }
    i = 0;
    while (i < 4) {
        switch (i) {
        case 0:  total += 0; break;
        case 1:  total += 1; break;
        default: total += 2; break;
        }
        i++;
    }
    if (total > 10 && total < 100)
        LONG_MACRO(total);

    snprintf(buf, sizeof buf, "%s %c %c %d%% %d", text, at_sign, bracket,
             MAX(1, 2), p.b[2]);
    printf("%s\n", buf);
    printf("%d %d %d\n", table[0], matrix[1][1], ASM);

    return strcmp(buf, text) == 0 ? 1 : 0;
}
