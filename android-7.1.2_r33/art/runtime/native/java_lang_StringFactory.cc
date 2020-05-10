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

#include "java_lang_StringFactory.h"

#include "common_throws.h"
#include "jni_internal.h"
#include "mirror/object-inl.h"
#include "mirror/string.h"
#include "scoped_fast_native_object_access.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"
#include "ScopedPrimitiveArray.h"

namespace art {

static jstring StringFactory_newStringFromBytes(JNIEnv* env, jclass, jbyteArray java_data,
                                                jint high, jint offset, jint byte_count) {
  ScopedFastNativeObjectAccess soa(env);
  if (UNLIKELY(java_data == nullptr)) {
    ThrowNullPointerException("data == null");
    return nullptr;
  }
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ByteArray> byte_array(hs.NewHandle(soa.Decode<mirror::ByteArray*>(java_data)));
  int32_t data_size = byte_array->GetLength();
  if ((offset | byte_count) < 0 || byte_count > data_size - offset) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/StringIndexOutOfBoundsException;",
                                   "length=%d; regionStart=%d; regionLength=%d", data_size,
                                   offset, byte_count);
    return nullptr;
  }
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  mirror::String* result = mirror::String::AllocFromByteArray<true>(soa.Self(), byte_count,
                                                                    byte_array, offset, high,
                                                                    allocator_type);
  return soa.AddLocalReference<jstring>(result);
}

// The char array passed as `java_data` must not be a null reference.
static jstring StringFactory_newStringFromChars(JNIEnv* env, jclass, jint offset,
                                                jint char_count, jcharArray java_data) {
  DCHECK(java_data != nullptr);
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::CharArray> char_array(hs.NewHandle(soa.Decode<mirror::CharArray*>(java_data)));
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  mirror::String* result = mirror::String::AllocFromCharArray<true>(soa.Self(), char_count,
                                                                    char_array, offset,
                                                                    allocator_type);
  return soa.AddLocalReference<jstring>(result);
}

static jstring StringFactory_newStringFromString(JNIEnv* env, jclass, jstring to_copy) {
  ScopedFastNativeObjectAccess soa(env);
  if (UNLIKELY(to_copy == nullptr)) {
    ThrowNullPointerException("toCopy == null");
    return nullptr;
  }
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::String> string(hs.NewHandle(soa.Decode<mirror::String*>(to_copy)));
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  mirror::String* result = mirror::String::AllocFromString<true>(soa.Self(), string->GetLength(),
                                                                 string, 0, allocator_type);
  return soa.AddLocalReference<jstring>(result);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(StringFactory, newStringFromBytes, "!([BIII)Ljava/lang/String;"),
  NATIVE_METHOD(StringFactory, newStringFromChars, "!(II[C)Ljava/lang/String;"),
  NATIVE_METHOD(StringFactory, newStringFromString, "!(Ljava/lang/String;)Ljava/lang/String;"),
};

void register_java_lang_StringFactory(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/StringFactory");
}

}  // namespace art
