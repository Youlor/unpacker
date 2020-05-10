%default { "is_object":"0" }
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    lhu     a3, 4(rPC)                  # a3 <- BBBB
    lhu     a2, 2(rPC)                  # a2 <- AAAA
    FETCH_ADVANCE_INST 3                # advance rPC, load rINST
    GET_VREG a0, a3                     # a0 <- vBBBB
    GET_INST_OPCODE v0                  # extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT a0, a2              # vAAAA <- vBBBB
    .else
    SET_VREG a0, a2                     # vAAAA <- vBBBB
    .endif
    GOTO_OPCODE v0                      # jump to next instruction
