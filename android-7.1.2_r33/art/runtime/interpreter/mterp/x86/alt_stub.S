/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Unlike the Arm handler, we can't do this as a tail call
 * because rIBASE is caller save and we need to reload it.
 *
 * Note that unlike in the Arm implementation, we should never arrive
 * here with a zero breakFlag because we always refresh rIBASE on
 * return.
 */
    .extern MterpCheckBefore
    EXPORT_PC

    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    call    SYMBOL(MterpCheckBefore)        # (self, shadow_frame)
    REFRESH_IBASE
    jmp     .L_op_nop+(${opnum}*${handler_size_bytes})
