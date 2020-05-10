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

#include "java_lang_DexCache.h"

#include "dex_file.h"
#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/dex_cache-inl.h"
#include "mirror/object-inl.h"
#include "scoped_fast_native_object_access.h"
#include "well_known_classes.h"

namespace art {

static jobject DexCache_getDexNative(JNIEnv* env, jobject javaDexCache) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::DexCache* dex_cache = soa.Decode<mirror::DexCache*>(javaDexCache);
  // Should only be called while holding the lock on the dex cache.
  DCHECK_EQ(dex_cache->GetLockOwnerThreadId(), soa.Self()->GetThreadId());
  const DexFile* dex_file = dex_cache->GetDexFile();
  if (dex_file == nullptr) {
    return nullptr;
  }
  void* address = const_cast<void*>(reinterpret_cast<const void*>(dex_file->Begin()));
  jobject byte_buffer = env->NewDirectByteBuffer(address, dex_file->Size());
  if (byte_buffer == nullptr) {
    DCHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }

  jvalue args[1];
  args[0].l = byte_buffer;
  return env->CallStaticObjectMethodA(WellKnownClasses::com_android_dex_Dex,
                                      WellKnownClasses::com_android_dex_Dex_create,
                                      args);
}

static jobject DexCache_getResolvedType(JNIEnv* env, jobject javaDexCache, jint type_index) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::DexCache* dex_cache = soa.Decode<mirror::DexCache*>(javaDexCache);
  CHECK_LT(static_cast<size_t>(type_index), dex_cache->NumResolvedTypes());
  return soa.AddLocalReference<jobject>(dex_cache->GetResolvedType(type_index));
}

static jobject DexCache_getResolvedString(JNIEnv* env, jobject javaDexCache, jint string_index) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::DexCache* dex_cache = soa.Decode<mirror::DexCache*>(javaDexCache);
  CHECK_LT(static_cast<size_t>(string_index), dex_cache->NumStrings());
  return soa.AddLocalReference<jobject>(dex_cache->GetResolvedString(string_index));
}

static void DexCache_setResolvedType(JNIEnv* env, jobject javaDexCache, jint type_index,
                                     jobject type) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::DexCache* dex_cache = soa.Decode<mirror::DexCache*>(javaDexCache);
  CHECK_LT(static_cast<size_t>(type_index), dex_cache->NumResolvedTypes());
  dex_cache->SetResolvedType(type_index, soa.Decode<mirror::Class*>(type));
}

static void DexCache_setResolvedString(JNIEnv* env, jobject javaDexCache, jint string_index,
                                       jobject string) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::DexCache* dex_cache = soa.Decode<mirror::DexCache*>(javaDexCache);
  CHECK_LT(static_cast<size_t>(string_index), dex_cache->NumStrings());
  dex_cache->SetResolvedString(string_index, soa.Decode<mirror::String*>(string));
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(DexCache, getDexNative, "!()Lcom/android/dex/Dex;"),
  NATIVE_METHOD(DexCache, getResolvedType, "!(I)Ljava/lang/Class;"),
  NATIVE_METHOD(DexCache, getResolvedString, "!(I)Ljava/lang/String;"),
  NATIVE_METHOD(DexCache, setResolvedType, "!(ILjava/lang/Class;)V"),
  NATIVE_METHOD(DexCache, setResolvedString, "!(ILjava/lang/String;)V"),
};

void register_java_lang_DexCache(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/DexCache");
}

}  // namespace art
