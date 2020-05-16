%default { "is_object":"0" }
    /* for: move-result, move-result-object */
    /* op vAA */
    lsr     w2, wINST, #8               // r2<- AA
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    ldr     x0, [xFP, #OFF_FP_RESULT_REGISTER]  // get pointer to result JType.
    ldr     w0, [x0]                    // r0 <- result.i.
    GET_INST_OPCODE ip                  // extract opcode from wINST
    .if $is_object
    SET_VREG_OBJECT w0, w2, w1          // fp[AA]<- r0
    .else
    SET_VREG w0, w2                     // fp[AA]<- r0
    .endif
    GOTO_OPCODE ip                      // jump to next instruction
