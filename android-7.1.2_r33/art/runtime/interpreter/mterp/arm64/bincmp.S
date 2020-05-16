    /*
     * Generic two-operand compare-and-branch operation.  Provide a "condition"
     * fragment that specifies the comparison to perform.
     *
     * For: if-eq, if-ne, if-lt, if-ge, if-gt, if-le
     */
    /* if-cmp vA, vB, +CCCC */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w0, wINST, #8, #4           // w0<- A
    GET_VREG w3, w1                     // w3<- vB
    GET_VREG w2, w0                     // w2<- vA
    FETCH_S wINST, 1                    // wINST<- branch offset, in code units
    cmp     w2, w3                      // compare (vA, vB)
    b.${condition} MterpCommonTakenBranchNoFlags
    cmp     wPROFILE, #JIT_CHECK_OSR    // possible OSR re-entry?
    b.eq    .L_check_not_taken_osr
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
