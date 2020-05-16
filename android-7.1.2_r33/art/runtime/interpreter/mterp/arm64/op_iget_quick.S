%default { "load":"ldr", "extend":"" }
    /* For: iget-quick, iget-boolean-quick, iget-byte-quick, iget-char-quick, iget-short-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     x3, #0                      // check object for null
    beq     common_errNullObject        // object was null
    $load   w0, [x3, x1]                // w0<- obj.field
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    $extend
    SET_VREG w0, w2                     // fp[A]<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
