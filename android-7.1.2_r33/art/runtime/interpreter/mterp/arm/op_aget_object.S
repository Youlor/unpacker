    /*
     * Array object get.  vAA <- vBB[vCC].
     *
     * for: aget-object
     */
    /* op vAA, vBB, vCC */
    FETCH_B r2, 1, 0                    @ r2<- BB
    mov     r9, rINST, lsr #8           @ r9<- AA
    FETCH_B r3, 1, 1                    @ r3<- CC
    EXPORT_PC
    GET_VREG r0, r2                     @ r0<- vBB (array object)
    GET_VREG r1, r3                     @ r1<- vCC (requested index)
    bl       artAGetObjectFromMterp     @ (array, index)
    ldr      r1, [rSELF, #THREAD_EXCEPTION_OFFSET]
    PREFETCH_INST 2
    cmp      r1, #0
    bne      MterpException
    SET_VREG_OBJECT r0, r9
    ADVANCE 2
    GET_INST_OPCODE ip
    GOTO_OPCODE ip                      @ jump to next instruction
