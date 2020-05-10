%default {"preinstr":"", "result":"w0", "chkzero":"0"}
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = w0 op w1".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than w0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    FETCH_S w3, 1                       // w3<- ssssCCBB (sign-extended for CC
    lsr     w9, wINST, #8               // w9<- AA
    and     w2, w3, #255                // w2<- BB
    GET_VREG w0, w2                     // w0<- vBB
    asr    w1, w3, #8                   // w1<- ssssssCC (sign extended)
    .if $chkzero
    cbz     w1, common_errDivideByZero
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    $preinstr                           // optional op; may set condition codes
    $instr                              // $result<- op, w0-w3 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG $result, w9                // vAA<- $result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 10-12 instructions */
