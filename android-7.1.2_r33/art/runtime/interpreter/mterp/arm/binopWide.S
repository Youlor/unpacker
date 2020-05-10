%default {"preinstr":"", "result0":"r0", "result1":"r1", "chkzero":"0"}
    /*
     * Generic 64-bit binary operation.  Provide an "instr" line that
     * specifies an instruction that performs "result = r0-r1 op r2-r3".
     * This could be an ARM instruction or a function call.  (If the result
     * comes back in a register other than r0, you can override "result".)
     *
     * If "chkzero" is set to 1, we perform a divide-by-zero check on
     * vCC (r1).  Useful for integer division and modulus.
     *
     * for: add-long, sub-long, div-long, rem-long, and-long, or-long,
     *      xor-long, add-double, sub-double, mul-double, div-double,
     *      rem-double
     *
     * IMPORTANT: you may specify "chkzero" or "preinstr" but not both.
     */
    /* binop vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r9, rINST        @ r9<- &fp[AA]
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    .if $chkzero
    orrs    ip, r2, r3                  @ second arg (r2-r3) is zero?
    beq     common_errDivideByZero
    .endif
    CLEAR_SHADOW_PAIR rINST, lr, ip     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    $preinstr                           @ optional op; may set condition codes
    $instr                              @ result<- op, r0-r3 changed
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r9, {$result0,$result1}     @ vAA/vAA+1<- $result0/$result1
    GOTO_OPCODE ip                      @ jump to next instruction
    /* 14-17 instructions */
