%default {"preinstr":"", "instr":"add x0, x1, x2", "result":"x0", "r1":"x1", "r2":"x2", "chkzero":"0"}
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = x1 op x2".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than x0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (w1).  Useful for integer division and modulus.
     *
     * For: add-long, sub-long, mul-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double, rem-double
     */
    /* binop vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    lsr     w2, w0, #8                  // w2<- CC
    and     w1, w0, #255                // w1<- BB
    GET_VREG_WIDE $r2, w2               // w2<- vCC
    GET_VREG_WIDE $r1, w1               // w1<- vBB
    .if $chkzero
    cbz     $r2, common_errDivideByZero  // is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    $preinstr
    $instr                              // $result<- op, w0-w4 changed
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG_WIDE $result, w4           // vAA<- $result
    GOTO_OPCODE ip                      // jump to next instruction
    /* 11-14 instructions */
