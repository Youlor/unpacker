%default { "is_object":"0" }
    /* for: move/from16, move-object/from16 */
    /* op vAA, vBBBB */
    FETCH(a1, 1)                           #  a1 <- BBBB
    GET_OPA(a0)                            #  a0 <- AA
    FETCH_ADVANCE_INST(2)                  #  advance rPC, load rINST
    GET_VREG(a2, a1)                       #  a2 <- fp[BBBB]
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    .if $is_object
    SET_VREG_OBJECT(a2, a0)                #  fp[AA] <- a2
    .else
    SET_VREG(a2, a0)                       #  fp[AA] <- a2
    .endif
    GOTO_OPCODE(t0)                        #  jump to next instruction
