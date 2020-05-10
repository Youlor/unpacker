    /*
     * Array object get.  vAA <- vBB[vCC].
     *
     * for: aget-object
     */
    /* op vAA, vBB, vCC */
    .extern artAGetObjectFromMterp
    lbu     a2, 2(rPC)                  # a2 <- BB
    lbu     a3, 3(rPC)                  # a3 <- CC
    EXPORT_PC
    GET_VREG_U a0, a2                   # a0 <- vBB (array object)
    GET_VREG a1, a3                     # a1 <- vCC (requested index)
    jal     artAGetObjectFromMterp      # (array, index)
    ld      a1, THREAD_EXCEPTION_OFFSET(rSELF)
    srl     a4, rINST, 8                # a4 <- AA
    PREFETCH_INST 2
    bnez    a1, MterpException
    SET_VREG_OBJECT v0, a4              # vAA <- v0
    ADVANCE 2
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
