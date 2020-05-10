%default {}
    /*:
     * Generic 32-bit floating-point operation.
     *
     * For: add-float, sub-float, mul-float, div-float
     * form: <op> s0, s0, s1
     */
    /* floatop vAA, vBB, vCC */
    FETCH w0, 1                         // r0<- CCBB
    lsr     w1, w0, #8                  // r2<- CC
    and     w0, w0, #255                // r1<- BB
    GET_VREG  s1, w1
    GET_VREG  s0, w0
    $instr                              // s0<- op
    lsr     w1, wINST, #8               // r1<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG  s0, w1
    GOTO_OPCODE ip                      // jump to next instruction
