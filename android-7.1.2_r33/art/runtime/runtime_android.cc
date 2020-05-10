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
#include <string.h>
#include <sys/utsname.h>
#include <inttypes.h>

#include "base/logging.h"
#include "base/mutex.h"
#include "base/stringprintf.h"
#include "thread-inl.h"
#include "utils.h"

namespace art {

static constexpr bool kDumpHeapObjectOnSigsevg = false;
static constexpr bool kUseSignalHandler = false;

struct sigaction old_action;
void HandleUnexpectedSignal(int signal_number, siginfo_t* info, void* raw_context) {
  static bool handling_unexpected_signal = false;
  if (handling_unexpected_signal) {
    LogMessage::LogLine(__FILE__, __LINE__, INTERNAL_FATAL, "HandleUnexpectedSignal reentered\n");
    _exit(1);
  }
  handling_unexpected_signal = true;
  gAborting++;  // set before taking any locks
  MutexLock mu(Thread::Current(), *Locks::unexpected_signal_lock_);

  Runtime* runtime = Runtime::Current();
  if (runtime != nullptr) {
    // Print this out first in case DumpObject faults.
    LOG(INTERNAL_FATAL) << "Fault message: " << runtime->GetFaultMessage();
    gc::Heap* heap = runtime->GetHeap();
    if (kDumpHeapObjectOnSigsevg && heap != nullptr && info != nullptr) {
      LOG(INTERNAL_FATAL) << "Dump heap object at fault address: ";
      heap->DumpObject(LOG(INTERNAL_FATAL), reinterpret_cast<mirror::Object*>(info->si_addr));
    }
  }
  // Run the old signal handler.
  old_action.sa_sigaction(signal_number, info, raw_context);
}

void Runtime::InitPlatformSignalHandlers() {
  if (kUseSignalHandler) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_sigaction = HandleUnexpectedSignal;
    // Use the three-argument sa_sigaction handler.
    action.sa_flags |= SA_SIGINFO;
    // Use the alternate signal stack so we can catch stack overflows.
    action.sa_flags |= SA_ONSTACK;
    int rc = 0;
    rc += sigaction(SIGSEGV, &action, &old_action);
    CHECK_EQ(rc, 0);
  }
}

}  // namespace art
