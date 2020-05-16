    /*
     * Generic one-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * for: if-eqz, if-nez, if-ltz, if-gez, if-gtz, if-lez
     */
    /* if-cmp vAA, +BBBB */
    lsr     w0, wINST, #8               // w0<- AA
    GET_VREG w2, w0                     // w2<- vAA
    FETCH_S wINST, 1                    // w1<- branch offset, in code units
    cmp     w2, #0                      // compare (vA, 0)
    b.${condition} MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
