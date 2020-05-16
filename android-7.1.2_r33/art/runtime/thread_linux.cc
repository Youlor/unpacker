/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include <signal.h>

#include "thread.h"
#include "utils.h"

namespace art {

void Thread::SetNativePriority(int) {
  // Do nothing.
}

int Thread::GetNativePriority() {
  return kNormThreadPriority;
}

static void SigAltStack(stack_t* new_stack, stack_t* old_stack) {
  if (sigaltstack(new_stack, old_stack) == -1) {
    PLOG(FATAL) << "sigaltstack failed";
  }
}

// The default SIGSTKSZ on linux is 8K.  If we do any logging in a signal
// handler or do a stack unwind, this is too small.  We allocate 32K
// instead of the minimum signal stack size.
// TODO: We shouldn't do logging (with locks) in signal handlers.
static constexpr int kHostAltSigStackSize =
    32 * KB < MINSIGSTKSZ ? MINSIGSTKSZ : 32 * KB;

void Thread::SetUpAlternateSignalStack() {
  // Create and set an alternate signal stack.
#ifdef __ANDROID__
  LOG(FATAL) << "Invalid use of alternate signal stack on Android";
#endif
  stack_t ss;
  ss.ss_sp = new uint8_t[kHostAltSigStackSize];
  ss.ss_size = kHostAltSigStackSize;
  ss.ss_flags = 0;
  CHECK(ss.ss_sp != nullptr);
  SigAltStack(&ss, nullptr);

  // Double-check that it worked.
  ss.ss_sp = nullptr;
  SigAltStack(nullptr, &ss);
  VLOG(threads) << "Alternate signal stack is " << PrettySize(ss.ss_size) << " at " << ss.ss_sp;
}

void Thread::TearDownAlternateSignalStack() {
  // Get the pointer so we can free the memory.
  stack_t ss;
  SigAltStack(nullptr, &ss);
  uint8_t* allocated_signal_stack = reinterpret_cast<uint8_t*>(ss.ss_sp);

  // Tell the kernel to stop using it.
  ss.ss_sp = nullptr;
  ss.ss_flags = SS_DISABLE;
  ss.ss_size = kHostAltSigStackSize;  // Avoid ENOMEM failure with Mac OS' buggy libc.
  SigAltStack(&ss, nullptr);

  // Free it.
  delete[] allocated_signal_stack;
}

}  // namespace art
