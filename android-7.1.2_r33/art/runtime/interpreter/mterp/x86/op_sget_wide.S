/*
 * SGET_WIDE handler wrapper.
 *
 */
    /* sget-wide vAA, field@BBBB */
    .extern artGet64StaticFromCode
    EXPORT_PC
    movzwl  2(rPC), %eax
    movl    %eax, OUT_ARG0(%esp)            # field ref CCCC
    movl    OFF_FP_METHOD(rFP), %eax
    movl    %eax, OUT_ARG1(%esp)            # referrer
    movl    rSELF, %ecx
    movl    %ecx, OUT_ARG2(%esp)            # self
    call    SYMBOL(artGet64StaticFromCode)
    movl    rSELF, %ecx
    cmpl    $$0, THREAD_EXCEPTION_OFFSET(%ecx)
    jnz     MterpException
    SET_VREG %eax, rINST                    # fp[A]<- low part
    SET_VREG_HIGH %edx, rINST               # fp[A+1]<- high part
    RESTORE_IBASE_FROM_SELF %ecx
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
