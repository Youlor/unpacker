    /*
     * Unconditional branch, 8-bit offset.
     *
     * The branch distance is a signed code-unit offset, which we need to
     * double to get a byte offset.
     */
    /* goto +AA */
    sbfx    rINST, rINST, #8, #8           @ rINST<- ssssssAA (sign-extended)
    b       MterpCommonTakenBranchNoFlags
