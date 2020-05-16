    /*
     * Unconditional branch, 32-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     *
     * Unlike most opcodes, this one is allowed to branch to itself, so
     * our "backward branch" test must be "<=0" instead of "<0".  Because
     * we need the V bit set, we'll use an adds to convert from Dalvik
     * offset to byte offset.
     */
    /* goto/32 +AAAAAAAA */
    FETCH r0, 1                         @ r0<- aaaa (lo)
    FETCH r3, 2                         @ r1<- AAAA (hi)
    orrs    rINST, r0, r3, lsl #16      @ rINST<- AAAAaaaa
    b       MterpCommonTakenBranch
