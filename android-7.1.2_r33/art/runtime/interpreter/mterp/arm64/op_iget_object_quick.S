    /* For: iget-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    EXPORT_PC
    GET_VREG w0, w2                     // w0<- object we're operating on
    bl      artIGetObjectFromMterp      // (obj, offset)
    ldr     x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx    w2, wINST, #8, #4           // w2<- A
    PREFETCH_INST 2
    cbnz    w3, MterpPossibleException      // bail out
    SET_VREG_OBJECT w0, w2              // fp[A]<- w0
    ADVANCE 2                           // advance rPC
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
