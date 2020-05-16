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

#include "dalvik_system_VMStack.h"

#include "art_method-inl.h"
#include "jni_internal.h"
#include "nth_caller_visitor.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "scoped_fast_native_object_access.h"
#include "scoped_thread_state_change.h"
#include "thread_list.h"

namespace art {

static jobject GetThreadStack(const ScopedFastNativeObjectAccess& soa, jobject peer)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  jobject trace = nullptr;
  if (soa.Decode<mirror::Object*>(peer) == soa.Self()->GetPeer()) {
    trace = soa.Self()->CreateInternalStackTrace<false>(soa);
  } else {
    // Suspend thread to build stack trace.
    ScopedThreadSuspension sts(soa.Self(), kNative);
    ThreadList* thread_list = Runtime::Current()->GetThreadList();
    bool timed_out;
    Thread* thread = thread_list->SuspendThreadByPeer(peer, true, false, &timed_out);
    if (thread != nullptr) {
      // Must be runnable to create returned array.
      {
        ScopedObjectAccess soa2(soa.Self());
        trace = thread->CreateInternalStackTrace<false>(soa);
      }
      // Restart suspended thread.
      thread_list->Resume(thread, false);
    } else if (timed_out) {
      LOG(ERROR) << "Trying to get thread's stack failed as the thread failed to suspend within a "
          "generous timeout.";
    }
  }
  return trace;
}

static jint VMStack_fillStackTraceElements(JNIEnv* env, jclass, jobject javaThread,
                                           jobjectArray javaSteArray) {
  ScopedFastNativeObjectAccess soa(env);
  jobject trace = GetThreadStack(soa, javaThread);
  if (trace == nullptr) {
    return 0;
  }
  int32_t depth;
  Thread::InternalStackTraceToStackTraceElementArray(soa, trace, javaSteArray, &depth);
  return depth;
}

// Returns the defining class loader of the caller's caller.
static jobject VMStack_getCallingClassLoader(JNIEnv* env, jclass) {
  ScopedFastNativeObjectAccess soa(env);
  NthCallerVisitor visitor(soa.Self(), 2);
  visitor.WalkStack();
  if (UNLIKELY(visitor.caller == nullptr)) {
    // The caller is an attached native thread.
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(visitor.caller->GetDeclaringClass()->GetClassLoader());
}

static jobject VMStack_getClosestUserClassLoader(JNIEnv* env, jclass) {
  struct ClosestUserClassLoaderVisitor : public StackVisitor {
    explicit ClosestUserClassLoaderVisitor(Thread* thread)
      : StackVisitor(thread, nullptr, StackVisitor::StackWalkKind::kIncludeInlinedFrames),
        class_loader(nullptr) {}

    bool VisitFrame() SHARED_REQUIRES(Locks::mutator_lock_) {
      DCHECK(class_loader == nullptr);
      mirror::Class* c = GetMethod()->GetDeclaringClass();
      // c is null for runtime methods.
      if (c != nullptr) {
        mirror::Object* cl = c->GetClassLoader();
        if (cl != nullptr) {
          class_loader = cl;
          return false;
        }
      }
      return true;
    }

    mirror::Object* class_loader;
  };
  ScopedFastNativeObjectAccess soa(env);
  ClosestUserClassLoaderVisitor visitor(soa.Self());
  visitor.WalkStack();
  return soa.AddLocalReference<jobject>(visitor.class_loader);
}

// Returns the class of the caller's caller's caller.
static jclass VMStack_getStackClass2(JNIEnv* env, jclass) {
  ScopedFastNativeObjectAccess soa(env);
  NthCallerVisitor visitor(soa.Self(), 3);
  visitor.WalkStack();
  if (UNLIKELY(visitor.caller == nullptr)) {
    // The caller is an attached native thread.
    return nullptr;
  }
  return soa.AddLocalReference<jclass>(visitor.caller->GetDeclaringClass());
}

static jobjectArray VMStack_getThreadStackTrace(JNIEnv* env, jclass, jobject javaThread) {
  ScopedFastNativeObjectAccess soa(env);
  jobject trace = GetThreadStack(soa, javaThread);
  if (trace == nullptr) {
    return nullptr;
  }
  return Thread::InternalStackTraceToStackTraceElementArray(soa, trace);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(VMStack, fillStackTraceElements, "!(Ljava/lang/Thread;[Ljava/lang/StackTraceElement;)I"),
  NATIVE_METHOD(VMStack, getCallingClassLoader, "!()Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(VMStack, getClosestUserClassLoader, "!()Ljava/lang/ClassLoader;"),
  NATIVE_METHOD(VMStack, getStackClass2, "!()Ljava/lang/Class;"),
  NATIVE_METHOD(VMStack, getThreadStackTrace, "!(Ljava/lang/Thread;)[Ljava/lang/StackTraceElement;"),
};

void register_dalvik_system_VMStack(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("dalvik/system/VMStack");
}

}  // namespace art
