%default {}
    /*:
     * Generic 32-bit "/2addr" floating-point operation.
     *
     * For: add-float/2addr, sub-float/2addr, mul-float/2addr, div-float/2addr.
     * form: <op> f0, f0, f1
     */
    /* binop/2addr vA, vB */
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    GET_VREG_FLOAT f0, a2               # f0 <- vA
    GET_VREG_FLOAT f1, a3               # f1 <- vB
    $instr                              # f0 <- f0 op f1
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_FLOAT f0, a2               # vA <- f0
    GOTO_OPCODE v0                      # jump to next instruction
