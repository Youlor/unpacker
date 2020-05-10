    /* For: iget-object-quick */
    /* op vA, vB, offset//CCCC */
    .extern artIGetObjectFromMterp
    srl     a2, rINST, 12               # a2 <- B
    lhu     a1, 2(rPC)                  # a1 <- field byte offset
    EXPORT_PC
    GET_VREG_U a0, a2                   # a0 <- object we're operating on
    jal     artIGetObjectFromMterp      # (obj, offset)
    ld      a3, THREAD_EXCEPTION_OFFSET(rSELF)
    ext     a2, rINST, 8, 4             # a2 <- A
    PREFETCH_INST 2
    bnez    a3, MterpPossibleException  # bail out
    SET_VREG_OBJECT v0, a2              # fp[A] <- v0
    ADVANCE 2                           # advance rPC
    GET_INST_OPCODE v0                  # extract opcode from rINST
    GOTO_OPCODE v0                      # jump to next instruction
