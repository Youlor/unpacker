/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "art_method-inl.h"
#include "base/logging.h"
#include "callee_save_frame.h"
#include "dex_file-inl.h"
#include "interpreter/interpreter.h"
#include "mirror/class-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/object-inl.h"
#include "quick_exception_handler.h"
#include "stack.h"
#include "thread.h"
#include "verifier/method_verifier.h"

namespace art {

extern "C" NO_RETURN void artDeoptimize(Thread* self) SHARED_REQUIRES(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);

  if (VLOG_IS_ON(deopt)) {
    LOG(INFO) << "Deopting:";
    self->Dump(LOG(INFO));
  }

  self->AssertHasDeoptimizationContext();
  self->SetException(Thread::GetDeoptimizationException());
  self->QuickDeliverException();
}

extern "C" NO_RETURN void artDeoptimizeFromCompiledCode(Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);

  // Deopt logging will be in DeoptimizeSingleFrame. It is there to take advantage of the
  // specialized visitor that will show whether a method is Quick or Shadow.

  // Before deoptimizing to interpreter, we must push the deoptimization context.
  JValue return_value;
  return_value.SetJ(0);  // we never deoptimize from compiled code with an invoke result.
  self->PushDeoptimizationContext(return_value, false, /* from_code */ true, self->GetException());

  QuickExceptionHandler exception_handler(self, true);
  exception_handler.DeoptimizeSingleFrame();
  exception_handler.UpdateInstrumentationStack();
  exception_handler.DeoptimizeSingleFrameArchDependentFixup();
  // We cannot smash the caller-saves, as we need the ArtMethod in a parameter register that would
  // be caller-saved. This has the downside that we cannot track incorrect register usage down the
  // line.
  exception_handler.DoLongJump(false);
}

}  // namespace art
