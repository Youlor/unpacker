    /*
     * Check to see if a cast from one class to another is allowed.
     */
    /* check-cast vAA, class//BBBB */
    EXPORT_PC
    FETCH    w0, 1                      // w0<- BBBB
    lsr      w1, wINST, #8              // w1<- AA
    VREG_INDEX_TO_ADDR x1, w1           // w1<- &object
    ldr      x2, [xFP, #OFF_FP_METHOD]  // w2<- method
    mov      x3, xSELF                  // w3<- self
    bl       MterpCheckCast             // (index, &obj, method, self)
    PREFETCH_INST 2
    cbnz     w0, MterpPossibleException
    ADVANCE  2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
