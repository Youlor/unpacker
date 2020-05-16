%default { "store":"str" }
    /* For: iput-quick, iput-object-quick */
    /* op vA, vB, offset//CCCC */
    lsr     w2, wINST, #12              // w2<- B
    FETCH w1, 1                         // w1<- field byte offset
    GET_VREG w3, w2                     // w3<- fp[B], the object pointer
    ubfx    w2, wINST, #8, #4           // w2<- A
    cmp     w3, #0                      // check object for null
    cbz     w3, common_errNullObject    // object was null
    GET_VREG w0, w2                     // w0<- fp[A]
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    $store     w0, [x3, x1]             // obj.field<- w0
    GET_INST_OPCODE ip                  // extract opcode from rINST
    GOTO_OPCODE ip                      // jump to next instruction
