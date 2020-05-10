%default { "helper":"UndefinedInvokeHandler" }
/*
 * Generic invoke handler wrapper.
 */
    /* op vB, {vD, vE, vF, vG, vA}, class@CCCC */
    /* op {vCCCC..v(CCCC+AA-1)}, meth@BBBB */
    .extern $helper
    EXPORT_PC
    movq    rSELF, OUT_ARG0
    leaq    OFF_FP_SHADOWFRAME(rFP), OUT_ARG1
    movq    rPC, OUT_ARG2
    REFRESH_INST ${opnum}
    movl    rINST, OUT_32_ARG3
    call    SYMBOL($helper)
    testb   %al, %al
    jz      MterpException
    ADVANCE_PC 3
    call    SYMBOL(MterpShouldSwitchInterpreters)
    testb   %al, %al
    jnz     MterpFallback
    FETCH_INST
    GOTO_NEXT
