#ifndef ASNIX_IRPASS_DEADFRAME_H
#define ASNIX_IRPASS_DEADFRAME_H

/* Registers the "deadframe" bytecode pass -- see irpass_deadframe.c's own
 * header comment for what it does (dead zero-slot FRAME_ALLOC/FRAME_EXIT
 * bracket elimination, with the depth-fixup it needs to stay correct
 * when the bracket's own body still reaches past it). */
void asnix_irpass_register_deadframe(void);

#endif /* ASNIX_IRPASS_DEADFRAME_H */
