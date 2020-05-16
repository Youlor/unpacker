%default { "is_object":"0" }
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    lhu     a3, 2(rPC)                  # a3 <- BBBB
    srl     a2, rINST, 8                # a2 <- AA
    FETCH_ADVANCE_INST 2                # advance rPC, load rINST
    GET_VREG a0, a3                     # a0 <- vBBBB
    GET_INST_OPCODE v0                  # extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT a0, a2              # vAA <- vBBBB
    .else
    SET_VREG a0, a2                     # vAA <- vBBBB
    .endif
    GOTO_OPCODE v0                      # jump to next instruction
