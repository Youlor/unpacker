    /*
     * 64-bit instance field get.
     *
     * for: iget-wide
     */
    EXPORT_PC
    FETCH    r0, 1                         @ r0<- field ref CCCC
    mov      r1, rINST, lsr #12            @ r1<- B
    GET_VREG r1, r1                        @ r1<- fp[B], the object pointer
    ldr      r2, [rFP, #OFF_FP_METHOD]     @ r2<- referrer
    mov      r3, rSELF                     @ r3<- self
    bl       artGet64InstanceFromCode
    ldr      r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx     r2, rINST, #8, #4             @ r2<- A
    PREFETCH_INST 2
    cmp      r3, #0
    bne      MterpException                @ bail out
    CLEAR_SHADOW_PAIR r2, ip, lr           @ Zero out the shadow regs
    VREG_INDEX_TO_ADDR r3, r2              @ r3<- &fp[A]
    stmia    r3, {r0-r1}                   @ fp[A]<- r0/r1
    ADVANCE 2
    GET_INST_OPCODE ip                     @ extract opcode from rINST
    GOTO_OPCODE ip                         @ jump to next instruction
