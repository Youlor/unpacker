    /*
     * Signed 64-bit integer multiply.
     *         a1   a0
     *   x     a3   a2
     *   -------------
     *       a2a1 a2a0
     *       a3a0
     *  a3a1 (<= unused)
     *  ---------------
     *         v1   v0
     */
    /* mul-long vAA, vBB, vCC */
    FETCH(a0, 1)                           #  a0 <- CCBB
    and       t0, a0, 255                  #  a2 <- BB
    srl       t1, a0, 8                    #  a3 <- CC
    EAS2(t0, rFP, t0)                      #  t0 <- &fp[BB]
    LOAD64(a0, a1, t0)                     #  a0/a1 <- vBB/vBB+1

    EAS2(t1, rFP, t1)                      #  t0 <- &fp[CC]
    LOAD64(a2, a3, t1)                     #  a2/a3 <- vCC/vCC+1

    mul       v1, a3, a0                   #  v1= a3a0
#ifdef MIPS32REVGE6
    mulu      v0, a2, a0                   #  v0= a2a0
    muhu      t1, a2, a0
#else
    multu     a2, a0
    mfhi      t1
    mflo      v0                           #  v0= a2a0
#endif
    mul       t0, a2, a1                   #  t0= a2a1
    addu      v1, v1, t1                   #  v1+= hi(a2a0)
    addu      v1, v1, t0                   #  v1= a3a0 + a2a1;

    GET_OPA(a0)                            #  a0 <- AA
    FETCH_ADVANCE_INST(2)                  #  advance rPC, load rINST
    b         .L${opcode}_finish
%break

.L${opcode}_finish:
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST
    SET_VREG64(v0, v1, a0)                 #  vAA::vAA+1 <- v0(low) :: v1(high)
    GOTO_OPCODE(t0)                        #  jump to next instruction
