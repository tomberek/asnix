#include "asnix/heap.h"
#include "asnix/ir.h"
#include "asnix/stackcheck.h"
#include "asnix/value.h"

    .section .rodata
.Lmsg_blackhole:
    .ascii "asnix: infinite recursion forcing a thunk (blackhole), aborting\n"
    .equ ASNIX_MSG_BLACKHOLE_LEN, . - .Lmsg_blackhole
.Lmsg_reeval_failed:
    .ascii "asnix: re-evaluation of a thunk whose first evaluation failed, aborting\n"
    .equ ASNIX_MSG_REEVAL_FAILED_LEN, . - .Lmsg_reeval_failed

    .text

    .globl asnix_op_select
    .type asnix_op_select, @function
asnix_op_select:                        // a = symbol id; pops an attrset, pushes the matching value
    subq $8, %r12
    movq (%r12), %rax                   // rax = attrs tagged value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_ATTRS, %rcx
    jne .Lselect_type_error

    // M88 (lazy `//` union-layering, heap.h): a UnionHdr has a NEGATIVE
    // `count` at offset 0 -- a single sign-bit check (no call overhead
    // paid) on the common already-flat case, which is why this is
    // checked inline here rather than always calling through.
    //
    // fork (unionhdr-probe): the union (negative-count) case used to
    // call asnix_bshared_attrs_resolve, which ALWAYS fully flattens +
    // caches every leaf merged together before this function's own
    // single-key binary search ever runs -- even though this is a
    // point lookup that only wants ONE value. That made M88's lazy
    // deferral provide essentially zero benefit the moment an attrset
    // is read even once via `a.b` (the single most common real-world
    // attrs operation), which is nearly always true for a real `//`
    // result. Now calls asnix_bshared_attrs_probe instead: a bounded
    // multi-leaf walk (highest-priority leaf first, matching `//`'s
    // right-biased override semantics) that binary-searches each flat
    // leaf directly and returns on the first hit, never materializing
    // or allocating -- the same thing real Nix's own layered
    // Bindings::get() does (walk the layer chain directly; only full
    // iteration ever needs a flattened view). See
    // asnix_bshared_attrs_probe's own comment (builtins_shared.c) for
    // the full reasoning and scope note (deliberately NOT applied to
    // asnix_bshared_attrs_find/_count/_sym/_value, whose 35+ callers
    // expect a stable positional index into a flat form).
    movq %rax, %r9                      // r9 = tagged value, kept across the maybe-call below
    andq $-8, %rax                      // rax = real Attrs* (untagged)
    movq ASNIX_ATTRS_OFF_COUNT(%rax), %r8 // r8 = count (maybe negative -- union header)
    testq %r8, %r8
    jns .Lselect_resolved
    movl ASNIX_IROP_OFF_A(%rsi), %r10d  // r10d = target symbol id, read now (safe: %rsi
                                         // untouched since entry) before %rsi/%rdi get pushed
    pushq %rsi                          // save IP
    pushq %rdi                          // save ENV
    subq $16, %rsp                      // 16-byte-aligned scratch slot for out_value (only
                                         // 8 used -- keeps the call's own stack-alignment
                                         // requirement simple alongside the 2 pushes above)
    movq %r9, %rdi                      // arg1 = tagged attrs value
    movl %r10d, %esi                    // arg2 = symbol id
    movq %rsp, %rdx                     // arg3 = &out_value (the scratch slot just reserved)
    call asnix_bshared_attrs_probe
    movq (%rsp), %r9                    // r9 = probed value (meaningful only if found; cheap
                                         // to load unconditionally rather than branch around)
    addq $16, %rsp
    popq %rdi
    popq %rsi
    // M154: `testl %eax, %eax` MUST happen here, immediately before the
    // `jz` -- found and fixed a real pre-existing bug in this exact spot
    // (landed by M111, never caught by any test): the ORIGINAL ordering
    // tested %eax right after the call but then ran `addq $16, %rsp`
    // before branching on the result. `addq` unconditionally clobbers
    // ZF/SF/CF/OF based on ITS OWN result (here, always a nonzero stack
    // address), silently overwriting the `testl`'s flags. On a genuine
    // miss (`eax==0`), the clobbered ZF read back as 0 (since rsp+16 is
    // never literally zero), so `jz` wasn't taken -- the miss path fell
    // through treating garbage stack-slot contents as a "found" value,
    // corrupting %rax with whatever was left in the never-written
    // out_value scratch slot and then segfaulting soon after (reached
    // via .Lselect_resolve's Cell/LazyApp/ThunkMap dispatch misreading
    // that garbage as a tagged pointer). The hit path happened to still
    // work by coincidence (its own clobbered ZF read back as 0 too,
    // which is the correct "don't take jz" outcome for a hit) -- this is
    // exactly why `make test`/`test-nix-conformance`/`test-nixpkgs-hash`
    // never caught it: every real nixpkgs point lookup that reaches a
    // UnionHdr apparently always HITS (confirmed by direct repro: a
    // genuinely-missing key on a real UnionHdr, e.g. `(base // {extra=1;})
    // .nope` with base >=16 entries, reliably segfaulted before this fix
    // and returns the correct fatal "attribute missing" error after it).
    testl %eax, %eax
    jz .Lselect_probe_missing
    movq %r9, %rax                      // rax = found value -- feed into the SAME thunk-
                                         // forcing/Cell-check/push logic every other select
                                         // result already goes through, below
    jmp .Lselect_resolve
.Lselect_probe_missing:
    movl ASNIX_IROP_OFF_A(%rsi), %edx   // %rsi restored above -- .Lselect_missing's own
                                         // handler expects %rdx = symbol id, same as the
                                         // already-flat path sets it at .Lselect_resolved
    jmp .Lselect_missing
.Lselect_resolved:

    movl ASNIX_IROP_OFF_A(%rsi), %edx   // rdx = target symbol id
    // %r8 already holds hi = count (loaded above)
    xorq %rcx, %rcx                     // rcx = lo = 0
.Lselect_loop:
    cmpq %r8, %rcx
    jge .Lselect_missing
    movq %rcx, %r9
    addq %r8, %r9
    shrq $1, %r9                        // r9 = mid
    movq %r9, %r10
    imulq $ASNIX_ATTR_SIZE, %r10         // r10 = mid * ASNIX_ATTR_SIZE (12 -- not a power of 2)
    movl ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_SYM(%rax, %r10, 1), %r11d // zero-extends into %r11;
        // MUST be 32-bit: M103 removed the pad word, so sym (4 bytes) is
        // immediately followed by this same entry's value bytes -- a movq
        // here would read into them
    cmpq %rdx, %r11
    je .Lselect_found
    jl .Lselect_go_right
    movq %r9, %r8                       // hi = mid
    jmp .Lselect_loop
.Lselect_go_right:
    leaq 1(%r9), %rcx                   // lo = mid + 1
    jmp .Lselect_loop
.Lselect_found:
    movq ASNIX_ATTRS_OFF_ENTRIES+ASNIX_ATTR_OFF_VALUE(%rax, %r10, 1), %rax

.Lselect_resolve:                       // M92: re-entered after unwrapping a Cell (see
                                         // .Lselect_check_cell below) -- see op_var_ref.S's
                                         // matching comment.
    // M6: force if the selected entry holds an unforced thunk (laziness
    // -- attrset binding values are deferred the same way `let` bindings
    // are; see op_mk_thunk.S / compiler.c).
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_THUNK, %rcx
    jne .Lselect_check_cell

    andq $-8, %rax
    // M103: OFF_STATE is tagged now -- UNFORCED: this word IS the clean
    // IrProgram* (tag bits 0); otherwise: the bare state tag alone.
    // Extract into %r8 (dead since the binary search above) for
    // dispatch, keeping %rcx (the possibly-a-pointer original word)
    // intact for the UNFORCED path below.
    movq ASNIX_THUNK_OFF_STATE(%rax), %rcx
    movq %rcx, %r8
    andq $ASNIX_TAG_MASK, %r8
    cmpq $ASNIX_THUNK_STATE_FORCED, %r8
    je .Lselect_forced
    cmpq $ASNIX_THUNK_STATE_IN_PROGRESS, %r8
    je .Lselect_blackhole
    cmpq $ASNIX_THUNK_STATE_FAILED, %r8
    je .Lselect_reraise

    movq $ASNIX_THUNK_STATE_IN_PROGRESS, ASNIX_THUNK_OFF_STATE(%rax)

    movq %rsi, %rdx
    addq $ASNIX_IROP_SIZE, %rdx
    ASNIX_CHECK_RETSP %r8               // %r8 dead (already consumed into the state tag above);
                                         // %rcx must survive -- it holds the clean prog pointer
    movq %rdx, ASNIX_RETFRAME_OFF_IP(%r14)
    movq %rdi, ASNIX_RETFRAME_OFF_ENV(%r14)
    movq %r13, ASNIX_RETFRAME_OFF_POOL(%r14)
    movq %rax, ASNIX_RETFRAME_OFF_FORCE_TARGET(%r14)
    addq $ASNIX_RETFRAME_SIZE, %r14

    // M103: %rcx already holds the clean prog pointer (read above, tag
    // bits were 0) -- no second memory read needed (the old separate
    // OFF_PROG field is gone).
    movq ASNIX_IRPROG_OFF_POOL(%rcx), %r13
    movq ASNIX_IRPROG_OFF_OPS(%rcx), %rsi
    movq ASNIX_THUNK_OFF_ENV(%rax), %rdi // last read of ENV's original meaning -- safe to repurpose below
    // M82: push onto the intrusive IN_PROGRESS stack -- see
    // stacklimits.S's asnix_inprogress_head comment. %rcx is dead here
    // (overwritten immediately below, same as before this M100 change).
    movq asnix_inprogress_head(%rip), %rcx
    movq %rcx, ASNIX_THUNK_OFF_ENV(%rax)
    movq %rax, asnix_inprogress_head(%rip)
    movq (%rsi), %rax
    jmp *%rax

.Lselect_forced:
    movq ASNIX_THUNK_OFF_ENV(%rax), %rax
    jmp .Lselect_push                    // perf: skip the resolve re-check entirely, see
                                          // op_var_ref.S's matching .Lvar_ref_forced comment for
                                          // the invariant proof (a forced thunk's cached value can
                                          // never be a Thunk or a Cell)

    // M92: not a thunk -- check whether it's a Cell instead (see heap.h's
    // ASNIX_CELL_MARKER comment). %rcx still holds the tag bits from the
    // check just above (the only path that reaches this label).
.Lselect_check_cell:
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lselect_push
    movq %rax, %r8
    andq $-8, %r8                        // clear the low 3 tag bits...
    shlq $16, %r8                        // ...and the high 16 bits a packed M118 ThunkMap word's
    shrq $16, %r8                        // index occupies (a no-op for every other PTR kind --
                                          // see heap.h's ASNIX_THUNKMAP_MARKER comment) --
                                          // r8 = masked base pointer
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lselect_check_lazyapp           // PTR but not a Cell -- maybe a LazyApp instead

    // It's a Cell -- M166: inline Cell-chain walk, same hand-rolled loop
    // as op_var_ref.S's .Lvar_ref_cell_loop (see that file's comment for
    // the full rationale). %r8 already holds the masked base pointer of
    // a CONFIRMED Cell (the check just above). %r9/%r10/%r11 are free
    // scratch here (the binary search above is done; %r9 held the
    // Cell-marker just checked, now dead). %rdx is free too (last
    // written setting up the binary search's target symbol id, dead
    // since .Lselect_loop/.Lselect_found ran). Measured on d.nix: this
    // call site alone hits 77,236 times (122,709 total chase hops across
    // all three op_select.S/op_select_or.S/op_with.S call sites
    // combined) -- genuinely hot, not the rare case M162's own comment
    // assumed.
    movq $ASNIX_CELL_CHASE_LIMIT, %rdx
.Lselect_cell_loop:
    movq ASNIX_CELL_OFF_TARGET(%r8), %r10 // r10 = target pointer (Value* into an Env's values[])
    movq (%r10), %rax                     // rax = *target -- the actual next raw Value
    movq %rax, %rcx
    andq $ASNIX_TAG_MASK, %rcx
    cmpq $ASNIX_TAG_PTR, %rcx
    jne .Lselect_resolve                  // not even a PTR -- can't be a Cell, done chasing
    movq %rax, %r8
    andq $-8, %r8
    shlq $16, %r8
    shrq $16, %r8                         // r8 = masked base pointer of the new value
    movq ASNIX_CELL_OFF_MARKER(%r8), %r9
    cmpq $ASNIX_CELL_MARKER, %r9
    jne .Lselect_resolve                  // PTR but not a Cell -- done, re-dispatch on %rax
    decq %rdx
    jnz .Lselect_cell_loop
    jmp .Lselect_blackhole                // chase limit exceeded -- a genuine alias cycle

    // M113: not a Cell either -- maybe a LazyApp (heap.h's
    // ASNIX_LAZYAPP_MARKER comment). %r9 already holds the marker value
    // from the Cell check just above -- see op_var_ref.S's matching
    // comment.
.Lselect_check_lazyapp:
    cmpq $ASNIX_LAZYAPP_MARKER, %r9
    je .Lselect_lazyapp
    cmpq $ASNIX_LAZYAPP_FORCED_MARKER, %r9
    je .Lselect_lazyapp
    jmp .Lselect_check_thunkmap          // PTR but not Cell/LazyApp -- maybe a ThunkMap instead
.Lselect_lazyapp:
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi                      // arg1 = the LazyApp value
    call asnix_resolve_lazyapp
    popq %rdi
    popq %rsi
    jmp .Lselect_resolve

    // M118: not a Cell/LazyApp either -- maybe a ThunkMap (heap.h's
    // ASNIX_THUNKMAP_MARKER comment) -- see op_var_ref.S's matching
    // comment.
.Lselect_check_thunkmap:
    cmpq $ASNIX_THUNKMAP_MARKER, %r9
    jne .Lselect_push                    // PTR but not Cell/LazyApp/ThunkMap: a real value
    pushq %rsi
    pushq %rdi
    movq %rax, %rdi                      // arg1 = the ThunkMap value (original, unmasked)
    call asnix_resolve_thunkmap
    popq %rdi
    popq %rsi
    jmp .Lselect_resolve
.Lselect_push:
    ASNIX_CHECK_VALSTACK %rcx
    movq %rax, (%r12)
    addq $8, %r12
    addq $ASNIX_IROP_SIZE, %rsi
    movq (%rsi), %rax
    jmp *%rax

.Lselect_reraise:
    // M82: see op_var_ref.S's matching .Lvar_ref_reraise comment --
    // re-forcing a previously-FAILED thunk re-raises instead of re-
    // running the body or blackholing.
    leaq .Lmsg_reeval_failed(%rip), %rdi
    movq $ASNIX_MSG_REEVAL_FAILED_LEN, %rsi
    jmp asnix_tryeval_raise_or_fatal

.Lselect_type_error:
    // M50: names the actual type of the non-attrset target (real `call`
    // into C, not a tail `jmp` -- same reasoning as .Lselect_missing
    // below/M36: %rsp is already 16-byte aligned entering this handler,
    // asnix_bshared_select_type_fatal is _Noreturn so the pushed return
    // address is never used, and this only ever runs immediately before
    // exit(1), so it costs nothing on the hot (successful select) path
    // above. %rax still holds the original tagged Value here (loaded at
    // handler entry, not yet `andq $-8`'d).
    movq %rax, %rdi
    call asnix_bshared_select_type_fatal
.Lselect_missing:
    // Names the attribute (real Nix's own "attribute 'x' missing"
    // wording) -- a real `call` into C (not a tail `jmp` like the other
    // two fatal paths below), safe here because %rsp is already 16-byte
    // aligned entering this handler (same standing dispatch-loop
    // invariant op_call.S/op_mk_thunk.S's own direct C calls rely on) and
    // asnix_bshared_select_missing_fatal is _Noreturn, so the harmless
    // pushed return address is never used. Only reached immediately
    // before exit(1), so this costs nothing on the hot (successful
    // select) path above.
    movq %rdx, %rdi
    call asnix_bshared_select_missing_fatal
.Lselect_blackhole:
    leaq .Lmsg_blackhole(%rip), %rdi
    movq $ASNIX_MSG_BLACKHOLE_LEN, %rsi
    jmp asnix_fatal
    .size asnix_op_select, . - asnix_op_select
