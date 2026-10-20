#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/value.h"

// Lambda-patterns (`{a, b ? d, ...}: body`): eager, pre-destructuring
// validation of the single attrset call argument, matching real Nix's
// timing exactly (confirmed against `nix-instantiate`: a type mismatch or a
// missing required field aborts even when the lambda body never reads the
// bad field; `...` present suppresses only the "unexpected argument" check,
// not the type/required checks).
//
// Pool layout at pool_idx (see ib_check_formals() in irbuild.c):
//   pool[pool_idx + 0]                                   = total declared-field
//       count, or -1 if the pattern has `...` (skips phase 2 below)
//   pool[pool_idx + 1 .. pool_idx + required_count]       = required fields'
//       symbol ids, sorted ascending (required_count = IrOp.a)
//   pool[pool_idx + 1 + required_count .. +total_count]   = *every* declared
//       field's symbol id, sorted ascending (only present/read when `...`
//       is absent)
//
// No heap allocation and no nested `call` happens anywhere in this handler,
// so (unlike op_let.S/op_call.S/op_mk_thunk.S) there is no need to save/
// restore IP(%rsi)/ENV(%rdi) around anything -- every register other than
// %rsi/%rdi/%r12/%r13/%r14 is free scratch for the whole handler body.

    .section .rodata
.Lmsg_cf_type:
    .ascii "asnix: lambda pattern argument is not a set, aborting\n"
    .equ ASNIX_MSG_CF_TYPE_LEN, . - .Lmsg_cf_type
.Lmsg_cf_missing:
    .ascii "asnix: lambda called without a required argument, aborting\n"
    .equ ASNIX_MSG_CF_MISSING_LEN, . - .Lmsg_cf_missing
.Lmsg_cf_extra:
    .ascii "asnix: lambda called with an unexpected argument, aborting\n"
    .equ ASNIX_MSG_CF_EXTRA_LEN, . - .Lmsg_cf_extra

    .text

    .globl asnix_op_check_formals
    .type asnix_op_check_formals, @function
asnix_op_check_formals:                 // a = required_count; b = pool_idx
    subq $8, %r12
    movq (%r12), %rax                   // rax = attrs tagged value (popped)
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lcf_type_error
    andq $-8, %rax                      // rax = Attrs*
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %rbx     // rbx = given entry count (n)
    leaq ASNIX_ATTRS_OFF_ENTRIES(%rax), %rbp   // rbp = given entries base

    movq ASNIX_IROP_OFF_A(%rsi), %r8    // r8 = required_count
    movq ASNIX_IROP_OFF_B(%rsi), %r9    // r9 = pool_idx

    // ---- Phase 1: every required field must be present among the given
    // entries (binary search, since the given Attrs is sorted by sym). ----
    xorq %r10, %r10                     // r10 = i, index into the required-syms pool region
.Lcf_req_loop:
    cmpq %r8, %r10
    jge .Lcf_req_done
    movq %r9, %r11
    addq $1, %r11
    addq %r10, %r11                     // r11 = pool_idx + 1 + i
    movq (%r13, %r11, 8), %r15          // r15 = required sym to find (persists through the search)

    xorq %rcx, %rcx                     // lo = 0
    movq %rbx, %rdx                     // hi = n
.Lcf_req_bs:
    cmpq %rdx, %rcx
    jge .Lcf_missing_error              // lo >= hi: not found among given entries
    leaq (%rcx, %rdx), %r11
    sarq $1, %r11                       // r11 = mid
    movq %r11, %rax
    shlq $4, %rax                       // rax = mid * ASNIX_ATTR_SIZE
    movq ASNIX_ATTR_OFF_SYM(%rbp, %rax, 1), %rax // rax = loaded sym at entries[mid]
    cmpq %r15, %rax
    je .Lcf_req_found
    jl .Lcf_req_right
    movq %r11, %rdx                     // hi = mid
    jmp .Lcf_req_bs
.Lcf_req_right:
    leaq 1(%r11), %rcx                  // lo = mid + 1
    jmp .Lcf_req_bs
.Lcf_req_found:
    incq %r10
    jmp .Lcf_req_loop
.Lcf_req_done:

    // ---- Phase 2 (skipped when the pattern has `...`): every given entry
    // must be one of the declared fields. ----
    movq (%r13, %r9, 8), %rax           // rax = total_count_or_minus1
    cmpq $-1, %rax
    je .Lcf_done
    movq %rax, %r15                     // r15 = total_count (required-sym search is done with it)
    movq %r9, %r10
    addq $1, %r10
    addq %r8, %r10                      // r10 = allowed-syms pool base index

    xorq %r8, %r8                       // r8 = j, index into given entries (required_count no longer needed)
.Lcf_extra_loop:
    cmpq %rbx, %r8
    jge .Lcf_done
    movq %r8, %rax
    shlq $4, %rax                       // rax = j * ASNIX_ATTR_SIZE
    movq ASNIX_ATTR_OFF_SYM(%rbp, %rax, 1), %r11 // r11 = given entries[j]'s sym (persists through the search)

    xorq %rcx, %rcx                     // lo = 0
    movq %r15, %rdx                     // hi = total_count
.Lcf_extra_bs:
    cmpq %rdx, %rcx
    jge .Lcf_extra_error                // lo >= hi: this given key isn't declared
    leaq (%rcx, %rdx), %rax
    sarq $1, %rax                       // rax = mid
    movq %r10, %r9
    addq %rax, %r9                      // r9 = allowed_base + mid (index, not yet dereferenced)
    movq (%r13, %r9, 8), %r9            // r9 = loaded allowed sym
    cmpq %r11, %r9
    je .Lcf_extra_found
    jl .Lcf_extra_right
    movq %rax, %rdx                     // hi = mid
    jmp .Lcf_extra_bs
.Lcf_extra_right:
    leaq 1(%rax), %rcx                  // lo = mid + 1
    jmp .Lcf_extra_bs
.Lcf_extra_found:
    incq %r8
    jmp .Lcf_extra_loop

.Lcf_done:
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lcf_type_error:
    leaq .Lmsg_cf_type(%rip), %rdi
    movq $ASNIX_MSG_CF_TYPE_LEN, %rsi
    jmp asnix_fatal
.Lcf_missing_error:
    leaq .Lmsg_cf_missing(%rip), %rdi
    movq $ASNIX_MSG_CF_MISSING_LEN, %rsi
    jmp asnix_fatal
.Lcf_extra_error:
    leaq .Lmsg_cf_extra(%rip), %rdi
    movq $ASNIX_MSG_CF_EXTRA_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_check_formals, . - asnix_op_check_formals
