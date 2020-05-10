    /*
     * Check to see if an object reference is an instance of a class.
     *
     * Most common situation is a non-null object, being compared against
     * an already-resolved class.
     */
    /* instance-of vA, vB, class//CCCC */
    .extern MterpInstanceOf
    EXPORT_PC
    lhu     a0, 2(rPC)                  # a0 <- CCCC
    srl     a1, rINST, 12               # a1 <- B
    dlsa    a1, a1, rFP, 2              # a1 <- &object
    ld      a2, OFF_FP_METHOD(rFP)      # a2 <- method
    move    a3, rSELF                   # a3 <- self
    jal     MterpInstanceOf             # (index, &obj, method, self)
    ld      a1, THREAD_EXCEPTION_OFFSET(rSELF)
    ext     a2, rINST, 8, 4             # a2 <- A
    PREFETCH_INST 2
    bnez    a1, MterpException
    ADVANCE 2                           # advance rPC
    SET_VREG v0, a2                     # vA <- v0
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
