/*
 * Inter-instruction transfer stub.  Call out to MterpCheckBefore to handle
 * any interesting requests and then jump to the real instruction
 * handler.  Note that the call to MterpCheckBefore is done as a tail call.
 */
    .extern MterpCheckBefore
    EXPORT_PC
    REFRESH_IBASE
    dla     ra, artMterpAsmInstructionStart
    dla     t9, MterpCheckBefore
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    daddu   ra, ra, (${opnum} * 128)            # Addr of primary handler.
    jalr    zero, t9                            # (self, shadow_frame) Note: tail call.
