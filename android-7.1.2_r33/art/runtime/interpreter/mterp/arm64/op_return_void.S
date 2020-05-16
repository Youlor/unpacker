    .extern MterpThreadFenceForConstructor
    bl      MterpThreadFenceForConstructor
    ldr     w7, [xSELF, #THREAD_FLAGS_OFFSET]
    mov     x0, xSELF
    ands    w7, w7, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    b.ne    .L${opcode}_check
.L${opcode}_return:
    mov     x0, #0
    b       MterpReturn
.L${opcode}_check:
    bl      MterpSuspendCheck           // (self)
    b       .L${opcode}_return
