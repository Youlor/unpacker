%default {"wide":"", "r1":"s1", "r2":"s2", "default_val":"-1","cond":"le"}
    /*
     * Compare two floating-point values.  Puts 0, 1, or -1 into the
     * destination register based on the results of the comparison.
     */
    /* op vAA, vBB, vCC */
    FETCH w0, 1                         // w0<- CCBB
    lsr     w4, wINST, #8               // w4<- AA
    and     w2, w0, #255                // w2<- BB
    lsr     w3, w0, #8                  // w3<- CC
    GET_VREG$wide $r1, w2
    GET_VREG$wide $r2, w3
    mov     w0, #$default_val
    fcmp $r1, $r2
    csneg w0, w0, w0, $cond
    csel w0, wzr, w0, eq
    FETCH_ADVANCE_INST 2                // advance rPC, load rINST
    GET_INST_OPCODE ip                  // extract opcode from rINST
    SET_VREG w0, w4                     // vAA<- w0
    GOTO_OPCODE ip                      // jump to next instruction
