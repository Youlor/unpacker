    /*
     * Synchronize on an object.
     */
    /* monitor-enter vAA */
    EXPORT_PC
    mov      r2, rINST, lsr #8           @ r2<- AA
    GET_VREG r0, r2                      @ r0<- vAA (object)
    mov      r1, rSELF                   @ r1<- self
    bl       artLockObjectFromCode
    cmp      r0, #0
    bne      MterpException
    FETCH_ADVANCE_INST 1
    GET_INST_OPCODE ip                   @ extract opcode from rINST
    GOTO_OPCODE ip                       @ jump to next instruction
