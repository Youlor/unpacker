    /*
     * 64-bit instance field get.
     *
     * for: iget-wide
     */
    EXPORT_PC
    FETCH    w0, 1                         // w0<- field ref CCCC
    lsr      w1, wINST, #12                // w1<- B
    GET_VREG w1, w1                        // w1<- fp[B], the object pointer
    ldr      x2, [xFP, #OFF_FP_METHOD]     // w2<- referrer
    mov      x3, xSELF                     // w3<- self
    bl       artGet64InstanceFromCode
    ldr      x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     w2, wINST, #8, #4             // w2<- A
    PREFETCH_INST 2
    cmp      w3, #0
    cbnz     w3, MterpException            // bail out
    SET_VREG_WIDE x0, w2
    ADVANCE 2
    GET_INST_OPCODE ip                     // extract opcode from wINST
    GOTO_OPCODE ip                         // jump to next instruction
