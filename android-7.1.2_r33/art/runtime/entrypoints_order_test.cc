/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <memory>
#include <setjmp.h>

#include "base/macros.h"
#include "common_runtime_test.h"
#include "thread.h"

// This test checks the offsets of values in the thread TLS and entrypoint structures. A failure
// of this test means that offsets have changed from the last update of the test. This indicates
// that an oat version bump may be in order, and some defines should be carefully checked (or their
// corresponding tests run).

namespace art {

// OFFSETOF_MEMBER uses reinterpret_cast. This means it is not a constexpr. So we cannot use
// compile-time assertions. Once we find another way, adjust the define accordingly.
#define CHECKED(expr, name) \
  EXPECT_TRUE(expr) << #name

// Macro to check whether two fields have an expected difference in offsets.  The error is named
// name.
#define EXPECT_OFFSET_DIFF(first_type, first_field, second_type, second_field, diff, name) \
  CHECKED(OFFSETOF_MEMBER(second_type, second_field) \
          - OFFSETOF_MEMBER(first_type, first_field) == diff, name)

// Helper macro for when the fields are from the same type.
#define EXPECT_OFFSET_DIFFNP(type, first_field, second_field, diff) \
  EXPECT_OFFSET_DIFF(type, first_field, type, second_field, diff, \
                     type ## _ ## first_field ## _ ## second_field)

// Helper macro for when the fields are from the same type and in the same member of said type.
#define EXPECT_OFFSET_DIFFP(type, prefix, first_field, second_field, diff) \
  EXPECT_OFFSET_DIFF(type, prefix . first_field, type, prefix . second_field, diff, \
                     type ## _ ## prefix ## _ ## first_field ## _ ## second_field)

// Macro to check whether two fields have at least an expected difference in offsets.  The error is
// named name.
#define EXPECT_OFFSET_DIFF_GT(first_type, first_field, second_type, second_field, diff, name) \
  CHECKED(OFFSETOF_MEMBER(second_type, second_field) \
          - OFFSETOF_MEMBER(first_type, first_field) >= diff, name)

// Helper macro for when the fields are from the same type.
#define EXPECT_OFFSET_DIFF_GT3(type, first_field, second_field, diff, name) \
  EXPECT_OFFSET_DIFF_GT(type, first_field, type, second_field, diff, name)

class EntrypointsOrderTest : public CommonRuntimeTest {
 protected:
  void CheckThreadOffsets() {
    CHECKED(OFFSETOF_MEMBER(Thread, tls32_.state_and_flags) == 0, thread_flags_at_zero);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, state_and_flags, suspend_count, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, suspend_count, debug_suspend_count, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, debug_suspend_count, thin_lock_thread_id, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, thin_lock_thread_id, tid, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, tid, daemon, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, daemon, throwing_OutOfMemoryError, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, throwing_OutOfMemoryError, no_thread_suspension, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, no_thread_suspension, thread_exit_check_count, 4);
    EXPECT_OFFSET_DIFFP(Thread, tls32_, thread_exit_check_count, handling_signal_, 4);

    // TODO: Better connection. Take alignment into account.
    EXPECT_OFFSET_DIFF_GT3(Thread, tls32_.thread_exit_check_count, tls64_.trace_clock_base, 4,
                           thread_tls32_to_tls64);

    EXPECT_OFFSET_DIFFP(Thread, tls64_, trace_clock_base, stats, 8);

    // TODO: Better connection. Take alignment into account.
    EXPECT_OFFSET_DIFF_GT3(Thread, tls64_.stats, tlsPtr_.card_table, 8, thread_tls64_to_tlsptr);

    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, card_table, exception, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, exception, stack_end, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, stack_end, managed_stack, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, managed_stack, suspend_trigger, sizeof(ManagedStack));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, suspend_trigger, jni_env, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, jni_env, tmp_jni_env, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, tmp_jni_env, self, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, self, opeer, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, opeer, jpeer, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, jpeer, stack_begin, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, stack_begin, stack_size, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, stack_size, stack_trace_sample, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, stack_trace_sample, wait_next, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, wait_next, monitor_enter_object, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, monitor_enter_object, top_handle_scope, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, top_handle_scope, class_loader_override, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, class_loader_override, long_jump_context, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, long_jump_context, instrumentation_stack, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, instrumentation_stack, debug_invoke_req, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, debug_invoke_req, single_step_control, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, single_step_control, stacked_shadow_frame_record,
                        sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, stacked_shadow_frame_record,
                        deoptimization_context_stack, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, deoptimization_context_stack,
                        frame_id_to_shadow_frame, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, frame_id_to_shadow_frame, name, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, name, pthread_self, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, pthread_self, last_no_thread_suspension_cause,
                        sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, last_no_thread_suspension_cause, checkpoint_functions,
                        sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, checkpoint_functions, jni_entrypoints,
                        sizeof(void*) * 6);

    // Skip across the entrypoints structures.

    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, thread_local_objects, thread_local_start, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, thread_local_start, thread_local_pos, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, thread_local_pos, thread_local_end, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, thread_local_end, mterp_current_ibase, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, mterp_current_ibase, mterp_default_ibase, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, mterp_default_ibase, mterp_alt_ibase, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, mterp_alt_ibase, rosalloc_runs, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, rosalloc_runs, thread_local_alloc_stack_top,
                        sizeof(void*) * kNumRosAllocThreadLocalSizeBracketsInThread);
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, thread_local_alloc_stack_top, thread_local_alloc_stack_end,
                        sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, thread_local_alloc_stack_end, held_mutexes, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, held_mutexes, nested_signal_state,
                        sizeof(void*) * kLockLevelCount);
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, nested_signal_state, flip_function, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, flip_function, method_verifier, sizeof(void*));
    EXPECT_OFFSET_DIFFP(Thread, tlsPtr_, method_verifier, thread_local_mark_stack, sizeof(void*));
    EXPECT_OFFSET_DIFF(Thread, tlsPtr_.thread_local_mark_stack, Thread, wait_mutex_, sizeof(void*),
                       thread_tlsptr_end);
  }

  void CheckJniEntryPoints() {
    CHECKED(OFFSETOF_MEMBER(JniEntryPoints, pDlsymLookup) == 0,
            JniEntryPoints_start_with_dlsymlookup);
    CHECKED(OFFSETOF_MEMBER(JniEntryPoints, pDlsymLookup)
            + sizeof(void*) == sizeof(JniEntryPoints), JniEntryPoints_all);
  }

  void CheckQuickEntryPoints() {
    CHECKED(OFFSETOF_MEMBER(QuickEntryPoints, pAllocArray) == 0,
                QuickEntryPoints_start_with_allocarray);
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocArray, pAllocArrayResolved, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocArrayResolved, pAllocArrayWithAccessCheck,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocArrayWithAccessCheck, pAllocObject, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocObject, pAllocObjectResolved, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocObjectResolved, pAllocObjectInitialized,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocObjectInitialized, pAllocObjectWithAccessCheck,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocObjectWithAccessCheck, pCheckAndAllocArray,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCheckAndAllocArray, pCheckAndAllocArrayWithAccessCheck,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCheckAndAllocArrayWithAccessCheck,
                         pAllocStringFromBytes, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocStringFromBytes, pAllocStringFromChars,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocStringFromChars, pAllocStringFromString,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAllocStringFromString, pInstanceofNonTrivial,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInstanceofNonTrivial, pCheckCast, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCheckCast, pInitializeStaticStorage, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInitializeStaticStorage, pInitializeTypeAndVerifyAccess,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInitializeTypeAndVerifyAccess, pInitializeType,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInitializeType, pResolveString, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pResolveString, pSet8Instance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet8Instance, pSet8Static, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet8Static, pSet16Instance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet16Instance, pSet16Static, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet16Static, pSet32Instance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet32Instance, pSet32Static, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet32Static, pSet64Instance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet64Instance, pSet64Static, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSet64Static, pSetObjInstance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSetObjInstance, pSetObjStatic, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSetObjStatic, pGetByteInstance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetByteInstance, pGetBooleanInstance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetBooleanInstance, pGetByteStatic, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetByteStatic, pGetBooleanStatic, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetBooleanStatic, pGetShortInstance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetShortInstance, pGetCharInstance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetCharInstance, pGetShortStatic, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetShortStatic, pGetCharStatic, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetCharStatic, pGet32Instance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGet32Instance, pGet32Static, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGet32Static, pGet64Instance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGet64Instance, pGet64Static, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGet64Static, pGetObjInstance, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetObjInstance, pGetObjStatic, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pGetObjStatic, pAputObjectWithNullAndBoundCheck,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAputObjectWithNullAndBoundCheck,
                         pAputObjectWithBoundCheck, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAputObjectWithBoundCheck, pAputObject, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAputObject, pHandleFillArrayData, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pHandleFillArrayData, pJniMethodStart, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pJniMethodStart, pJniMethodStartSynchronized,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pJniMethodStartSynchronized, pJniMethodEnd,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pJniMethodEnd, pJniMethodEndSynchronized, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pJniMethodEndSynchronized, pJniMethodEndWithReference,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pJniMethodEndWithReference,
                         pJniMethodEndWithReferenceSynchronized, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pJniMethodEndWithReferenceSynchronized,
                         pQuickGenericJniTrampoline, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pQuickGenericJniTrampoline, pLockObject, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pLockObject, pUnlockObject, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pUnlockObject, pCmpgDouble, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCmpgDouble, pCmpgFloat, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCmpgFloat, pCmplDouble, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCmplDouble, pCmplFloat, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCmplFloat, pCos, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCos, pSin, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSin, pAcos, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAcos, pAsin, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAsin, pAtan, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAtan, pAtan2, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pAtan2, pCbrt, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCbrt, pCosh, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pCosh, pExp, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pExp, pExpm1, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pExpm1, pHypot, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pHypot, pLog, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pLog, pLog10, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pLog10, pNextAfter, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNextAfter, pSinh, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pSinh, pTan, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pTan, pTanh, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pTanh, pFmod, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pFmod, pL2d, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pL2d, pFmodf, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pFmodf, pL2f, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pL2f, pD2iz, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pD2iz, pF2iz, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pF2iz, pIdivmod, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pIdivmod, pD2l, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pD2l, pF2l, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pF2l, pLdiv, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pLdiv, pLmod, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pLmod, pLmul, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pLmul, pShlLong, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pShlLong, pShrLong, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pShrLong, pUshrLong, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pUshrLong, pIndexOf, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pIndexOf, pStringCompareTo, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pStringCompareTo, pMemcpy, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pMemcpy, pQuickImtConflictTrampoline, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pQuickImtConflictTrampoline, pQuickResolutionTrampoline,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pQuickResolutionTrampoline, pQuickToInterpreterBridge,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pQuickToInterpreterBridge,
                         pInvokeDirectTrampolineWithAccessCheck, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInvokeDirectTrampolineWithAccessCheck,
                         pInvokeInterfaceTrampolineWithAccessCheck, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInvokeInterfaceTrampolineWithAccessCheck,
                         pInvokeStaticTrampolineWithAccessCheck, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInvokeStaticTrampolineWithAccessCheck,
                         pInvokeSuperTrampolineWithAccessCheck, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInvokeSuperTrampolineWithAccessCheck,
                         pInvokeVirtualTrampolineWithAccessCheck, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pInvokeVirtualTrampolineWithAccessCheck,
                         pTestSuspend, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pTestSuspend, pDeliverException, sizeof(void*));

    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pDeliverException, pThrowArrayBounds, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pThrowArrayBounds, pThrowDivZero, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pThrowDivZero, pThrowNoSuchMethod, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pThrowNoSuchMethod, pThrowNullPointer, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pThrowNullPointer, pThrowStackOverflow, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pThrowStackOverflow, pDeoptimize, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pDeoptimize, pA64Load, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pA64Load, pA64Store, sizeof(void*));

    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pA64Store, pNewEmptyString, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewEmptyString, pNewStringFromBytes_B, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_B, pNewStringFromBytes_BI,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_BI, pNewStringFromBytes_BII,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_BII, pNewStringFromBytes_BIII,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_BIII, pNewStringFromBytes_BIIString,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_BIIString,
                         pNewStringFromBytes_BString, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_BString,
                         pNewStringFromBytes_BIICharset, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_BIICharset,
                         pNewStringFromBytes_BCharset, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromBytes_BCharset,
                         pNewStringFromChars_C, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromChars_C, pNewStringFromChars_CII,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromChars_CII, pNewStringFromChars_IIC,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromChars_IIC, pNewStringFromCodePoints,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromCodePoints, pNewStringFromString,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromString, pNewStringFromStringBuffer,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromStringBuffer, pNewStringFromStringBuilder,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pNewStringFromStringBuilder, pReadBarrierJni,
                         sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pReadBarrierJni, pReadBarrierMark, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pReadBarrierMark, pReadBarrierSlow, sizeof(void*));
    EXPECT_OFFSET_DIFFNP(QuickEntryPoints, pReadBarrierSlow, pReadBarrierForRootSlow,
                         sizeof(void*));

    CHECKED(OFFSETOF_MEMBER(QuickEntryPoints, pReadBarrierForRootSlow)
            + sizeof(void*) == sizeof(QuickEntryPoints), QuickEntryPoints_all);
  }
};

TEST_F(EntrypointsOrderTest, ThreadOffsets) {
  CheckThreadOffsets();
}

TEST_F(EntrypointsOrderTest, JniEntryPoints) {
  CheckJniEntryPoints();
}

TEST_F(EntrypointsOrderTest, QuickEntryPoints) {
  CheckQuickEntryPoints();
}

}  // namespace art
