%default {}
    /*:
     * Generic 64-bit "/2addr" floating-point operation.
     *
     * For: add-double/2addr, sub-double/2addr, mul-double/2addr, div-double/2addr.
     * form: <op> f0, f0, f1
     */
    /* binop/2addr vA, vB */
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG_DOUBLE f0, a2              # f0 <- vA
    GET_VREG_DOUBLE f1, a3              # f1 <- vB
    $instr                              # f0 <- f0 op f1
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_DOUBLE f0, a2              # vA <- f0
    GOTO_OPCODE v0                      # jump to next instruction
