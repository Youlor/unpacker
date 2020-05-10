%default { "is_object":"0" }
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    movzx   rINSTbl, %eax                   # eax <- AA
    movw    2(rPC), rINSTw                  # rINSTw <- BBBB
    GET_VREG rINST, rINST                   # rINST <- fp[BBBB]
    .if $is_object
    SET_VREG_OBJECT rINST, %eax             # fp[A] <- fp[B]
    .else
    SET_VREG rINST, %eax                    # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
