    /* move-wide vA, vB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    rINST, rINST, #8, #4        @ rINST<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r2, rINST        @ r2<- &fp[A]
    ldmia   r3, {r0-r1}                 @ r0/r1<- fp[B]
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r2, {r0-r1}                 @ fp[A]<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
