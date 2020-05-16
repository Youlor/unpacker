%default { "is_object":"0" }
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    movl    rINST, %eax                     # eax <- BA
    andb    $$0xf, %al                      # eax <- A
    shrl    $$4, rINST                      # rINST <- B
    GET_VREG %edx, rINSTq
    .if $is_object
    SET_VREG_OBJECT %edx, %rax              # fp[A] <- fp[B]
    .else
    SET_VREG %edx, %rax                     # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
