    /*
     * Check to see if a cast from one class to another is allowed.
     */
    /* check-cast vAA, class@BBBB */
    EXPORT_PC
    FETCH    r0, 1                      @ r0<- BBBB
    mov      r1, rINST, lsr #8          @ r1<- AA
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &object
    ldr      r2, [rFP, #OFF_FP_METHOD]  @ r2<- method
    mov      r3, rSELF                  @ r3<- self
    bl       MterpCheckCast             @ (index, &obj, method, self)
    PREFETCH_INST 2
    cmp      r0, #0
    bne      MterpPossibleException
    ADVANCE  2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
