%default { "is_object":"0" }
    /* for: move/16, move-object/16 */
    /* op vAAAA, vBBBB */
    FETCH w1, 2                         // w1<- BBBB
    FETCH w0, 1                         // w0<- AAAA
    FETCH_ADVANCE_INST 3                // advance xPC, load xINST
    GET_VREG w2, w1                     // w2<- fp[BBBB]
    GET_INST_OPCODE ip                  // extract opcode from xINST
    .if $is_object
    SET_VREG_OBJECT w2, w0              // fp[AAAA]<- w2
    .else
    SET_VREG w2, w0                     // fp[AAAA]<- w2
    .endif
    GOTO_OPCODE ip                      // jump to next instruction
