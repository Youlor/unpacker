    /*
     * Array object get.  vAA <- vBB[vCC].
     *
     * for: aget-object
     */
    /* op vAA, vBB, vCC */
    FETCH_B w2, 1, 0                    // w2<- BB
    FETCH_B w3, 1, 1                    // w3<- CC
    EXPORT_PC
    GET_VREG w0, w2                     // w0<- vBB (array object)
    GET_VREG w1, w3                     // w1<- vCC (requested index)
    bl       artAGetObjectFromMterp     // (array, index)
    ldr      x1, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr      w2, wINST, #8               // w9<- AA
    PREFETCH_INST 2
    cbnz     w1, MterpException
    SET_VREG_OBJECT w0, w2
    ADVANCE 2
    GET_INST_OPCODE ip
    GOTO_OPCODE ip                      // jump to next instruction
