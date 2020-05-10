%default { "is_object":"0" }
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    movzwq  4(rPC), %rcx                    # ecx <- BBBB
    movzwq  2(rPC), %rax                    # eax <- AAAA
    GET_VREG %edx, %rcx
    .if $is_object
    SET_VREG_OBJECT %edx, %rax              # fp[A] <- fp[B]
    .else
    SET_VREG %edx, %rax                     # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
