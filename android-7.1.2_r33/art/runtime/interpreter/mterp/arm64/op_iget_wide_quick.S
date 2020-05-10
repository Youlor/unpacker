    /* iget-wide-quick vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w4, 1                         // w4<- field byte offset
    GET_VREG w3, w2                     // w3<- object we're operating on
    ubfx    w2, wINST, #8, #4           // w2<- A
    cbz     w3, common_errNullObject        // object was null
    add     x4, x3, x4                  // create direct pointer
    ldr     x0, [x4]
    FETCH_ADVANCE_INST 2                // advance rPC, load wINST
    SET_VREG_WIDE x0, w2
    GET_INST_OPCODE ip                  // extract opcode from wINST
    GOTO_OPCODE ip                      // jump to next instruction
