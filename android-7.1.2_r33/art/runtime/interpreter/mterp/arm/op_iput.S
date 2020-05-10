%default { "is_object":"0", "handler":"artSet32InstanceFromMterp" }
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field@CCCC */
    .extern $handler
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    GET_VREG r2, r2                     @ r2<- fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       $handler
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
