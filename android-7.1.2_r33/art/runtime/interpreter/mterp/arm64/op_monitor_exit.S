    /*
     * Unlock an object.
     *
     * Exceptions that occur when unlocking a monitor need to appear as
     * if they happened at the following instruction.  See the Dalvik
     * instruction spec.
     */
    /* monitor-exit vAA */
    EXPORT_PC
    lsr      w2, wINST, #8              // w2<- AA
    GET_VREG w0, w2                     // w0<- vAA (object)
    mov      x1, xSELF                  // w0<- self
    bl       artUnlockObjectFromCode    // w0<- success for unlock(self, obj)
    cbnz     w0, MterpException
    FETCH_ADVANCE_INST 1                // before throw: advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
