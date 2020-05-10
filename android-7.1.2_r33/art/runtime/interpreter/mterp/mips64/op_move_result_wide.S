    /* for: move-result-wide */
    /* op vAA */
    srl     a2, rINST, 8                # a2 <- AA
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    ld      a0, OFF_FP_RESULT_REGISTER(rFP)  # get pointer to result JType
    ld      a0, 0(a0)                   # a0 <- result.j
    GET_INST_OPCODE v0                  # extract opcode from rINST
    SET_VREG_WIDE a0, a2                # vAA <- result
    GOTO_OPCODE v0                      # jump to next instruction
