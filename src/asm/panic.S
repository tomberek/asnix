    .equ ASNIX_SYS_WRITE,      1
    .equ ASNIX_SYS_EXIT_GROUP, 231
    .equ ASNIX_STDERR_FD,      2

    .text
    .globl asnix_fatal
    .type asnix_fatal, @function
asnix_fatal:
    .cfi_startproc
    movq %rsi, %rdx
    movq %rdi, %rsi
    movq $ASNIX_STDERR_FD, %rdi
    movq $ASNIX_SYS_WRITE, %rax
    syscall
    movq $1, %rdi
    movq $ASNIX_SYS_EXIT_GROUP, %rax
    syscall
    .cfi_endproc
    .size asnix_fatal, . - asnix_fatal

