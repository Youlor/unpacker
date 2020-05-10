%default { "helper":"UndefinedInvokeHandler" }
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern $helper
    EXPORT_PC
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG0(%esp)
    leal    OFF_FP_SHADOWFRAME(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)
    movl    rPC, OUT_ARG2(%esp)
    REFRESH_INST ${opnum}
    movl    rINST, OUT_ARG3(%esp)
    call    SYMBOL($helper)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    RESTORE_IBASE
    FETCH_INST
    GOTO_NEXT
