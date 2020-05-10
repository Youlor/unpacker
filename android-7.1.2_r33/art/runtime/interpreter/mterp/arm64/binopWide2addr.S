%default {"preinstr":"", "instr":"add x0, x0, x1", "r0":"x0", "r1":"x1", "chkzero":"0"}
    /*
     * Generic 64-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "x0 = x0 op x1".
     * This must not be a function call, as we keep w2 live across it.
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long/2addr, sub-long/2addr, mul-long/2addr, div-long/2addr,
     *      and-long/2addr, or-long/2addr, xor-long/2addr,
     *      shl-long/2addr, shr-long/2addr, ushr-long/2addr, add-double/2addr,
     *      sub-double/2addr, mul-double/2addr, div-double/2addr, rem-double/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w1, wINST, #12              // w1<- B
    ubfx    w2, wINST, #8, #4           // w2<- A
    GET_VREG_WIDE $r1, w1               // x1<- vB
    GET_VREG_WIDE $r0, w2               // x0<- vA
    .if $chkzero
    cbz     $r1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    $preinstr
    $instr                              // result<- op
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE $r0, w2               // vAA<- result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */
