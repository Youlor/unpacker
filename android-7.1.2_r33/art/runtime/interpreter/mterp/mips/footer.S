/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */

    .text
    .align 2

/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
common_errDivideByZero:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpLogDivideByZeroException)
#endif
    b MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpLogArrayIndexException)
#endif
    b MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpLogNegativeArraySizeException)
#endif
    b MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpLogNoSuchMethodException)
#endif
    b MterpCommonFallback

common_errNullObject:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpLogNullObjectException)
#endif
    b MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpLogExceptionThrownException)
#endif
    b MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    lw    a2, THREAD_FLAGS_OFFSET(rSELF)
    JAL(MterpLogSuspendFallback)
#endif
    b MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    lw      a0, THREAD_EXCEPTION_OFFSET(rSELF)
    beqz    a0, MterpFallback          # If exception, fall back to reference interpreter.
    /* intentional fallthrough - handle pending exception. */
/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    move    a0, rSELF
    addu    a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpHandleException)                    # (self, shadow_frame)
    beqz    v0, MterpExceptionReturn             # no local catch, back to caller.
    lw      a0, OFF_FP_CODE_ITEM(rFP)
    lw      a1, OFF_FP_DEX_PC(rFP)
    lw      rIBASE, THREAD_CURRENT_IBASE_OFFSET(rSELF)
    addu    rPC, a0, CODEITEM_INSNS_OFFSET
    sll     a1, a1, 1
    addu    rPC, rPC, a1                         # generate new dex_pc_ptr
    /* Do we need to switch interpreters? */
    JAL(MterpShouldSwitchInterpreters)
    bnez    v0, MterpFallback
    /* resume execution at catch block */
    EXPORT_PC()
    FETCH_INST()
    GET_INST_OPCODE(t0)
    GOTO_OPCODE(t0)
    /* NOTE: no fallthrough */

/*
 * Check for suspend check request.  Assumes rINST already loaded, rPC advanced and
 * still needs to get the opcode and branch to it, and flags are in lr.
 */
MterpCheckSuspendAndContinue:
    lw      rIBASE, THREAD_CURRENT_IBASE_OFFSET(rSELF)  # refresh rIBASE
    and     ra, (THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    bnez    ra, 1f
    GET_INST_OPCODE(t0)                 # extract opcode from rINST
    GOTO_OPCODE(t0)                     # jump to next instruction
1:
    EXPORT_PC()
    move    a0, rSELF
    JAL(MterpSuspendCheck)              # (self)
    bnez    v0, MterpFallback
    GET_INST_OPCODE(t0)                 # extract opcode from rINST
    GOTO_OPCODE(t0)                     # jump to next instruction

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    move    a0, rSELF
    addu    a1, rFP, OFF_FP_SHADOWFRAME
    move    a2, rINST
    JAL(MterpLogOSR)
#endif
    li      v0, 1                       # Signal normal return
    b       MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC()
#if MTERP_LOGGING
    move  a0, rSELF
    addu  a1, rFP, OFF_FP_SHADOWFRAME
    JAL(MterpLogFallback)
#endif
MterpCommonFallback:
    move    v0, zero                    # signal retry with reference interpreter.
    b       MterpDone
/*
 * We pushed some registers on the stack in ExecuteMterpImpl, then saved
 * SP and LR.  Here we restore SP, restore the registers, and then restore
 * LR to PC.
 *
 * On entry:
 *  uint32_t* rFP  (should still be live, pointer to base of vregs)
 */
MterpExceptionReturn:
    li      v0, 1                       # signal return to caller.
    b       MterpDone
MterpReturn:
    lw      a2, OFF_FP_RESULT_REGISTER(rFP)
    sw      v0, 0(a2)
    sw      v1, 4(a2)
    li      v0, 1                       # signal return to caller.
MterpDone:
/* Restore from the stack and return. Frame size = STACK_SIZE */
    STACK_LOAD_FULL()
    jalr    zero, ra

    .end ExecuteMterpImpl
