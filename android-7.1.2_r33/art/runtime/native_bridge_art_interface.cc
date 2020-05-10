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

#include "native_bridge_art_interface.h"

#include <signal.h>

#include "nativebridge/native_bridge.h"

#include "art_method-inl.h"
#include "base/logging.h"
#include "base/macros.h"
#include "dex_file-inl.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"
#include "sigchain.h"

namespace art {

static const char* GetMethodShorty(JNIEnv* env, jmethodID mid) {
  ScopedObjectAccess soa(env);
  ArtMethod* m = soa.DecodeMethod(mid);
  return m->GetShorty();
}

static uint32_t GetNativeMethodCount(JNIEnv* env, jclass clazz) {
  if (clazz == nullptr) {
    return 0;
  }

  ScopedObjectAccess soa(env);
  mirror::Class* c = soa.Decode<mirror::Class*>(clazz);

  uint32_t native_method_count = 0;
  for (auto& m : c->GetMethods(sizeof(void*))) {
    native_method_count += m.IsNative() ? 1u : 0u;
  }
  return native_method_count;
}

static uint32_t GetNativeMethods(JNIEnv* env, jclass clazz, JNINativeMethod* methods,
                                 uint32_t method_count) {
  if ((clazz == nullptr) || (methods == nullptr)) {
    return 0;
  }
  ScopedObjectAccess soa(env);
  mirror::Class* c = soa.Decode<mirror::Class*>(clazz);

  uint32_t count = 0;
  for (auto& m : c->GetMethods(sizeof(void*))) {
    if (m.IsNative()) {
      if (count < method_count) {
        methods[count].name = m.GetName();
        methods[count].signature = m.GetShorty();
        methods[count].fnPtr = m.GetEntryPointFromJni();
        count++;
      } else {
        LOG(WARNING) << "Output native method array too small. Skipping " << PrettyMethod(&m);
      }
    }
  }
  return count;
}

// Native bridge library runtime callbacks. They represent the runtime interface to native bridge.
//
// The interface is expected to expose the following methods:
// getMethodShorty(): in the case of native method calling JNI native function CallXXXXMethodY(),
//   native bridge calls back to VM for the shorty of the method so that it can prepare based on
//   host calling convention.
// getNativeMethodCount() and getNativeMethods(): in case of JNI function UnregisterNatives(),
//   native bridge can call back to get all native methods of specified class so that all
//   corresponding trampolines can be destroyed.
static android::NativeBridgeRuntimeCallbacks native_bridge_art_callbacks_ {
  GetMethodShorty, GetNativeMethodCount, GetNativeMethods
};

bool LoadNativeBridge(std::string& native_bridge_library_filename) {
  VLOG(startup) << "Runtime::Setup native bridge library: "
      << (native_bridge_library_filename.empty() ? "(empty)" : native_bridge_library_filename);
  return android::LoadNativeBridge(native_bridge_library_filename.c_str(),
                                   &native_bridge_art_callbacks_);
}

void PreInitializeNativeBridge(std::string dir) {
  VLOG(startup) << "Runtime::Pre-initialize native bridge";
#ifndef __APPLE__  // Mac OS does not support CLONE_NEWNS.
  if (unshare(CLONE_NEWNS) == -1) {
    LOG(WARNING) << "Could not create mount namespace.";
  }
  android::PreInitializeNativeBridge(dir.c_str(), GetInstructionSetString(kRuntimeISA));
#else
  UNUSED(dir);
#endif
}

void InitializeNativeBridge(JNIEnv* env, const char* instruction_set) {
  if (android::InitializeNativeBridge(env, instruction_set)) {
    if (android::NativeBridgeGetVersion() >= 2U) {
#ifdef _NSIG  // Undefined on Apple, but we don't support running on Mac, anyways.
      // Managed signal handling support added in version 2.
      for (int signal = 0; signal < _NSIG; ++signal) {
        android::NativeBridgeSignalHandlerFn fn = android::NativeBridgeGetSignalHandler(signal);
        if (fn != nullptr) {
          SetSpecialSignalHandlerFn(signal, fn);
        }
      }
#endif
    }
  }
}

void UnloadNativeBridge() {
  android::UnloadNativeBridge();
}

}  // namespace art
