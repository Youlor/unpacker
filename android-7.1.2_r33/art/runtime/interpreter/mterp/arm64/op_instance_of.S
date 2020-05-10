    /*
     * Check to see if an object reference is an instance of a class.
     *
     * Most common situation is a non-null object, being compared against
     * an already-resolved class.
     */
    /* instance-of vA, vB, class//CCCC */
    EXPORT_PC
    FETCH     w0, 1                     // w0<- CCCC
    lsr       w1, wINST, #12            // w1<- B
    VREG_INDEX_TO_ADDR x1, w1           // w1<- &object
    ldr       x2, [xFP, #OFF_FP_METHOD] // w2<- method
    mov       x3, xSELF                 // w3<- self
    bl        MterpInstanceOf           // (index, &obj, method, self)
    ldr       x1, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr       w2, wINST, #8             // w2<- A+
    and       w2, w2, #15               // w2<- A
    PREFETCH_INST 2
    cbnz      x1, MterpException
    ADVANCE 2                           // advance rPC
    SET_VREG w0, w2                     // vA<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
