    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    mov     r0, rINST, lsr #8           @ r0<- AA
    GET_VREG r0, r0                     @ r0<- vAA
    FETCH_S rINST, 1                    @ rINST<- branch offset, in code units
    cmp     r0, #0                      @ compare (vA, 0)
    b${condition} MterpCommonTakenBranchNoFlags
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    beq     .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
