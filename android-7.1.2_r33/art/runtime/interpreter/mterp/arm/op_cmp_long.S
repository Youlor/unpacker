    /*
     * Compare two 64-bit values.  Puts 0, 1, or -1 into the destination
     * register based on the results of the comparison.
     */
    /* cmp-long vAA, vBB, vCC */
    FETCH r0, 1                         @ r0<- CCBB
    mov     r9, rINST, lsr #8           @ r9<- AA
    and     r2, r0, #255                @ r2<- BB
    mov     r3, r0, lsr #8              @ r3<- CC
    VREG_INDEX_TO_ADDR r2, r2           @ r2<- &fp[BB]
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[CC]
    ldmia   r2, {r0-r1}                 @ r0/r1<- vBB/vBB+1
    ldmia   r3, {r2-r3}                 @ r2/r3<- vCC/vCC+1
    cmp     r0, r2
    sbcs    ip, r1, r3                  @ Sets correct CCs for checking LT (but not EQ/NE)
    mov     ip, #0
    mvnlt   ip, #0                      @ -1
    cmpeq   r0, r2                      @ For correct EQ/NE, we may need to repeat the first CMP
    orrne   ip, #1
    FETCH_ADVANCE_INST 2                @ advance rPC, load rINST
    SET_VREG ip, r9                     @ vAA<- ip
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
