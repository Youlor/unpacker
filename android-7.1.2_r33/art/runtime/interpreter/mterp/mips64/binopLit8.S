%default {"preinstr":"", "result":"a0", "chkzero":"0"}
    /*
     * Generic 32-bit "lit8" binary operation.  Provide an "instr" line
     * that specifies an instruction that performs "result = a0 op a1".
     * This could be an MIPS instruction or a function call.  (If the result
     * comes back in a register other than a0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * CC (a1).  Useful for integer division and modulus.
     *
     * For: add-int/lit8, rsub-int/lit8, mul-int/lit8, div-int/lit8,
     *      rem-int/lit8, and-int/lit8, or-int/lit8, xor-int/lit8,
     *      shl-int/lit8, shr-int/lit8, ushr-int/lit8
     */
    /* binop/lit8 vAA, vBB, #+CC */
    lbu     a3, 2(rPC)                  # a3 <- BB
    lb      a1, 3(rPC)                  # a1 <- sign-extended CC
    srl     a2, rINST, 8                # a2 <- AA
    GET_VREG a0, a3                     # a0 <- vBB
    .if $chkzero
    beqz    a1, common_errDivideByZero  # is second operand zero?
    .endif
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    $preinstr                           # optional op
    $instr                              # $result <- op, a0-a3 changed
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG $result, a2                # vAA <- $result
    GOTO_OPCODE v0                      # jump to next instruction

