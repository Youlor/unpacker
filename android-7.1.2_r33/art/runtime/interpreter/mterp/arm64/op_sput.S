%default { "helper":"artSet32StaticFromCode"}
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field//BBBB */
    EXPORT_PC
    FETCH   w0, 1                       // r0<- field ref BBBB
    lsr     w3, wINST, #8               // r3<- AA
    GET_VREG w1, w3                     // r1<= fp[AA]
    ldr     x2, [xFP, #OFF_FP_METHOD]
    mov     x3, xSELF
    PREFETCH_INST 2                     // Get next inst, but don't advance rPC
    bl      $helper
    cbnz    w0, MterpException          // 0 on success
    ADVANCE 2                           // Past exception point - now advance rPC
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
