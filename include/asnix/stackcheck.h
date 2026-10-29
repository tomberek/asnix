#ifndef ASNIX_STACKCHECK_H
#define ASNIX_STACKCHECK_H

#ifdef __ASSEMBLER__

.macro ASNIX_CHECK_VALSTACK scratch
    leaq 8(%r12), \scratch
    cmpq asnix_valstack_limit(%rip), \scratch
    ja asnix_valstack_overflow
.endm

.macro ASNIX_CHECK_RETSP scratch
    leaq ASNIX_RETFRAME_SIZE(%r14), \scratch
    cmpq asnix_retsp_limit(%rip), \scratch
    ja asnix_retsp_overflow
.endm

#endif

#endif

