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
    mov $1, %rax                 /* SYS_write */
    mov $1, %rdi                 /* fd 1      */
    lea msg(%rip), %rsi          /* buf       */
    mov $11, %rdx                /* count     */
    syscall
    mov $60, %rax                /* SYS_exit  */
    xor %rdi, %rdi
    syscall)

/*
 * The same thing written as a block, with a BBC-BASIC style label.  Labels
 * starting with '.' (or ending with ':') are given a private name, so the
 * block can be used more than once in a program.
 */
ASM ULL strlen_asm(const char *s)
{
    mov %rdi, %rsi
    xor %rax, %rax
.again
    cmpb $0, (%rsi,%rax)
    je done
    inc %rax
    jmp again
.done
    ret
}

int main(void)
{
    hello();                                  /* never returns */
    return (int)strlen_asm(msg);
}
