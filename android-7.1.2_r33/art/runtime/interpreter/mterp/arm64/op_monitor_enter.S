    /*
     * Synchronize on an object.
     */
    /* monitor-enter vAA */
    EXPORT_PC
    lsr      w2, wINST, #8               // w2<- AA
    GET_VREG w0, w2                      // w0<- vAA (object)
    mov      x1, xSELF                   // w1<- self
    bl       artLockObjectFromCode
    cbnz     w0, MterpException
    FETCH_ADVANCE_INST 1
    GET_INST_OPCODE ip                   // extract opcode from rINST
    GOTO_OPCODE ip                       // jump to next instruction
