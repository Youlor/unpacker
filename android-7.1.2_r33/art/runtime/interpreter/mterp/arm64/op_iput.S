%default { "is_object":"0", "handler":"artSet32InstanceFromMterp" }
    /*
     * General 32-bit instance field put.
     *
     * for: iput, iput-object, iput-boolean, iput-byte, iput-char, iput-short
     */
    /* op vA, vB, field//CCCC */
    .extern $handler
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    GET_VREG w2, w2                     // w2<- fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       $handler
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
