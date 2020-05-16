%default {}
    /*:
     * Generic 32-bit floating-point operation.
     *
     * For: add-float, sub-float, mul-float, div-float.
     * form: <op> f0, f0, f1
     */
    /* binop vAA, vBB, vCC */
    srl     a4, rINST, 8                # a4 <- AA
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    GET_VREG_FLOAT f0, a2               # f0 <- vBB
    GET_VREG_FLOAT f1, a3               # f1 <- vCC
    $instr                              # f0 <- f0 op f1
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_FLOAT f0, a4               # vAA <- f0
    GOTO_OPCODE v0                      # jump to next instruction
