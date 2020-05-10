    /*
     * See op_mul_long.S for more details
     */
    /* mul-long/2addr vA, vB */
    GET_OPA4(rOBJ)                         #  rOBJ <- A+

    EAS2(t0, rFP, rOBJ)                    #  t0 <- &fp[A]
    LOAD64(a0, a1, t0)                     #  vAA.low / high

    GET_OPB(t1)                            #  t1 <- B
    EAS2(t1, rFP, t1)                      #  t1 <- &fp[B]
    LOAD64(a2, a3, t1)                     #  vBB.low / high

    mul       v1, a3, a0                   #  v1= a3a0
#ifdef MIPS32REVGE6
    mulu      v0, a2, a0                   #  v0= a2a0
    muhu      t1, a2, a0
#else
    multu     a2, a0
    mfhi      t1
    mflo      v0                           #  v0= a2a0
 #endif
    mul       t2, a2, a1                   #  t2= a2a1
    addu      v1, v1, t1                   #  v1= a3a0 + hi(a2a0)
    addu      v1, v1, t2                   #  v1= v1 + a2a1;

    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    GET_INST_OPCODE(t1)                    #  extract opcode from rINST
    # vAA <- v0 (low)
    SET_VREG64(v0, v1, rOBJ)               #  vAA+1 <- v1 (high)
    GOTO_OPCODE(t1)                        #  jump to next instruction
