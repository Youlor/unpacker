    /*
     * Create a new instance of a class.
     */
    /* new-instance vAA, class@BBBB */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rSELF
    mov     r2, rINST
    bl      MterpNewInstance           @ (shadow_frame, self, inst_data)
    cmp     r0, #0
    beq     MterpPossibleException
    FETCH_ADVANCE_INST 2               @ advance rPC, load rINST
    GET_INST_OPCODE ip                 @ extract opcode from rINST
    GOTO_OPCODE ip                     @ jump to next instruction
