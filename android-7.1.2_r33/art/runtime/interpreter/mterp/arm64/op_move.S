%default { "is_object":"0" }
    /* for move, move-object, long-to-int */
    /* op vA, vB */
    lsr     w1, wINST, #12              // x1<- B from 15:12
    ubfx    w0, wINST, #8, #4           // x0<- A from 11:8
    FETCH_ADVANCE_INST 1                // advance rPC, load wINST
    GET_VREG w2, w1                     // x2<- fp[B]
    GET_INST_OPCODE ip                  // ip<- opcode from wINST
    .if $is_object
    SET_VREG_OBJECT w2, w0              // fp[A]<- x2
    .else
    SET_VREG w2, w0                     // fp[A]<- x2
    .endif
    GOTO_OPCODE ip                      // execute next instruction
