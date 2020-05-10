%default {}
    /*
     * Specialised 64-bit floating point operation.
     *
     * Note: The result will be returned in d2.
     *
     * For: long-to-double
     */
    mov     r3, rINST, lsr #12          @ r3<- B
    ubfx    r9, rINST, #8, #4           @ r9<- A
    VREG_INDEX_TO_ADDR r3, r3           @ r3<- &fp[B]
    VREG_INDEX_TO_ADDR r9, r9           @ r9<- &fp[A]
    vldr    d0, [r3]                    @ d0<- vAA
    FETCH_ADVANCE_INST 1                @ advance rPC, load rINST

    vcvt.f64.s32    d1, s1              @ d1<- (double)(vAAh)
    vcvt.f64.u32    d2, s0              @ d2<- (double)(vAAl)
    vldr            d3, constval$opcode
    vmla.f64        d2, d1, d3          @ d2<- vAAh*2^32 + vAAl

    GET_INST_OPCODE ip                  @ extract opcode from rINST
    vstr.64 d2, [r9]                    @ vAA<- d2
    GOTO_OPCODE ip                      @ jump to next instruction

    /* literal pool helper */
constval${opcode}:
    .8byte          0x41f0000000000000
