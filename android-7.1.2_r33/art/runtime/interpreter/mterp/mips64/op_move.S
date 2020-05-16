%default { "is_object":"0" }
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    ext     a2, rINST, 8, 4             # a2 <- A
    ext     a3, rINST, 12, 4            # a3 <- B
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    GET_VREG a0, a3                     # a0 <- vB
    GET_INST_OPCODE v0                  # extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT a0, a2              # vA <- vB
    .else
    SET_VREG a0, a2                     # vA <- vB
    .endif
    GOTO_OPCODE v0                      # jump to next instruction
