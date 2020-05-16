%default { "helper":"artSet32StaticFromCode"}
    /*
     * General SPUT handler wrapper.
     *
     * for: sput, sput-boolean, sput-byte, sput-char, sput-short
     */
    /* op vAA, field@BBBB */
    EXPORT_PC
    FETCH   r0, 1                       @ r0<- field ref BBBB
    mov     r3, rINST, lsr #8           @ r3<- AA
    GET_VREG r1, r3                     @ r1<= fp[AA]
    ldr     r2, [rFP, #OFF_FP_METHOD]
    mov     r3, rSELF
    PREFETCH_INST 2                     @ Get next inst, but don't advance rPC
    bl      $helper
    cmp     r0, #0                      @ 0 on success, -1 on failure
    bne     MterpException
    ADVANCE 2                           @ Past exception point - now advance rPC
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
