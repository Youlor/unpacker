    /*
     * SGET_WIDE handler wrapper.
     *
     */
    /* sget-wide vAA, field//BBBB */

    .extern artGet64StaticFromCode
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    artGet64StaticFromCode
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w4, wINST, #8                 // w4<- AA
    cbnz  x3, MterpException            // bail out
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    SET_VREG_WIDE x0, w4
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
