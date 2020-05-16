    /* iput-wide vA, vB, field@CCCC */
    .extern artSet64InstanceFromMterp
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12         @ r1<- B
    GET_VREG r1, r1                     @ r1<- fp[B], the object pointer
    ubfx     r2, rINST, #8, #4          @ r2<- A
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[A]
    ldr      r3, [rFP, #OFF_FP_METHOD]  @ r3<- referrer
    PREFETCH_INST 2
    bl       artSet64InstanceFromMterp
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2                          @ advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
