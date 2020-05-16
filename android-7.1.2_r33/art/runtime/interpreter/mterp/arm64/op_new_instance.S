    /*
     * Create a new instance of a class.
     */
    /* new-instance vAA, class//BBBB */
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xSELF
    mov     w2, wINST
    bl      MterpNewInstance           // (shadow_frame, self, inst_data)
    cbz     w0, MterpPossibleException
    FETCH_ADVANCE_INST 2               // advance rPC, load rINST
    GET_INST_OPCODE ip                 // extract opcode from rINST
    GOTO_OPCODE ip                     // jump to next instruction
