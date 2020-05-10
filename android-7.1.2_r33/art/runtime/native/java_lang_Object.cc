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

#include "java_lang_Object.h"

#include "jni_internal.h"
#include "mirror/object-inl.h"
#include "scoped_fast_native_object_access.h"


namespace art {

static jobject Object_internalClone(JNIEnv* env, jobject java_this) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* o = soa.Decode<mirror::Object*>(java_this);
  return soa.AddLocalReference<jobject>(o->Clone(soa.Self()));
}

static void Object_notify(JNIEnv* env, jobject java_this) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* o = soa.Decode<mirror::Object*>(java_this);
  o->Notify(soa.Self());
}

static void Object_notifyAll(JNIEnv* env, jobject java_this) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* o = soa.Decode<mirror::Object*>(java_this);
  o->NotifyAll(soa.Self());
}

static void Object_wait(JNIEnv* env, jobject java_this) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* o = soa.Decode<mirror::Object*>(java_this);
  o->Wait(soa.Self());
}

static void Object_waitJI(JNIEnv* env, jobject java_this, jlong ms, jint ns) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* o = soa.Decode<mirror::Object*>(java_this);
  o->Wait(soa.Self(), ms, ns);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Object, internalClone, "!()Ljava/lang/Object;"),
  NATIVE_METHOD(Object, notify, "!()V"),
  NATIVE_METHOD(Object, notifyAll, "!()V"),
  OVERLOADED_NATIVE_METHOD(Object, wait, "!()V", wait),
  OVERLOADED_NATIVE_METHOD(Object, wait, "!(JI)V", waitJI),
};

void register_java_lang_Object(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Object");
}

}  // namespace art
