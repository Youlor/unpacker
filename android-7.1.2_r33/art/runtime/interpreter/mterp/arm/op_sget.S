%default { "is_object":"0", "helper":"artGet32StaticFromCode" }
    /*
     * General SGET handler wrapper.
     *
     * for: sget, sget-object, sget-boolean, sget-byte, sget-char, sget-short
     */
    /* op vAA, field@BBBB */

    .extern $helper
    EXPORT_PC
    FETCH r0, 1                         @ r0<- field ref BBBB
    ldr   r1, [rFP, #OFF_FP_METHOD]
    mov   r2, rSELF
    bl    $helper
    ldr   r3, [rSELF, #THREAD_EXCEPTION_OFFSET]
    mov   r2, rINST, lsr #8             @ r2<- AA
    PREFETCH_INST 2
    cmp   r3, #0                        @ Fail to resolve?
    bne   MterpException                @ bail out
.if $is_object
    SET_VREG_OBJECT r0, r2              @ fp[AA]<- r0
.else
    SET_VREG r0, r2                     @ fp[AA]<- r0
.endif
    ADVANCE 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip
