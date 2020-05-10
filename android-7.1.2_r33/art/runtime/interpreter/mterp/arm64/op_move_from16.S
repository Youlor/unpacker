%default { "is_object":"0" }
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    FETCH w1, 1                         // r1<- BBBB
    lsr     w0, wINST, #8               // r0<- AA
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    GET_VREG w2, w1                     // r2<- fp[BBBB]
    GET_INST_OPCODE ip                  // extract opcode from wINST
    .if $is_object
    SET_VREG_OBJECT w2, w0              // fp[AA]<- r2
    .else
    SET_VREG w2, w0                     // fp[AA]<- r2
    .endif
    GOTO_OPCODE ip                      // jump to next instruction
