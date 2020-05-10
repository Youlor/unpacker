/*
 * ===========================================================================
 *  Common subroutines and data
 * ===========================================================================
 */

    .text
    .align  2

/*
 * We've detected a condition that will result in an exception, but the exception
 * has not yet been thrown.  Just bail out to the reference interpreter to deal with it.
 * TUNING: for consistency, we may want to just go ahead and handle these here.
 */
common_errDivideByZero:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogDivideByZeroException
#endif
    b MterpCommonFallback

common_errArrayIndex:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogArrayIndexException
#endif
    b MterpCommonFallback

common_errNegativeArraySize:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNegativeArraySizeException
#endif
    b MterpCommonFallback

common_errNoSuchMethod:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNoSuchMethodException
#endif
    b MterpCommonFallback

common_errNullObject:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogNullObjectException
#endif
    b MterpCommonFallback

common_exceptionThrown:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogExceptionThrownException
#endif
    b MterpCommonFallback

MterpSuspendFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    ldr  r2, [rSELF, #THREAD_FLAGS_OFFSET]
    bl MterpLogSuspendFallback
#endif
    b MterpCommonFallback

/*
 * If we're here, something is out of the ordinary.  If there is a pending
 * exception, handle it.  Otherwise, roll back and retry with the reference
 * interpreter.
 */
MterpPossibleException:
    ldr     r0, [rSELF, #THREAD_EXCEPTION_OFFSET]
    cmp     r0, #0                                  @ Exception pending?
    beq     MterpFallback                           @ If not, fall back to reference interpreter.
    /* intentional fallthrough - handle pending exception. */
/*
 * On return from a runtime helper routine, we've found a pending exception.
 * Can we handle it here - or need to bail out to caller?
 *
 */
MterpException:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    bl      MterpHandleException                    @ (self, shadow_frame)
    cmp     r0, #0
    beq     MterpExceptionReturn                    @ no local catch, back to caller.
    ldr     r0, [rFP, #OFF_FP_CODE_ITEM]
    ldr     r1, [rFP, #OFF_FP_DEX_PC]
    ldr     rIBASE, [rSELF, #THREAD_CURRENT_IBASE_OFFSET]
    add     rPC, r0, #CODEITEM_INSNS_OFFSET
    add     rPC, rPC, r1, lsl #1                    @ generate new dex_pc_ptr
    /* Do we need to switch interpreters? */
    bl      MterpShouldSwitchInterpreters
    cmp     r0, #0
    bne     MterpFallback
    /* resume execution at catch block */
    EXPORT_PC
    FETCH_INST
    GET_INST_OPCODE ip
    GOTO_OPCODE ip
    /* NOTE: no fallthrough */

/*
 * Common handling for branches with support for Jit profiling.
 * On entry:
 *    rINST          <= signed offset
 *    rPROFILE       <= signed hotness countdown (expanded to 32 bits)
 *    condition bits <= set to establish sign of offset (use "NoFlags" entry if not)
 *
 * We have quite a few different cases for branch profiling, OSR detection and
 * suspend check support here.
 *
 * Taken backward branches:
 *    If profiling active, do hotness countdown and report if we hit zero.
 *    If in osr check mode, see if our target is a compiled loop header entry and do OSR if so.
 *    Is there a pending suspend request?  If so, suspend.
 *
 * Taken forward branches and not-taken backward branches:
 *    If in osr check mode, see if our target is a compiled loop header entry and do OSR if so.
 *
 * Our most common case is expected to be a taken backward branch with active jit profiling,
 * but no full OSR check and no pending suspend request.
 * Next most common case is not-taken branch with no full OSR check.
 *
 */
MterpCommonTakenBranchNoFlags:
    cmp     rINST, #0
MterpCommonTakenBranch:
    bgt     .L_forward_branch           @ don't add forward branches to hotness
/*
 * We need to subtract 1 from positive values and we should not see 0 here,
 * so we may use the result of the comparison with -1.
 */
#if JIT_CHECK_OSR != -1
#  error "JIT_CHECK_OSR must be -1."
#endif
    cmp     rPROFILE, #JIT_CHECK_OSR
    beq     .L_osr_check
    subgts  rPROFILE, #1
    beq     .L_add_batch                @ counted down to zero - report
.L_resume_backward_branch:
    ldr     lr, [rSELF, #THREAD_FLAGS_OFFSET]
    REFRESH_IBASE
    add     r2, rINST, rINST            @ r2<- byte offset
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
    ands    lr, #(THREAD_SUSPEND_REQUEST | THREAD_CHECKPOINT_REQUEST)
    bne     .L_suspend_request_pending
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

.L_suspend_request_pending:
    EXPORT_PC
    mov     r0, rSELF
    bl      MterpSuspendCheck           @ (self)
    cmp     r0, #0
    bne     MterpFallback
    REFRESH_IBASE                       @ might have changed during suspend
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

.L_no_count_backwards:
    cmp     rPROFILE, #JIT_CHECK_OSR    @ possible OSR re-entry?
    bne     .L_resume_backward_branch
.L_osr_check:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement
    b       .L_resume_backward_branch

.L_forward_branch:
    cmp     rPROFILE, #JIT_CHECK_OSR @ possible OSR re-entry?
    beq     .L_check_osr_forward
.L_resume_forward_branch:
    add     r2, rINST, rINST            @ r2<- byte offset
    FETCH_ADVANCE_INST_RB r2            @ update rPC, load rINST
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

.L_check_osr_forward:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rINST
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement
    b       .L_resume_forward_branch

.L_add_batch:
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    strh    rPROFILE, [r1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    ldr     r0, [rFP, #OFF_FP_METHOD]
    mov     r2, rSELF
    bl      MterpAddHotnessBatch        @ (method, shadow_frame, self)
    mov     rPROFILE, r0                @ restore new hotness countdown to rPROFILE
    b       .L_no_count_backwards

/*
 * Entered from the conditional branch handlers when OSR check request active on
 * not-taken path.  All Dalvik not-taken conditional branch offsets are 2.
 */
.L_check_not_taken_osr:
    mov     r0, rSELF
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, #2
    EXPORT_PC
    bl      MterpMaybeDoOnStackReplacement  @ (self, shadow_frame, offset)
    cmp     r0, #0
    bne     MterpOnStackReplacement
    FETCH_ADVANCE_INST 2
    GET_INST_OPCODE ip                  @ extract opcode from rINST
    GOTO_OPCODE ip                      @ jump to next instruction

/*
 * On-stack replacement has happened, and now we've returned from the compiled method.
 */
MterpOnStackReplacement:
#if MTERP_LOGGING
    mov r0, rSELF
    add r1, rFP, #OFF_FP_SHADOWFRAME
    mov r2, rINST
    bl MterpLogOSR
#endif
    mov r0, #1                          @ Signal normal return
    b MterpDone

/*
 * Bail out to reference interpreter.
 */
MterpFallback:
    EXPORT_PC
#if MTERP_LOGGING
    mov  r0, rSELF
    add  r1, rFP, #OFF_FP_SHADOWFRAME
    bl MterpLogFallback
#endif
MterpCommonFallback:
    mov     r0, #0                                  @ signal retry with reference interpreter.
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
    mov     r0, #1                                  @ signal return to caller.
    b MterpDone
MterpReturn:
    ldr     r2, [rFP, #OFF_FP_RESULT_REGISTER]
    str     r0, [r2]
    str     r1, [r2, #4]
    mov     r0, #1                                  @ signal return to caller.
MterpDone:
/*
 * At this point, we expect rPROFILE to be non-zero.  If negative, hotness is disabled or we're
 * checking for OSR.  If greater than zero, we might have unreported hotness to register
 * (the difference between the ending rPROFILE and the cached hotness counter).  rPROFILE
 * should only reach zero immediately after a hotness decrement, and is then reset to either
 * a negative special state or the new non-zero countdown value.
 */
    cmp     rPROFILE, #0
    bgt     MterpProfileActive                      @ if > 0, we may have some counts to report.
    ldmfd   sp!, {r3-r10,fp,pc}                     @ restore 10 regs and return

MterpProfileActive:
    mov     rINST, r0                               @ stash return value
    /* Report cached hotness counts */
    ldr     r0, [rFP, #OFF_FP_METHOD]
    add     r1, rFP, #OFF_FP_SHADOWFRAME
    mov     r2, rSELF
    strh    rPROFILE, [r1, #SHADOWFRAME_HOTNESS_COUNTDOWN_OFFSET]
    bl      MterpAddHotnessBatch                    @ (method, shadow_frame, self)
    mov     r0, rINST                               @ restore return value
    ldmfd   sp!, {r3-r10,fp,pc}                     @ restore 10 regs and return

    .fnend
    .size   ExecuteMterpImpl, .-ExecuteMterpImpl

