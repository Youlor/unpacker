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

#include "sun_misc_Unsafe.h"
#include "common_throws.h"
#include "gc/accounting/card_table-inl.h"
#include "jni_internal.h"
#include "mirror/array.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "scoped_fast_native_object_access.h"

#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <atomic>

namespace art {

static jboolean Unsafe_compareAndSwapInt(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                         jint expectedValue, jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  bool success = obj->CasFieldStrongSequentiallyConsistent32<false>(MemberOffset(offset),
                                                                    expectedValue, newValue);
  return success ? JNI_TRUE : JNI_FALSE;
}

static jboolean Unsafe_compareAndSwapLong(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                          jlong expectedValue, jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  bool success = obj->CasFieldStrongSequentiallyConsistent64<false>(MemberOffset(offset),
                                                                    expectedValue, newValue);
  return success ? JNI_TRUE : JNI_FALSE;
}

static jboolean Unsafe_compareAndSwapObject(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                            jobject javaExpectedValue, jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* expectedValue = soa.Decode<mirror::Object*>(javaExpectedValue);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  // JNI must use non transactional mode.
  if (kUseReadBarrier) {
    // Need to make sure the reference stored in the field is a to-space one before attempting the
    // CAS or the CAS could fail incorrectly.
    mirror::HeapReference<mirror::Object>* field_addr =
        reinterpret_cast<mirror::HeapReference<mirror::Object>*>(
            reinterpret_cast<uint8_t*>(obj) + static_cast<size_t>(offset));
    ReadBarrier::Barrier<mirror::Object, kWithReadBarrier, /*kAlwaysUpdateField*/true>(
        obj,
        MemberOffset(offset),
        field_addr);
  }
  bool success = obj->CasFieldStrongSequentiallyConsistentObject<false>(MemberOffset(offset),
                                                                        expectedValue, newValue);
  return success ? JNI_TRUE : JNI_FALSE;
}

static jint Unsafe_getInt(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField32(MemberOffset(offset));
}

static jint Unsafe_getIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField32Volatile(MemberOffset(offset));
}

static void Unsafe_putInt(JNIEnv* env, jobject, jobject javaObj, jlong offset, jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putIntVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                  jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField32Volatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedInt(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                 jint newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  QuasiAtomic::ThreadFenceRelease();
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), newValue);
}

static jlong Unsafe_getLong(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField64(MemberOffset(offset));
}

static jlong Unsafe_getLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  return obj->GetField64Volatile(MemberOffset(offset));
}

static void Unsafe_putLong(JNIEnv* env, jobject, jobject javaObj, jlong offset, jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putLongVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                   jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  // JNI must use non transactional mode.
  obj->SetField64Volatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedLong(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                  jlong newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  QuasiAtomic::ThreadFenceRelease();
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), newValue);
}

static jobject Unsafe_getObjectVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* value = obj->GetFieldObjectVolatile<mirror::Object>(MemberOffset(offset));
  return soa.AddLocalReference<jobject>(value);
}

static jobject Unsafe_getObject(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* value = obj->GetFieldObject<mirror::Object>(MemberOffset(offset));
  return soa.AddLocalReference<jobject>(value);
}

static void Unsafe_putObject(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                             jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  // JNI must use non transactional mode.
  obj->SetFieldObject<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putObjectVolatile(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                     jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  // JNI must use non transactional mode.
  obj->SetFieldObjectVolatile<false>(MemberOffset(offset), newValue);
}

static void Unsafe_putOrderedObject(JNIEnv* env, jobject, jobject javaObj, jlong offset,
                                    jobject javaNewValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  mirror::Object* newValue = soa.Decode<mirror::Object*>(javaNewValue);
  QuasiAtomic::ThreadFenceRelease();
  // JNI must use non transactional mode.
  obj->SetFieldObject<false>(MemberOffset(offset), newValue);
}

static jint Unsafe_getArrayBaseOffsetForComponentType(JNIEnv* env, jclass, jobject component_class) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Class* component = soa.Decode<mirror::Class*>(component_class);
  Primitive::Type primitive_type = component->GetPrimitiveType();
  return mirror::Array::DataOffset(Primitive::ComponentSize(primitive_type)).Int32Value();
}

static jint Unsafe_getArrayIndexScaleForComponentType(JNIEnv* env, jclass, jobject component_class) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Class* component = soa.Decode<mirror::Class*>(component_class);
  Primitive::Type primitive_type = component->GetPrimitiveType();
  return Primitive::ComponentSize(primitive_type);
}

static jint Unsafe_addressSize(JNIEnv* env ATTRIBUTE_UNUSED, jobject ob ATTRIBUTE_UNUSED) {
  return sizeof(void*);
}

static jint Unsafe_pageSize(JNIEnv* env ATTRIBUTE_UNUSED, jobject ob ATTRIBUTE_UNUSED) {
  return sysconf(_SC_PAGESIZE);
}

static jlong Unsafe_allocateMemory(JNIEnv* env, jobject, jlong bytes) {
  ScopedFastNativeObjectAccess soa(env);
  // bytes is nonnegative and fits into size_t
  if (bytes < 0 || bytes != (jlong)(size_t) bytes) {
    ThrowIllegalAccessException("wrong number of bytes");
    return 0;
  }
  void* mem = malloc(bytes);
  if (mem == nullptr) {
    soa.Self()->ThrowOutOfMemoryError("native alloc");
    return 0;
  }
  return (uintptr_t) mem;
}

static void Unsafe_freeMemory(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  free(reinterpret_cast<void*>(static_cast<uintptr_t>(address)));
}

static void Unsafe_setMemory(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jlong bytes, jbyte value) {
  memset(reinterpret_cast<void*>(static_cast<uintptr_t>(address)), value, bytes);
}

static jbyte Unsafe_getByteJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  return *reinterpret_cast<jbyte*>(address);
}

static void Unsafe_putByteJB(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jbyte value) {
  *reinterpret_cast<jbyte*>(address) = value;
}

static jshort Unsafe_getShortJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  return *reinterpret_cast<jshort*>(address);
}

static void Unsafe_putShortJS(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jshort value) {
  *reinterpret_cast<jshort*>(address) = value;
}

static jchar Unsafe_getCharJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  return *reinterpret_cast<jchar*>(address);
}

static void Unsafe_putCharJC(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jchar value) {
  *reinterpret_cast<jchar*>(address) = value;
}

static jint Unsafe_getIntJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  return *reinterpret_cast<jint*>(address);
}

static void Unsafe_putIntJI(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jint value) {
  *reinterpret_cast<jint*>(address) = value;
}

static jlong Unsafe_getLongJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  return *reinterpret_cast<jlong*>(address);
}

static void Unsafe_putLongJJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jlong value) {
  *reinterpret_cast<jlong*>(address) = value;
}

static jfloat Unsafe_getFloatJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  return *reinterpret_cast<jfloat*>(address);
}

static void Unsafe_putFloatJF(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jfloat value) {
  *reinterpret_cast<jfloat*>(address) = value;
}
static jdouble Unsafe_getDoubleJ(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address) {
  return *reinterpret_cast<jdouble*>(address);
}

static void Unsafe_putDoubleJD(JNIEnv* env ATTRIBUTE_UNUSED, jobject, jlong address, jdouble value) {
  *reinterpret_cast<jdouble*>(address) = value;
}

static void Unsafe_copyMemory(JNIEnv *env, jobject unsafe ATTRIBUTE_UNUSED, jlong src,
                              jlong dst, jlong size) {
    if (size == 0) {
        return;
    }
    // size is nonnegative and fits into size_t
    if (size < 0 || size != (jlong)(size_t) size) {
        ScopedFastNativeObjectAccess soa(env);
        ThrowIllegalAccessException("wrong number of bytes");
    }
    size_t sz = (size_t)size;
    memcpy(reinterpret_cast<void *>(dst), reinterpret_cast<void *>(src), sz);
}

template<typename T>
static void copyToArray(jlong srcAddr, mirror::PrimitiveArray<T>* array,
                        size_t array_offset,
                        size_t size)
        SHARED_REQUIRES(Locks::mutator_lock_) {
    const T* src = reinterpret_cast<T*>(srcAddr);
    size_t sz = size / sizeof(T);
    size_t of = array_offset / sizeof(T);
    for (size_t i = 0; i < sz; ++i) {
        array->Set(i + of, *(src + i));
    }
}

template<typename T>
static void copyFromArray(jlong dstAddr, mirror::PrimitiveArray<T>* array,
                          size_t array_offset,
                          size_t size)
        SHARED_REQUIRES(Locks::mutator_lock_) {
    T* dst = reinterpret_cast<T*>(dstAddr);
    size_t sz = size / sizeof(T);
    size_t of = array_offset / sizeof(T);
    for (size_t i = 0; i < sz; ++i) {
        *(dst + i) = array->Get(i + of);
    }
}

static void Unsafe_copyMemoryToPrimitiveArray(JNIEnv *env,
                                              jobject unsafe ATTRIBUTE_UNUSED,
                                              jlong srcAddr,
                                              jobject dstObj,
                                              jlong dstOffset,
                                              jlong size) {
    ScopedObjectAccess soa(env);
    if (size == 0) {
        return;
    }
    // size is nonnegative and fits into size_t
    if (size < 0 || size != (jlong)(size_t) size) {
        ThrowIllegalAccessException("wrong number of bytes");
    }
    size_t sz = (size_t)size;
    size_t dst_offset = (size_t)dstOffset;
    mirror::Object* dst = soa.Decode<mirror::Object*>(dstObj);
    mirror::Class* component_type = dst->GetClass()->GetComponentType();
    if (component_type->IsPrimitiveByte() || component_type->IsPrimitiveBoolean()) {
        copyToArray(srcAddr, dst->AsByteSizedArray(), dst_offset, sz);
    } else if (component_type->IsPrimitiveShort() || component_type->IsPrimitiveChar()) {
        copyToArray(srcAddr, dst->AsShortSizedArray(), dst_offset, sz);
    } else if (component_type->IsPrimitiveInt() || component_type->IsPrimitiveFloat()) {
        copyToArray(srcAddr, dst->AsIntArray(), dst_offset, sz);
    } else if (component_type->IsPrimitiveLong() || component_type->IsPrimitiveDouble()) {
        copyToArray(srcAddr, dst->AsLongArray(), dst_offset, sz);
    } else {
        ThrowIllegalAccessException("not a primitive array");
    }
}

static void Unsafe_copyMemoryFromPrimitiveArray(JNIEnv *env,
                                                jobject unsafe ATTRIBUTE_UNUSED,
                                                jobject srcObj,
                                                jlong srcOffset,
                                                jlong dstAddr,
                                                jlong size) {
    ScopedObjectAccess soa(env);
    if (size == 0) {
        return;
    }
    // size is nonnegative and fits into size_t
    if (size < 0 || size != (jlong)(size_t) size) {
        ThrowIllegalAccessException("wrong number of bytes");
    }
    size_t sz = (size_t)size;
    size_t src_offset = (size_t)srcOffset;
    mirror::Object* src = soa.Decode<mirror::Object*>(srcObj);
    mirror::Class* component_type = src->GetClass()->GetComponentType();
    if (component_type->IsPrimitiveByte() || component_type->IsPrimitiveBoolean()) {
        copyFromArray(dstAddr, src->AsByteSizedArray(), src_offset, sz);
    } else if (component_type->IsPrimitiveShort() || component_type->IsPrimitiveChar()) {
        copyFromArray(dstAddr, src->AsShortSizedArray(), src_offset, sz);
    } else if (component_type->IsPrimitiveInt() || component_type->IsPrimitiveFloat()) {
        copyFromArray(dstAddr, src->AsIntArray(), src_offset, sz);
    } else if (component_type->IsPrimitiveLong() || component_type->IsPrimitiveDouble()) {
        copyFromArray(dstAddr, src->AsLongArray(), src_offset, sz);
    } else {
        ThrowIllegalAccessException("not a primitive array");
    }
}
static jboolean Unsafe_getBoolean(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    return obj->GetFieldBoolean(MemberOffset(offset));
}

static void Unsafe_putBoolean(JNIEnv* env, jobject, jobject javaObj, jlong offset, jboolean newValue) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    // JNI must use non transactional mode (SetField8 is non-transactional).
    obj->SetFieldBoolean<false>(MemberOffset(offset), newValue);
}

static jbyte Unsafe_getByte(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    return obj->GetFieldByte(MemberOffset(offset));
}

static void Unsafe_putByte(JNIEnv* env, jobject, jobject javaObj, jlong offset, jbyte newValue) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    // JNI must use non transactional mode.
    obj->SetFieldByte<false>(MemberOffset(offset), newValue);
}

static jchar Unsafe_getChar(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    return obj->GetFieldChar(MemberOffset(offset));
}

static void Unsafe_putChar(JNIEnv* env, jobject, jobject javaObj, jlong offset, jchar newValue) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    // JNI must use non transactional mode.
    obj->SetFieldChar<false>(MemberOffset(offset), newValue);
}

static jshort Unsafe_getShort(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    return obj->GetFieldShort(MemberOffset(offset));
}

static void Unsafe_putShort(JNIEnv* env, jobject, jobject javaObj, jlong offset, jshort newValue) {
    ScopedFastNativeObjectAccess soa(env);
    mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
    // JNI must use non transactional mode.
    obj->SetFieldShort<false>(MemberOffset(offset), newValue);
}

static jfloat Unsafe_getFloat(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  union {int32_t val; jfloat converted;} conv;
  conv.val = obj->GetField32(MemberOffset(offset));
  return conv.converted;
}

static void Unsafe_putFloat(JNIEnv* env, jobject, jobject javaObj, jlong offset, jfloat newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  union {int32_t converted; jfloat val;} conv;
  conv.val = newValue;
  // JNI must use non transactional mode.
  obj->SetField32<false>(MemberOffset(offset), conv.converted);
}

static jdouble Unsafe_getDouble(JNIEnv* env, jobject, jobject javaObj, jlong offset) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  union {int64_t val; jdouble converted;} conv;
  conv.val = obj->GetField64(MemberOffset(offset));
  return conv.converted;
}

static void Unsafe_putDouble(JNIEnv* env, jobject, jobject javaObj, jlong offset, jdouble newValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* obj = soa.Decode<mirror::Object*>(javaObj);
  union {int64_t converted; jdouble val;} conv;
  conv.val = newValue;
  // JNI must use non transactional mode.
  obj->SetField64<false>(MemberOffset(offset), conv.converted);
}

static void Unsafe_loadFence(JNIEnv*, jobject) {
  std::atomic_thread_fence(std::memory_order_acquire);
}

static void Unsafe_storeFence(JNIEnv*, jobject) {
  std::atomic_thread_fence(std::memory_order_release);
}

static void Unsafe_fullFence(JNIEnv*, jobject) {
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Unsafe, compareAndSwapInt, "!(Ljava/lang/Object;JII)Z"),
  NATIVE_METHOD(Unsafe, compareAndSwapLong, "!(Ljava/lang/Object;JJJ)Z"),
  NATIVE_METHOD(Unsafe, compareAndSwapObject, "!(Ljava/lang/Object;JLjava/lang/Object;Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Unsafe, getIntVolatile, "!(Ljava/lang/Object;J)I"),
  NATIVE_METHOD(Unsafe, putIntVolatile, "!(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, getLongVolatile, "!(Ljava/lang/Object;J)J"),
  NATIVE_METHOD(Unsafe, putLongVolatile, "!(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, getObjectVolatile, "!(Ljava/lang/Object;J)Ljava/lang/Object;"),
  NATIVE_METHOD(Unsafe, putObjectVolatile, "!(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, getInt, "!(Ljava/lang/Object;J)I"),
  NATIVE_METHOD(Unsafe, putInt, "!(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, putOrderedInt, "!(Ljava/lang/Object;JI)V"),
  NATIVE_METHOD(Unsafe, getLong, "!(Ljava/lang/Object;J)J"),
  NATIVE_METHOD(Unsafe, putLong, "!(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, putOrderedLong, "!(Ljava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, getObject, "!(Ljava/lang/Object;J)Ljava/lang/Object;"),
  NATIVE_METHOD(Unsafe, putObject, "!(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, putOrderedObject, "!(Ljava/lang/Object;JLjava/lang/Object;)V"),
  NATIVE_METHOD(Unsafe, getArrayBaseOffsetForComponentType, "!(Ljava/lang/Class;)I"),
  NATIVE_METHOD(Unsafe, getArrayIndexScaleForComponentType, "!(Ljava/lang/Class;)I"),
  NATIVE_METHOD(Unsafe, addressSize, "!()I"),
  NATIVE_METHOD(Unsafe, pageSize, "!()I"),
  NATIVE_METHOD(Unsafe, allocateMemory, "!(J)J"),
  NATIVE_METHOD(Unsafe, freeMemory, "!(J)V"),
  NATIVE_METHOD(Unsafe, setMemory, "!(JJB)V"),
  NATIVE_METHOD(Unsafe, copyMemory, "!(JJJ)V"),
  NATIVE_METHOD(Unsafe, copyMemoryToPrimitiveArray, "!(JLjava/lang/Object;JJ)V"),
  NATIVE_METHOD(Unsafe, copyMemoryFromPrimitiveArray, "!(Ljava/lang/Object;JJJ)V"),
  NATIVE_METHOD(Unsafe, getBoolean, "!(Ljava/lang/Object;J)Z"),

  NATIVE_METHOD(Unsafe, getByte, "!(Ljava/lang/Object;J)B"),
  NATIVE_METHOD(Unsafe, getChar, "!(Ljava/lang/Object;J)C"),
  NATIVE_METHOD(Unsafe, getShort, "!(Ljava/lang/Object;J)S"),
  NATIVE_METHOD(Unsafe, getFloat, "!(Ljava/lang/Object;J)F"),
  NATIVE_METHOD(Unsafe, getDouble, "!(Ljava/lang/Object;J)D"),
  NATIVE_METHOD(Unsafe, putBoolean, "!(Ljava/lang/Object;JZ)V"),
  NATIVE_METHOD(Unsafe, putByte, "!(Ljava/lang/Object;JB)V"),
  NATIVE_METHOD(Unsafe, putChar, "!(Ljava/lang/Object;JC)V"),
  NATIVE_METHOD(Unsafe, putShort, "!(Ljava/lang/Object;JS)V"),
  NATIVE_METHOD(Unsafe, putFloat, "!(Ljava/lang/Object;JF)V"),
  NATIVE_METHOD(Unsafe, putDouble, "!(Ljava/lang/Object;JD)V"),

  // Each of the getFoo variants are overloaded with a call that operates
  // directively on a native pointer.
  OVERLOADED_NATIVE_METHOD(Unsafe, getByte, "!(J)B", getByteJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, getChar, "!(J)C", getCharJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, getShort, "!(J)S", getShortJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, getInt, "!(J)I", getIntJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, getLong, "!(J)J", getLongJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, getFloat, "!(J)F", getFloatJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, getDouble, "!(J)D", getDoubleJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, putByte, "!(JB)V", putByteJB),
  OVERLOADED_NATIVE_METHOD(Unsafe, putChar, "!(JC)V", putCharJC),
  OVERLOADED_NATIVE_METHOD(Unsafe, putShort, "!(JS)V", putShortJS),
  OVERLOADED_NATIVE_METHOD(Unsafe, putInt, "!(JI)V", putIntJI),
  OVERLOADED_NATIVE_METHOD(Unsafe, putLong, "!(JJ)V", putLongJJ),
  OVERLOADED_NATIVE_METHOD(Unsafe, putFloat, "!(JF)V", putFloatJF),
  OVERLOADED_NATIVE_METHOD(Unsafe, putDouble, "!(JD)V", putDoubleJD),

  // CAS
  NATIVE_METHOD(Unsafe, loadFence, "!()V"),
  NATIVE_METHOD(Unsafe, storeFence, "!()V"),
  NATIVE_METHOD(Unsafe, fullFence, "!()V"),
};

void register_sun_misc_Unsafe(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("sun/misc/Unsafe");
}

}  // namespace art
