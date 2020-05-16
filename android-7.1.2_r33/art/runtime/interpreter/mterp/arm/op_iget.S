%default { "is_object":"0", "helper":"artGet32InstanceFromCode"}
    /*
     * General instance field get.
     *
     * for: iget, iget-object, iget-boolean, iget-byte, iget-char, iget-short
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       $helper
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpPossibleException        @ bail out
    .if $is_object
    SET_VREG_OBJECT r0, r2                 @ fp[A]<- r0
    .else
    SET_VREG r0, r2                        @ fp[A]<- r0
    .endif
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction
