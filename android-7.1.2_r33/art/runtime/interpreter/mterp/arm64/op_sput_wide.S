    /*
     * SPUT_WIDE handler wrapper.
     *
     */
    /* sput-wide vAA, field//BBBB */
    .extern artSet64IndirectStaticFromMterp
    EXPORT_PC
    FETCH   w0, 1                       // w0<- field ref BBBB
    ldr     x1, [xFP, #OFF_FP_METHOD]
    lsr     w2, wINST, #8               // w3<- AA
    VREG_INDEX_TO_ADDR x2, w2
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      artSet64IndirectStaticFromMterp
    cbnz    w0, MterpException          // 0 on success, -1 on failure
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
