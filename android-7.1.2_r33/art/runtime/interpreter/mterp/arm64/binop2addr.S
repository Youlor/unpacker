%default {"preinstr":"", "result":"w0", "chkzero":"0"}
    /*
     * Generic 32-bit "/2addr" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/2addr, sub-int/2addr, mul-int/2addr, div-int/2addr,
     *      rem-int/2addr, and-int/2addr, or-int/2addr, xor-int/2addr,
     *      shl-int/2addr, shr-int/2addr, ushr-int/2addr, add-float/2addr,
     *      sub-float/2addr, mul-float/2addr, div-float/2addr, rem-float/2addr
     */
    /* binop/2addr vA, vB */
    lsr     w3, wINST, #12              // w3<- B
    ubfx    w9, wINST, #8, #4           // w9<- A
    GET_VREG w1, w3                     // w1<- vB
    GET_VREG w0, w9                     // w0<- vA
    .if $chkzero
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 1                // advance rPC, load rINST
    $preinstr                           // optional op; may set condition codes
    $instr                              // $result<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG $result, w9                // vAA<- $result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-13 instructions */
