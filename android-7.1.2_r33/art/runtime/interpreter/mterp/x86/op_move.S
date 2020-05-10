%default { "is_object":"0" }
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movzbl  rINSTbl, %eax                   # eax <- BA
    andb    $$0xf, %al                      # eax <- A
    shrl    $$4, rINST                      # rINST <- B
    GET_VREG rINST, rINST
    .if $is_object
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
