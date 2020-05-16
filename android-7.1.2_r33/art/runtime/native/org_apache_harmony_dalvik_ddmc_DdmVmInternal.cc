/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "org_apache_harmony_dalvik_ddmc_DdmVmInternal.h"

#include "base/logging.h"
#include "base/mutex.h"
#include "debugger.h"
#include "jni_internal.h"
#include "scoped_fast_native_object_access.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"
#include "stack.h"
#include "thread_list.h"

namespace art {

static void DdmVmInternal_enableRecentAllocations(JNIEnv*, jclass, jboolean enable) {
  Dbg::SetAllocTrackingEnabled(enable);
}

static jbyteArray DdmVmInternal_getRecentAllocations(JNIEnv* env, jclass) {
  ScopedFastNativeObjectAccess soa(env);
  return Dbg::GetRecentAllocations();
}

static jboolean DdmVmInternal_getRecentAllocationStatus(JNIEnv*, jclass) {
  return Runtime::Current()->GetHeap()->IsAllocTrackingEnabled();
}

/*
 * Get a stack trace as an array of StackTraceElement objects.  Returns
 * nullptr on failure, e.g. if the threadId couldn't be found.
 */
static jobjectArray DdmVmInternal_getStackTraceById(JNIEnv* env, jclass, jint thin_lock_id) {
  jobjectArray trace = nullptr;
  Thread* const self = Thread::Current();
  if (static_cast<uint32_t>(thin_lock_id) == self->GetThreadId()) {
    // No need to suspend ourself to build stacktrace.
    ScopedObjectAccess soa(env);
    jobject internal_trace = self->CreateInternalStackTrace<false>(soa);
    trace = Thread::InternalStackTraceToStackTraceElementArray(soa, internal_trace);
  } else {
    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    bool timed_out;

    // Check for valid thread
    if (thin_lock_id == ThreadList::kInvalidThreadId) {
      return nullptr;
    }

    // Suspend thread to build stack trace.
    Thread* thread = thread_list->SuspendThreadByThreadId(thin_lock_id, false, &timed_out);
    if (thread != nullptr) {
      {
        ScopedObjectAccess soa(env);
        jobject internal_trace = thread->CreateInternalStackTrace<false>(soa);
        trace = Thread::InternalStackTraceToStackTraceElementArray(soa, internal_trace);
      }
      // Restart suspended thread.
      thread_list->Resume(thread, false);
    } else {
      if (timed_out) {
        LOG(ERROR) << "Trying to get thread's stack by id failed as the thread failed to suspend "
            "within a generous timeout.";
      }
    }
  }
  return trace;
}

static void ThreadCountCallback(Thread*, void* context) {
  uint16_t& count = *reinterpret_cast<uint16_t*>(context);
  ++count;
}

static const int kThstBytesPerEntry = 18;
static const int kThstHeaderLen = 4;

static void ThreadStatsGetterCallback(Thread* t, void* context) {
  /*
   * Generate the contents of a THST chunk.  The data encompasses all known
   * threads.
   *
   * Response has:
   *  (1b) header len
   *  (1b) bytes per entry
   *  (2b) thread count
   * Then, for each thread:
   *  (4b) thread id
   *  (1b) thread status
   *  (4b) tid
   *  (4b) utime
   *  (4b) stime
   *  (1b) is daemon?
   *
   * The length fields exist in anticipation of adding additional fields
   * without wanting to break ddms or bump the full protocol version.  I don't
   * think it warrants full versioning.  They might be extraneous and could
   * be removed from a future version.
   */
  char native_thread_state;
  int utime;
  int stime;
  int task_cpu;
  GetTaskStats(t->GetTid(), &native_thread_state, &utime, &stime, &task_cpu);

  std::vector<uint8_t>& bytes = *reinterpret_cast<std::vector<uint8_t>*>(context);
  JDWP::Append4BE(bytes, t->GetThreadId());
  JDWP::Append1BE(bytes, Dbg::ToJdwpThreadStatus(t->GetState()));
  JDWP::Append4BE(bytes, t->GetTid());
  JDWP::Append4BE(bytes, utime);
  JDWP::Append4BE(bytes, stime);
  JDWP::Append1BE(bytes, t->IsDaemon());
}

static jbyteArray DdmVmInternal_getThreadStats(JNIEnv* env, jclass) {
  std::vector<uint8_t> bytes;
  Thread* self = static_cast<JNIEnvExt*>(env)->self;
  {
    MutexLock mu(self, *Locks::thread_list_lock_);
    ThreadList* thread_list = Runtime::Current()->GetThreadList();

    uint16_t thread_count = 0;
    thread_list->ForEach(ThreadCountCallback, &thread_count);

    JDWP::Append1BE(bytes, kThstHeaderLen);
    JDWP::Append1BE(bytes, kThstBytesPerEntry);
    JDWP::Append2BE(bytes, thread_count);

    thread_list->ForEach(ThreadStatsGetterCallback, &bytes);
  }

  jbyteArray result = env->NewByteArray(bytes.size());
  if (result != nullptr) {
    env->SetByteArrayRegion(result, 0, bytes.size(), reinterpret_cast<const jbyte*>(&bytes[0]));
  }
  return result;
}

static jint DdmVmInternal_heapInfoNotify(JNIEnv* env, jclass, jint when) {
  ScopedFastNativeObjectAccess soa(env);
  return Dbg::DdmHandleHpifChunk(static_cast<Dbg::HpifWhen>(when));
}

static jboolean DdmVmInternal_heapSegmentNotify(JNIEnv*, jclass, jint when, jint what, jboolean native) {
  return Dbg::DdmHandleHpsgNhsgChunk(static_cast<Dbg::HpsgWhen>(when), static_cast<Dbg::HpsgWhat>(what), native);
}

static void DdmVmInternal_threadNotify(JNIEnv*, jclass, jboolean enable) {
  Dbg::DdmSetThreadNotification(enable);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DdmVmInternal, enableRecentAllocations, "(Z)V"),
  NATIVE_METHOD(DdmVmInternal, getRecentAllocations, "!()[B"),
  NATIVE_METHOD(DdmVmInternal, getRecentAllocationStatus, "!()Z"),
  NATIVE_METHOD(DdmVmInternal, getStackTraceById, "(I)[Ljava/lang/StackTraceElement;"),
  NATIVE_METHOD(DdmVmInternal, getThreadStats, "()[B"),
  NATIVE_METHOD(DdmVmInternal, heapInfoNotify, "!(I)Z"),
  NATIVE_METHOD(DdmVmInternal, heapSegmentNotify, "(IIZ)Z"),
  NATIVE_METHOD(DdmVmInternal, threadNotify, "(Z)V"),
};

void register_org_apache_harmony_dalvik_ddmc_DdmVmInternal(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("org/apache/harmony/dalvik/ddmc/DdmVmInternal");
}

}  // namespace art
