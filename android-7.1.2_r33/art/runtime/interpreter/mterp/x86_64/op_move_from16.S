%default { "is_object":"0" }
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    movzwq  2(rPC), %rax                    # eax <- BBBB
    GET_VREG %edx, %rax                     # edx <- fp[BBBB]
    .if $is_object
    SET_VREG_OBJECT %edx, rINSTq            # fp[A] <- fp[B]
    .else
    SET_VREG %edx, rINSTq                   # fp[A] <- fp[B]
    .endif
    ADVANCE_PC_FETCH_AND_GOTO_NEXT 2
