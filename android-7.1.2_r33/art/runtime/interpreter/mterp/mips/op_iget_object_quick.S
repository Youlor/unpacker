    /* For: iget-object-quick */
    /* op vA, vB, offset@CCCC */
    GET_OPB(a2)                            #  a2 <- B
    FETCH(a1, 1)                           #  a1 <- field byte offset
    EXPORT_PC()
    GET_VREG(a0, a2)                       #  a0 <- object we're operating on
    JAL(artIGetObjectFromMterp)            #  v0 <- GetObj(obj, offset)
    lw   a3, THREAD_EXCEPTION_OFFSET(rSELF)
    GET_OPA4(a2)                           #  a2<- A+
    PREFETCH_INST(2)                       #  load rINST
    bnez a3, MterpPossibleException        #  bail out
    SET_VREG_OBJECT(v0, a2)                #  fp[A] <- v0
    ADVANCE(2)                             #  advance rPC
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    GOTO_OPCODE(t0)                        #  jump to next instruction
