    /*
     * Check to see if an object reference is an instance of a class.
     *
     * Most common situation is a non-null object, being compared against
     * an already-resolved class.
     */
    /* instance-of vA, vB, class@CCCC */
    EXPORT_PC
    FETCH     r0, 1                     @ r0<- CCCC
    mov       r1, rINST, lsr #12        @ r1<- B
    VREG_INDEX_TO_ADDR r1, r1           @ r1<- &object
    ldr       r2, [rFP, #OFF_FP_METHOD] @ r2<- method
    mov       r3, rSELF                 @ r3<- self
    bl        MterpInstanceOf           @ (index, &obj, method, self)
    ldr       r1, [rSELF, #THREAD_EXCEPTION_OFFSET]
    ubfx      r9, rINST, #8, #4         @ r9<- A
    PREFETCH_INST 2
    cmp       r1, #0                    @ exception pending?
    bne       MterpException
    ADVANCE 2                           @ advance rPC
    SET_VREG r0, r9                     @ vA<- r0
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction
