%default { "is_object":"0" }
    /* for: move-result, move-result-object */
    /* op vAA */
    srl     a2, rINST, 8                # a2 <- AA
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    ld      a0, OFF_FP_RESULT_REGISTER(rFP)  # get pointer to result JType
    lw      a0, 0(a0)                   # a0 <- result.i
    GET_INST_OPCODE v0                  # extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT a0, a2              # vAA <- result
    .else
    SET_VREG a0, a2                     # vAA <- result
    .endif
    GOTO_OPCODE v0                      # jump to next instruction
