/*
 * hello - a naked function that talks to the kernel directly.
 *
 * write(1, msg, 13) and exit(0), straight from assembler.  `msg` is an
 * ordinary C object at file scope, so the assembler can name it; EC touches
 * nothing else about the function.
 */
#include <stddef.h>

typedef unsigned long long ULL;

static const char msg[] = "Hello, EC!\n";

ASM(hello, void,
    mov #1, RAX                  /* SYS_write */
    mov #1, ARG1                 /* fd 1      */
    lea msg(%rip), ARG2          /* buf       */
    mov #11, ARG3                /* count     */
    syscall
    mov #60, RAX                 /* SYS_exit  */
    xor ARG1, ARG1
    syscall)

/*
 * The same thing written as a block, with a BBC-BASIC style label.  Labels
 * starting with '.' (or ending with ':') are given a private name, so the
 * block can be used more than once in a program.
 */
ASM ULL strlen_asm(const char *s)
{
    mov s, RSI
    xor RAX, RAX
.again
    cmpb $0, (RSI,RAX,1)
    je done
    inc RAX
    jmp again
.done
    ret
}

int main(void)
{
    hello();                                  /* never returns */
    return (int)strlen_asm(msg);
}
