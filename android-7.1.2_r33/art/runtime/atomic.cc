/*
 * Copyright (C) 2010 The Android Open Source Project
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

#include "atomic.h"
#include "base/mutex.h"
#include "base/stl_util.h"
#include "thread-inl.h"

namespace art {

std::vector<Mutex*>* QuasiAtomic::gSwapMutexes = nullptr;

Mutex* QuasiAtomic::GetSwapMutex(const volatile int64_t* addr) {
  return (*gSwapMutexes)[(reinterpret_cast<uintptr_t>(addr) >> 3U) % kSwapMutexCount];
}

void QuasiAtomic::Startup() {
  if (NeedSwapMutexes(kRuntimeISA)) {
    gSwapMutexes = new std::vector<Mutex*>;
    for (size_t i = 0; i < kSwapMutexCount; ++i) {
      gSwapMutexes->push_back(new Mutex("QuasiAtomic stripe", kSwapMutexesLock));
    }
  }
}

void QuasiAtomic::Shutdown() {
  if (NeedSwapMutexes(kRuntimeISA)) {
    STLDeleteElements(gSwapMutexes);
    delete gSwapMutexes;
  }
}

int64_t QuasiAtomic::SwapMutexRead64(volatile const int64_t* addr) {
  MutexLock mu(Thread::Current(), *GetSwapMutex(addr));
  return *addr;
}

void QuasiAtomic::SwapMutexWrite64(volatile int64_t* addr, int64_t value) {
  MutexLock mu(Thread::Current(), *GetSwapMutex(addr));
  *addr = value;
}


bool QuasiAtomic::SwapMutexCas64(int64_t old_value, int64_t new_value, volatile int64_t* addr) {
  MutexLock mu(Thread::Current(), *GetSwapMutex(addr));
  if (*addr == old_value) {
    *addr = new_value;
    return true;
  }
  return false;
}

}  // namespace art
