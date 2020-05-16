%default { "is_object":"0" }
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    movzwl  4(rPC), %ecx                    # ecx <- BBBB
    movzwl  2(rPC), %eax                    # eax <- AAAA
    GET_VREG rINST, %ecx
    .if $is_object
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 3
