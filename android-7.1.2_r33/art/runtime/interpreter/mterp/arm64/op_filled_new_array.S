%default { "helper":"MterpFilledNewArray" }
    /*
     * Create a new array with elements filled from registers.
     *
     * for: filled-new-array, filled-new-array/range
     */
    /* op vB, {vD, vE, vF, vG, vA}, class//CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, type//BBBB */
    .extern $helper
    EXPORT_PC
    add     x0, xFP, #OFF_FP_SHADOWFRAME
    mov     x1, xPC
    mov     x2, xSELF
    bl      $helper
    cbz     w0, MterpPossibleException
    FETCH_ADVANCE_INST 3                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
