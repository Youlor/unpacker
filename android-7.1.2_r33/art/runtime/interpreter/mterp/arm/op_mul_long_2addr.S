    /*
     * Signed 64-bit integer multiply, "/2addr" version.
     *
     * See op_mul_long for an explanation.
     *
     * We get a little tight on registers, so to avoid looking up &fp[A]
     * again we stuff it into rINST.
     */
    /* mul-long/2addr vA, vB */
    mov     r1, rINST, lsr #12          @ r1<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &fp[B]
    VREG_INDEX_TO_ADDR rINST, r9        @ rINST<- &fp[A]
    ldmia   r1, {r2-r3}                 @ r2/r3<- vBB/vBB+1
    ldmia   rINST, {r0-r1}              @ r0/r1<- vAA/vAA+1
    mul     ip, r2, r1                  @ ip<- ZxW
    umull   r1, lr, r2, r0              @ r1/lr <- ZxX
    mla     r2, r0, r3, ip              @ r2<- YxX + (ZxW)
    mov     r0, rINST                   @ r0<- &fp[A] (free up rINST)
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    add     r2, r2, lr                  @ r2<- r2 + low(ZxW + (YxX))
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r0, {r1-r2}                 @ vAA/vAA+1<- r1/r2
    GOTO_OPCODE ip                      @ jump to next instruction
