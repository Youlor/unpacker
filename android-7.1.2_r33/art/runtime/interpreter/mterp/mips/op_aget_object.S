    /*
     * Array object get.  vAA <- vBB[vCC].
     *
     * for: aget-object
     */
    /* op vAA, vBB, vCC */
    FETCH_B(a2, 1, 0)                      #  a2 <- BB
    GET_OPA(rOBJ)                          #  rOBJ <- AA
    FETCH_B(a3, 1, 1)                      #  a3 <- CC
    EXPORT_PC()
    GET_VREG(a0, a2)                       #  a0 <- vBB (array object)
    GET_VREG(a1, a3)                       #  a1 <- vCC (requested index)
    JAL(artAGetObjectFromMterp)            #  v0 <- GetObj(array, index)
    lw   a1, THREAD_EXCEPTION_OFFSET(rSELF)
    PREFETCH_INST(2)                       #  load rINST
    bnez a1, MterpException
    SET_VREG_OBJECT(v0, rOBJ)              #  vAA <- v0
    ADVANCE(2)                             #  advance rPC
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
