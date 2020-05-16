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
#include "entrypoints/entrypoint_utils.h"
#include "mirror/object-inl.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

// Used by the JNI dlsym stub to find the native method to invoke if none is registered.
#if defined(__arm__) || defined(__aarch64__)
extern "C" void* artFindNativeMethod() {
  Thread* self = Thread::Current();
#else
extern "C" void* artFindNativeMethod(Thread* self) {
  DCHECK_EQ(self, Thread::Current());
#endif
  Locks::mutator_lock_->AssertNotHeld(self);  // We come here as Native.
  ScopedObjectAccess soa(self);

  ArtMethod* method = self->GetCurrentMethod(nullptr);
  DCHECK(method != nullptr);

  // Lookup symbol address for method, on failure we'll return null with an exception set,
  // otherwise we return the address of the method we found.
  void* native_code = soa.Vm()->FindCodeForNativeMethod(method);
  if (native_code == nullptr) {
    DCHECK(self->IsExceptionPending());
    return nullptr;
  } else {
    // Register so that future calls don't come here
    method->RegisterNative(native_code, false);
    return native_code;
  }
}

}  // namespace art
