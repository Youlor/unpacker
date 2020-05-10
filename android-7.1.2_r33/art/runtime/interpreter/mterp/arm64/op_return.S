    /*
     * Return a 32-bit value.
     *
     * for: return, return-object
     */
    /* op vAA */
    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     x0, xSELF
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .L${opcode}_check
.L${opcode}_return:
    lsr     w2, wINST, #8               // r2<- AA
    GET_VREG w0, w2                     // r0<- vAA
    b       MterpReturn
.L${opcode}_check:
    bl      MterpSuspendCheck           // (self)
    b       .L${opcode}_return
