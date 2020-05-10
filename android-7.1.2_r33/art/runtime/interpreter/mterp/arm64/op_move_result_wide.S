    /* for: move-result-wide */
    /* op vAA */
    lsr     w2, wINST, #8               // r2<- AA
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    ldr     x0, [xFP, #OFF_FP_RESULT_REGISTER]  // get pointer to result JType.
    ldr     x0, [x0]                    // r0 <- result.i.
    GET_INST_OPCODE ip                  // extract opcode from wINST
    SET_VREG_WIDE x0, x2                // fp[AA]<- r0
    GOTO_OPCODE ip                      // jump to next instruction
