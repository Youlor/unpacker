    /* move-wide/16 vAAAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH r3, 2                         @ r3<- BBBB
    FETCH r2, 1                         @ r2<- AAAA
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[BBBB]
    VREG_INDEX_TO_ADDR lr, r2           @ r2<- &fp[AAAA]
    ldmia   r3, {r0-r1}                 @ r0/r1<- fp[BBBB]
    FETCH_ADVANCE_INST 3                @ advance rPC, load rINST
    CLEAR_SHADOW_PAIR r2, r3, ip        @ Zero out the shadow regs
    stmia   lr, {r0-r1}                 @ fp[AAAA]<- r0/r1
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
