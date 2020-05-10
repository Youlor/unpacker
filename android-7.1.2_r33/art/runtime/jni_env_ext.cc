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

#include "jni_env_ext.h"

#include <algorithm>
#include <vector>

#include "check_jni.h"
#include "indirect_reference_table.h"
#include "java_vm_ext.h"
#include "jni_internal.h"
#include "lock_word.h"
#include "mirror/object-inl.h"
#include "nth_caller_visitor.h"
#include "thread-inl.h"

namespace art {

static constexpr size_t kMonitorsInitial = 32;  // Arbitrary.
static constexpr size_t kMonitorsMax = 4096;  // Arbitrary sanity check.

static constexpr size_t kLocalsInitial = 64;  // Arbitrary.

// Checking "locals" requires the mutator lock, but at creation time we're really only interested
// in validity, which isn't changing. To avoid grabbing the mutator lock, factored out and tagged
// with NO_THREAD_SAFETY_ANALYSIS.
static bool CheckLocalsValid(JNIEnvExt* in) NO_THREAD_SAFETY_ANALYSIS {
  if (in == nullptr) {
    return false;
  }
  return in->locals.IsValid();
}

JNIEnvExt* JNIEnvExt::Create(Thread* self_in, JavaVMExt* vm_in) {
  std::unique_ptr<JNIEnvExt> ret(new JNIEnvExt(self_in, vm_in));
  if (CheckLocalsValid(ret.get())) {
    return ret.release();
  }
  return nullptr;
}

JNIEnvExt::JNIEnvExt(Thread* self_in, JavaVMExt* vm_in)
    : self(self_in),
      vm(vm_in),
      local_ref_cookie(IRT_FIRST_SEGMENT),
      locals(kLocalsInitial, kLocalsMax, kLocal, false),
      check_jni(false),
      runtime_deleted(false),
      critical(0),
      monitors("monitors", kMonitorsInitial, kMonitorsMax) {
  functions = unchecked_functions = GetJniNativeInterface();
  if (vm->IsCheckJniEnabled()) {
    SetCheckJniEnabled(true);
  }
}

void JNIEnvExt::SetFunctionsToRuntimeShutdownFunctions() {
  functions = GetRuntimeShutdownNativeInterface();
  runtime_deleted = true;
}

JNIEnvExt::~JNIEnvExt() {
}

jobject JNIEnvExt::NewLocalRef(mirror::Object* obj) {
  if (obj == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<jobject>(locals.Add(local_ref_cookie, obj));
}

void JNIEnvExt::DeleteLocalRef(jobject obj) {
  if (obj != nullptr) {
    locals.Remove(local_ref_cookie, reinterpret_cast<IndirectRef>(obj));
  }
}

void JNIEnvExt::SetCheckJniEnabled(bool enabled) {
  check_jni = enabled;
  functions = enabled ? GetCheckJniNativeInterface() : GetJniNativeInterface();
}

void JNIEnvExt::DumpReferenceTables(std::ostream& os) {
  locals.Dump(os);
  monitors.Dump(os);
}

void JNIEnvExt::PushFrame(int capacity ATTRIBUTE_UNUSED) {
  // TODO: take 'capacity' into account.
  stacked_local_ref_cookies.push_back(local_ref_cookie);
  local_ref_cookie = locals.GetSegmentState();
}

void JNIEnvExt::PopFrame() {
  locals.SetSegmentState(local_ref_cookie);
  local_ref_cookie = stacked_local_ref_cookies.back();
  stacked_local_ref_cookies.pop_back();
}

// Note: the offset code is brittle, as we can't use OFFSETOF_MEMBER or offsetof easily. Thus, there
//       are tests in jni_internal_test to match the results against the actual values.

// This is encoding the knowledge of the structure and layout of JNIEnv fields.
static size_t JNIEnvSize(size_t pointer_size) {
  // A single pointer.
  return pointer_size;
}

Offset JNIEnvExt::SegmentStateOffset(size_t pointer_size) {
  size_t locals_offset = JNIEnvSize(pointer_size) +
                         2 * pointer_size +          // Thread* self + JavaVMExt* vm.
                         4 +                         // local_ref_cookie.
                         (pointer_size - 4);         // Padding.
  size_t irt_segment_state_offset =
      IndirectReferenceTable::SegmentStateOffset(pointer_size).Int32Value();
  return Offset(locals_offset + irt_segment_state_offset);
}

Offset JNIEnvExt::LocalRefCookieOffset(size_t pointer_size) {
  return Offset(JNIEnvSize(pointer_size) +
                2 * pointer_size);          // Thread* self + JavaVMExt* vm
}

Offset JNIEnvExt::SelfOffset(size_t pointer_size) {
  return Offset(JNIEnvSize(pointer_size));
}

// Use some defining part of the caller's frame as the identifying mark for the JNI segment.
static uintptr_t GetJavaCallFrame(Thread* self) SHARED_REQUIRES(Locks::mutator_lock_) {
  NthCallerVisitor zeroth_caller(self, 0, false);
  zeroth_caller.WalkStack();
  if (zeroth_caller.caller == nullptr) {
    // No Java code, must be from pure native code.
    return 0;
  } else if (zeroth_caller.GetCurrentQuickFrame() == nullptr) {
    // Shadow frame = interpreter. Use the actual shadow frame's address.
    DCHECK(zeroth_caller.GetCurrentShadowFrame() != nullptr);
    return reinterpret_cast<uintptr_t>(zeroth_caller.GetCurrentShadowFrame());
  } else {
    // Quick frame = compiled code. Use the bottom of the frame.
    return reinterpret_cast<uintptr_t>(zeroth_caller.GetCurrentQuickFrame());
  }
}

void JNIEnvExt::RecordMonitorEnter(jobject obj) {
  locked_objects_.push_back(std::make_pair(GetJavaCallFrame(self), obj));
}

static std::string ComputeMonitorDescription(Thread* self,
                                             jobject obj) SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::Object* o = self->DecodeJObject(obj);
  if ((o->GetLockWord(false).GetState() == LockWord::kThinLocked) &&
      Locks::mutator_lock_->IsExclusiveHeld(self)) {
    // Getting the identity hashcode here would result in lock inflation and suspension of the
    // current thread, which isn't safe if this is the only runnable thread.
    return StringPrintf("<@addr=0x%" PRIxPTR "> (a %s)",
                        reinterpret_cast<intptr_t>(o),
                        PrettyTypeOf(o).c_str());
  } else {
    // IdentityHashCode can cause thread suspension, which would invalidate o if it moved. So
    // we get the pretty type before we call IdentityHashCode.
    const std::string pretty_type(PrettyTypeOf(o));
    return StringPrintf("<0x%08x> (a %s)", o->IdentityHashCode(), pretty_type.c_str());
  }
}

static void RemoveMonitors(Thread* self,
                           uintptr_t frame,
                           ReferenceTable* monitors,
                           std::vector<std::pair<uintptr_t, jobject>>* locked_objects)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  auto kept_end = std::remove_if(
      locked_objects->begin(),
      locked_objects->end(),
      [self, frame, monitors](const std::pair<uintptr_t, jobject>& pair)
          SHARED_REQUIRES(Locks::mutator_lock_) {
        if (frame == pair.first) {
          mirror::Object* o = self->DecodeJObject(pair.second);
          monitors->Remove(o);
          return true;
        }
        return false;
      });
  locked_objects->erase(kept_end, locked_objects->end());
}

void JNIEnvExt::CheckMonitorRelease(jobject obj) {
  uintptr_t current_frame = GetJavaCallFrame(self);
  std::pair<uintptr_t, jobject> exact_pair = std::make_pair(current_frame, obj);
  auto it = std::find(locked_objects_.begin(), locked_objects_.end(), exact_pair);
  bool will_abort = false;
  if (it != locked_objects_.end()) {
    locked_objects_.erase(it);
  } else {
    // Check whether this monitor was locked in another JNI "session."
    mirror::Object* mirror_obj = self->DecodeJObject(obj);
    for (std::pair<uintptr_t, jobject>& pair : locked_objects_) {
      if (self->DecodeJObject(pair.second) == mirror_obj) {
        std::string monitor_descr = ComputeMonitorDescription(self, pair.second);
        vm->JniAbortF("<JNI MonitorExit>",
                      "Unlocking monitor that wasn't locked here: %s",
                      monitor_descr.c_str());
        will_abort = true;
        break;
      }
    }
  }

  // When we abort, also make sure that any locks from the current "session" are removed from
  // the monitors table, otherwise we may visit local objects in GC during abort (which won't be
  // valid anymore).
  if (will_abort) {
    RemoveMonitors(self, current_frame, &monitors, &locked_objects_);
  }
}

void JNIEnvExt::CheckNoHeldMonitors() {
  uintptr_t current_frame = GetJavaCallFrame(self);
  // The locked_objects_ are grouped by their stack frame component, as this enforces structured
  // locking, and the groups form a stack. So the current frame entries are at the end. Check
  // whether the vector is empty, and when there are elements, whether the last element belongs
  // to this call - this signals that there are unlocked monitors.
  if (!locked_objects_.empty()) {
    std::pair<uintptr_t, jobject>& pair = locked_objects_[locked_objects_.size() - 1];
    if (pair.first == current_frame) {
      std::string monitor_descr = ComputeMonitorDescription(self, pair.second);
      vm->JniAbortF("<JNI End>",
                    "Still holding a locked object on JNI end: %s",
                    monitor_descr.c_str());
      // When we abort, also make sure that any locks from the current "session" are removed from
      // the monitors table, otherwise we may visit local objects in GC during abort.
      RemoveMonitors(self, current_frame, &monitors, &locked_objects_);
    } else if (kIsDebugBuild) {
      // Make sure there are really no other entries and our checking worked as expected.
      for (std::pair<uintptr_t, jobject>& check_pair : locked_objects_) {
        CHECK_NE(check_pair.first, current_frame);
      }
    }
  }
}

}  // namespace art
