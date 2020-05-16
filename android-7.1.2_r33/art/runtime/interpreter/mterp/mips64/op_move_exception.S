    /* move-exception vAA */
    srl     a2, rINST, 8                # a2 <- AA
    ld      a0, THREAD_EXCEPTION_OFFSET(rSELF)  # load exception obj
    FETCH_ADVANCE_INST 1                # advance rPC, load rINST
    SET_VREG_OBJECT a0, a2              # vAA <- exception obj
    GET_INST_OPCODE v0                  # extract opcode from rINST
    sd      zero, THREAD_EXCEPTION_OFFSET(rSELF)  # clear exception
    GOTO_OPCODE v0                      # jump to next instruction
