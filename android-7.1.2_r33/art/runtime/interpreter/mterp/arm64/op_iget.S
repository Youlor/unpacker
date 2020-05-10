%default { "extend":"", "is_object":"0", "helper":"artGet32InstanceFromCode"}
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       $helper
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    $extend
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cbnz     x3, MterpPossibleException    // bail out
    .if $is_object
    SET_VREG_OBJECT w0, w2                 // fp[A]<- w0
    .else
    SET_VREG w0, w2                        // fp[A]<- w0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from rINST
    GOTO_OPCODE ip                         // jump to next instruction
