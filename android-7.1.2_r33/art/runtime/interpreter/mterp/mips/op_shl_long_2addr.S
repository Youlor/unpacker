    /*
     * Long integer shift, 2addr version.  vA is 64-bit value/result, vB is
     * 32-bit shift distance.
     */
    /* shl-long/2addr vA, vB */
    GET_OPA4(rOBJ)                         #  rOBJ <- A+
    GET_OPB(a3)                            #  a3 <- B
    GET_VREG(a2, a3)                       #  a2 <- vB
    EAS2(t2, rFP, rOBJ)                    #  t2 <- &fp[A]
    LOAD64(a0, a1, t2)                     #  a0/a1 <- vAA/vAA+1

    FETCH_ADVANCE_INST(1)                  #  advance rPC, load rINST
    GET_INST_OPCODE(t0)                    #  extract opcode from rINST

    andi    v1, a2, 0x20                   #  shift< shift & 0x20
    sll     v0, a0, a2                     #  rlo<- alo << (shift&31)
    bnez    v1, .L${opcode}_finish
    not     v1, a2                         #  rhi<- 31-shift  (shift is 5b)
    srl     a0, 1
    srl     a0, v1                         #  alo<- alo >> (32-(shift&31))
    sll     v1, a1, a2                     #  rhi<- ahi << (shift&31)
    or      v1, a0                         #  rhi<- rhi | alo
    SET_VREG64_GOTO(v0, v1, rOBJ, t0)      #  vAA/vAA+1 <- a0/a1
%break

.L${opcode}_finish:
    SET_VREG64_GOTO(zero, v0, rOBJ, t0)    #  vAA/vAA+1 <- rlo/rhi
