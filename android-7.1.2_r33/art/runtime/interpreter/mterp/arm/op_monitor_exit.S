    /*
     * Unlock an object.
     *
     * Exceptions that occur when unlocking a monitor need to appear as
     * if they happened at the following instruction.  See the Dalvik
     * instruction spec.
     */
    /* monitor-exit vAA */
    EXPORT_PC
    mov      r2, rINST, lsr #8          @ r2<- AA
    GET_VREG r0, r2                     @ r0<- vAA (object)
    mov      r1, rSELF                  @ r0<- self
    bl       artUnlockObjectFromCode    @ r0<- success for unlock(self, obj)
    cmp     r0, #0                      @ failed?
    bne     MterpException
    FETCH_ADVANCE_INST 1                @ before throw: advance rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
