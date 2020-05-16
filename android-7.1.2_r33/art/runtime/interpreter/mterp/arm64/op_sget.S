%default { "is_object":"0", "helper":"artGet32StaticFromCode", "extend":"" }
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field//BBBB */

    .extern $helper
    EXPORT_PC
    FETCH w0, 1                         // w0<- field ref BBBB
    ldr   x1, [xFP, #OFF_FP_METHOD]
    mov   x2, xSELF
    bl    $helper
    ldr   x3, [xSELF, #THREAD_EXCEPTION_OFFSET]
    lsr   w2, wINST, #8                 // w2<- AA
    $extend
    PREFETCH_INST 2
    cbnz  x3, MterpException            // bail out
.if $is_object
    SET_VREG_OBJECT w0, w2              // fp[AA]<- w0
.else
    SET_VREG w0, w2                     // fp[AA]<- w0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip
