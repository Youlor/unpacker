    /* move-wide/from16 vAA, vBBBB */
    /* NOTE: regs can overlap, e.g. "move v6,v7" or "move v7,v6" */
    FETCH r3, 1                         @ r3<- BBBB
    mov     rINST, rINST, lsr #8        @ rINST<- AA
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[BBBB]
    VREG_INDEX_TO_ADDR r2, rINST        @ r2<- &fp[AA]
    ldmia   r3, {r0-r1}                 @ r0/r1<- fp[BBBB]
    CLEAR_SHADOW_PAIR rINST, ip, lr     @ Zero out the shadow regs
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    stmia   r2, {r0-r1}                 @ fp[AA]<- r0/r1
    GOTO_OPCODE ip                      @ jump to next instruction
