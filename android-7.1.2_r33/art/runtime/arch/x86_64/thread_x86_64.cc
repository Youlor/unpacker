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

#include "thread.h"

#include "asm_support_x86_64.h"
#include "base/macros.h"
#include "thread-inl.h"
#include "thread_list.h"

#if defined(__linux__)
#include <asm/prctl.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#endif

namespace art {

#if defined(__linux__)
static void arch_prctl(int code, void* val) {
  syscall(__NR_arch_prctl, code, val);
}
#endif

void Thread::InitCpu() {
  MutexLock mu(nullptr, *Locks::modify_ldt_lock_);

#if defined(__linux__)
  arch_prctl(ARCH_SET_GS, this);
#else
  UNIMPLEMENTED(FATAL) << "Need to set GS";
#endif

  // Allow easy indirection back to Thread*.
  tlsPtr_.self = this;

  // Sanity check that reads from %gs point to this Thread*.
  Thread* self_check;
  __asm__ __volatile__("movq %%gs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);
}

void Thread::CleanupCpu() {
  // Sanity check that reads from %gs point to this Thread*.
  Thread* self_check;
  __asm__ __volatile__("movq %%gs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);

  // Do nothing.
}

}  // namespace art
