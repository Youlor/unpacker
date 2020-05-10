/*
 * Check to see if an object reference is an instance of a class.
 *
 * Most common situation is a non-null object, being compared against
 * an already-resolved class.
 */
    /* instance-of vA, vB, class@CCCC */
    EXPORT_PC
    movzwl  2(rPC), OUT_32_ARG0             # OUT_32_ARG0 <- CCCC
    movl    rINST, %eax                     # eax <- BA
    sarl    $$4, %eax                       # eax <- B
    leaq    VREG_ADDRESS(%rax), OUT_ARG1    # Get object address
    movq    OFF_FP_METHOD(rFP), OUT_ARG2
    movq    rSELF, OUT_ARG3
    call    SYMBOL(MterpInstanceOf)         # (index, &obj, method, self)
    movsbl  %al, %eax
    movq    rSELF, %rcx
    cmpq    $$0, THREAD_EXCEPTION_OFFSET(%rcx)
    jnz     MterpException
    andb    $$0xf, rINSTbl                  # rINSTbl <- A
    SET_VREG %eax, rINSTq
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
