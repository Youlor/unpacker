/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */

    .extern MterpLogDivideByZeroException
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    jal     MterpLogDivideByZeroException
#endif
    b       MterpCommonFallback

    .extern MterpLogArrayIndexException
common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    jal     MterpLogArrayIndexException
#endif
    b       MterpCommonFallback

    .extern MterpLogNullObjectException
common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    jal     MterpLogNullObjectException
#endif
    b       MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    ld      a0, THREAD_EXCEPTION_OFFSET(rSELF)
    beqzc   a0, MterpFallback                       # If not, fall back to reference interpreter.
    /* intentional fallthrough - handle pending exception. */
/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
    .extern MterpHandleException
    .extern MterpShouldSwitchInterpreters
MterpException:
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    jal     MterpHandleException                    # (self, shadow_frame)
    beqzc   v0, MterpExceptionReturn                # no local catch, back to caller.
    ld      a0, OFF_FP_CODE_ITEM(rFP)
    lwu     a1, OFF_FP_DEX_PC(rFP)
    REFRESH_IBASE
    daddu   rPC, a0, CODEITEM_INSNS_OFFSET
    dlsa    rPC, a1, rPC, 1                         # generate new dex_pc_ptr
    /* Do we need to switch interpreters? */
    jal     MterpShouldSwitchInterpreters
    bnezc   v0, MterpFallback
    /* resume execution at catch block */
    EXPORT_PC
    FETCH_INST
    GET_INST_OPCODE v0
    GOTO_OPCODE v0
    /* NOTE: no fallthrough */

/*
 * Check for suspend check request.  Assumes rINST already loaded, rPC advanced and
 * still needs to get the opcode and branch to it, and flags are in ra.
 */
    .extern MterpSuspendCheck
MterpCheckSuspendAndContinue:
    REFRESH_IBASE
    and     ra, ra, (THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    bnez    ra, check1
    GET_INST_OPCODE v0                              # extract opcode from rINST
    GOTO_OPCODE v0                                  # jump to next instruction
check1:
    EXPORT_PC
    move    a0, rSELF
    jal     MterpSuspendCheck                       # (self)
    bnezc   v0, MterpFallback                       # Something in the environment changed, switch interpreters
    GET_INST_OPCODE v0                              # extract opcode from rINST
    GOTO_OPCODE v0                                  # jump to next instruction

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    move    a2, rINST                               # rINST contains offset
    jal     MterpLogOSR
#endif
    li      v0, 1                                   # Signal normal return
    b       MterpDone

/*
 * Bail out to reference interpreter.
 */
    .extern MterpLogFallback
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    move    a0, rSELF
    daddu   a1, rFP, OFF_FP_SHADOWFRAME
    jal     MterpLogFallback
#endif
MterpCommonFallback:
    li      v0, 0                                   # signal retry with reference interpreter.
    b       MterpDone

/*
 * We pushed some registers on the stack in ExecuteMterpImpl, then saved
 * SP and RA.  Here we restore SP, restore the registers, and then restore
 * RA to PC.
 *
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    li      v0, 1                                   # signal return to caller.
    b       MterpDone
/*
 * Returned value is expected in a0 and if it's not 64-bit, the 32 most
 * significant bits of a0 must be 0.
 */
MterpReturn:
    ld      a2, OFF_FP_RESULT_REGISTER(rFP)
    sd      a0, 0(a2)
    li      v0, 1                                   # signal return to caller.
MterpDone:
    ld      s5, STACK_OFFSET_S5(sp)
    .cfi_restore 21
    ld      s4, STACK_OFFSET_S4(sp)
    .cfi_restore 20
    ld      s3, STACK_OFFSET_S3(sp)
    .cfi_restore 19
    ld      s2, STACK_OFFSET_S2(sp)
    .cfi_restore 18
    ld      s1, STACK_OFFSET_S1(sp)
    .cfi_restore 17
    ld      s0, STACK_OFFSET_S0(sp)
    .cfi_restore 16

    ld      ra, STACK_OFFSET_RA(sp)
    .cfi_restore 31

    ld      t8, STACK_OFFSET_GP(sp)
    .cpreturn
    .cfi_restore 28

    .set    noreorder
    jr      ra
    daddu   sp, sp, STACK_SIZE
    .cfi_adjust_cfa_offset -STACK_SIZE

    .cfi_endproc
    .size ExecuteMterpImpl, .-ExecuteMterpImpl
