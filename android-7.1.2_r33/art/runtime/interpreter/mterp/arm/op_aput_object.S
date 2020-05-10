    /*
     * Store an object into an array.  vBB[vCC] <- vAA.
     */
    /* op vAA, vBB, vCC */
    EXPORT_PC
    add     r0, rFP, #OFF_FP_SHADOWFRAME
    mov     r1, rPC
    mov     r2, rINST
    bl      MterpAputObject
    cmp     r0, #0
    beq     MterpPossibleException
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
