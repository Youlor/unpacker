%default { "is_object":"0" }
    /* for: move-result, move-result-object */
    /* op vAA */
    movl    OFF_FP_RESULT_REGISTER(rFP), %eax    # get pointer to result JType.
    movl    (%eax), %eax                    # r0 <- result.i.
    .if $is_object
    SET_VREG_OBJECT %eax, rINST             # fp[A] <- fp[B]
    .else
    SET_VREG %eax, rINST                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 1
