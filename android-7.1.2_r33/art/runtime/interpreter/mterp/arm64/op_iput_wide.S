    /* iput-wide vA, vB, field//CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    FETCH    w0, 1                      // w0<- field ref CCCC
    lsr      w1, wINST, #12             // w1<- B
    GET_VREG w1, w1                     // w1<- fp[B], the object pointer
    ubfx     w2, wINST, #8, #4          // w2<- A
    VREG_INDEX_TO_ADDR x2, x2           // w2<- &fp[A]
    ldr      x3, [xFP, #OFF_FP_METHOD]  // w3<- referrer
    PREFETCH_INST 2
    bl       artSet64InstanceFromMterp
    cbnz     w0, MterpPossibleException
    ADVANCE  2                          // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
