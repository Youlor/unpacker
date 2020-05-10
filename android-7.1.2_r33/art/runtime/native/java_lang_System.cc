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

#include "java_lang_System.h"

#include "common_throws.h"
#include "gc/accounting/card_table-inl.h"
#include "jni_internal.h"
#include "mirror/array.h"
#include "mirror/class.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "scoped_fast_native_object_access.h"

namespace art {

/*
 * We make guarantees about the atomicity of accesses to primitive variables.  These guarantees
 * also apply to elements of arrays. In particular, 8-bit, 16-bit, and 32-bit accesses must not
 * cause "word tearing".  Accesses to 64-bit array elements may be two 32-bit operations.
 * References are never torn regardless of the number of bits used to represent them.
 */

static void ThrowArrayStoreException_NotAnArray(const char* identifier, mirror::Object* array)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  std::string actualType(PrettyTypeOf(array));
  Thread* self = Thread::Current();
  self->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                           "%s of type %s is not an array", identifier, actualType.c_str());
}

static void System_arraycopy(JNIEnv* env, jclass, jobject javaSrc, jint srcPos, jobject javaDst,
                             jint dstPos, jint length) {
  // The API is defined in terms of length, but length is somewhat overloaded so we use count.
  const jint count = length;
  ScopedFastNativeObjectAccess soa(env);

  // Null pointer checks.
  if (UNLIKELY(javaSrc == nullptr)) {
    ThrowNullPointerException("src == null");
    return;
  }
  if (UNLIKELY(javaDst == nullptr)) {
    ThrowNullPointerException("dst == null");
    return;
  }

  // Make sure source and destination are both arrays.
  mirror::Object* srcObject = soa.Decode<mirror::Object*>(javaSrc);
  if (UNLIKELY(!srcObject->IsArrayInstance())) {
    ThrowArrayStoreException_NotAnArray("source", srcObject);
    return;
  }
  mirror::Object* dstObject = soa.Decode<mirror::Object*>(javaDst);
  if (UNLIKELY(!dstObject->IsArrayInstance())) {
    ThrowArrayStoreException_NotAnArray("destination", dstObject);
    return;
  }
  mirror::Array* srcArray = srcObject->AsArray();
  mirror::Array* dstArray = dstObject->AsArray();

  // Bounds checking.
  if (UNLIKELY(srcPos < 0) || UNLIKELY(dstPos < 0) || UNLIKELY(count < 0) ||
      UNLIKELY(srcPos > srcArray->GetLength() - count) ||
      UNLIKELY(dstPos > dstArray->GetLength() - count)) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/ArrayIndexOutOfBoundsException;",
                                   "src.length=%d srcPos=%d dst.length=%d dstPos=%d length=%d",
                                   srcArray->GetLength(), srcPos, dstArray->GetLength(), dstPos,
                                   count);
    return;
  }

  mirror::Class* dstComponentType = dstArray->GetClass()->GetComponentType();
  mirror::Class* srcComponentType = srcArray->GetClass()->GetComponentType();
  Primitive::Type dstComponentPrimitiveType = dstComponentType->GetPrimitiveType();

  if (LIKELY(srcComponentType == dstComponentType)) {
    // Trivial assignability.
    switch (dstComponentPrimitiveType) {
      case Primitive::kPrimVoid:
        LOG(FATAL) << "Unreachable, cannot have arrays of type void";
        UNREACHABLE();
      case Primitive::kPrimBoolean:
      case Primitive::kPrimByte:
        DCHECK_EQ(Primitive::ComponentSize(dstComponentPrimitiveType), 1U);
        dstArray->AsByteSizedArray()->Memmove(dstPos, srcArray->AsByteSizedArray(), srcPos, count);
        return;
      case Primitive::kPrimChar:
      case Primitive::kPrimShort:
        DCHECK_EQ(Primitive::ComponentSize(dstComponentPrimitiveType), 2U);
        dstArray->AsShortSizedArray()->Memmove(dstPos, srcArray->AsShortSizedArray(), srcPos, count);
        return;
      case Primitive::kPrimInt:
        DCHECK_EQ(Primitive::ComponentSize(dstComponentPrimitiveType), 4U);
        dstArray->AsIntArray()->Memmove(dstPos, srcArray->AsIntArray(), srcPos, count);
        return;
      case Primitive::kPrimFloat:
        DCHECK_EQ(Primitive::ComponentSize(dstComponentPrimitiveType), 4U);
        dstArray->AsFloatArray()->Memmove(dstPos, srcArray->AsFloatArray(), srcPos, count);
        return;
      case Primitive::kPrimLong:
        DCHECK_EQ(Primitive::ComponentSize(dstComponentPrimitiveType), 8U);
        dstArray->AsLongArray()->Memmove(dstPos, srcArray->AsLongArray(), srcPos, count);
        return;
      case Primitive::kPrimDouble:
        DCHECK_EQ(Primitive::ComponentSize(dstComponentPrimitiveType), 8U);
        dstArray->AsDoubleArray()->Memmove(dstPos, srcArray->AsDoubleArray(), srcPos, count);
        return;
      case Primitive::kPrimNot: {
        mirror::ObjectArray<mirror::Object>* dstObjArray = dstArray->AsObjectArray<mirror::Object>();
        mirror::ObjectArray<mirror::Object>* srcObjArray = srcArray->AsObjectArray<mirror::Object>();
        dstObjArray->AssignableMemmove(dstPos, srcObjArray, srcPos, count);
        return;
      }
      default:
        LOG(FATAL) << "Unknown array type: " << PrettyTypeOf(srcArray);
        UNREACHABLE();
    }
  }
  // If one of the arrays holds a primitive type the other array must hold the exact same type.
  if (UNLIKELY((dstComponentPrimitiveType != Primitive::kPrimNot) ||
               srcComponentType->IsPrimitive())) {
    std::string srcType(PrettyTypeOf(srcArray));
    std::string dstType(PrettyTypeOf(dstArray));
    soa.Self()->ThrowNewExceptionF("Ljava/lang/ArrayStoreException;",
                                   "Incompatible types: src=%s, dst=%s",
                                   srcType.c_str(), dstType.c_str());
    return;
  }
  // Arrays hold distinct types and so therefore can't alias - use memcpy instead of memmove.
  mirror::ObjectArray<mirror::Object>* dstObjArray = dstArray->AsObjectArray<mirror::Object>();
  mirror::ObjectArray<mirror::Object>* srcObjArray = srcArray->AsObjectArray<mirror::Object>();
  // If we're assigning into say Object[] then we don't need per element checks.
  if (dstComponentType->IsAssignableFrom(srcComponentType)) {
    dstObjArray->AssignableMemcpy(dstPos, srcObjArray, srcPos, count);
    return;
  }
  // This code is never run under a transaction.
  DCHECK(!Runtime::Current()->IsActiveTransaction());
  dstObjArray->AssignableCheckingMemcpy<false>(dstPos, srcObjArray, srcPos, count, true);
}

// Template to convert general array to that of its specific primitive type.
template <typename T>
inline T* AsPrimitiveArray(mirror::Array* array) {
  return down_cast<T*>(array);
}

template <typename T, Primitive::Type kPrimType>
inline void System_arraycopyTUnchecked(JNIEnv* env, jobject javaSrc, jint srcPos,
                                       jobject javaDst, jint dstPos, jint count) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* srcObject = soa.Decode<mirror::Object*>(javaSrc);
  mirror::Object* dstObject = soa.Decode<mirror::Object*>(javaDst);
  DCHECK(dstObject != nullptr);
  mirror::Array* srcArray = srcObject->AsArray();
  mirror::Array* dstArray = dstObject->AsArray();
  DCHECK_GE(count, 0);
  DCHECK_EQ(srcArray->GetClass(), dstArray->GetClass());
  DCHECK_EQ(srcArray->GetClass()->GetComponentType()->GetPrimitiveType(), kPrimType);
  AsPrimitiveArray<T>(dstArray)->Memmove(dstPos, AsPrimitiveArray<T>(srcArray), srcPos, count);
}

static void System_arraycopyCharUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                          jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::CharArray, Primitive::kPrimChar>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyByteUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                          jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::ByteArray, Primitive::kPrimByte>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyShortUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                           jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::ShortArray, Primitive::kPrimShort>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyIntUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                         jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::IntArray, Primitive::kPrimInt>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyLongUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                          jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::LongArray, Primitive::kPrimLong>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyFloatUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                           jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::FloatArray, Primitive::kPrimFloat>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyDoubleUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                            jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::DoubleArray, Primitive::kPrimDouble>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static void System_arraycopyBooleanUnchecked(JNIEnv* env, jclass, jobject javaSrc, jint srcPos,
                                             jobject javaDst, jint dstPos, jint count) {
  System_arraycopyTUnchecked<mirror::BooleanArray, Primitive::kPrimBoolean>(env, javaSrc, srcPos,
      javaDst, dstPos, count);
}

static jint System_identityHashCode(JNIEnv* env, jclass, jobject javaObject) {
  if (UNLIKELY(javaObject == nullptr)) {
    return 0;
  }
  ScopedFastNativeObjectAccess soa(env);
  mirror::Object* o = soa.Decode<mirror::Object*>(javaObject);
  return static_cast<jint>(o->IdentityHashCode());
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(System, arraycopy, "!(Ljava/lang/Object;ILjava/lang/Object;II)V"),
  NATIVE_METHOD(System, arraycopyCharUnchecked, "!([CI[CII)V"),
  NATIVE_METHOD(System, arraycopyByteUnchecked, "!([BI[BII)V"),
  NATIVE_METHOD(System, arraycopyShortUnchecked, "!([SI[SII)V"),
  NATIVE_METHOD(System, arraycopyIntUnchecked, "!([II[III)V"),
  NATIVE_METHOD(System, arraycopyLongUnchecked, "!([JI[JII)V"),
  NATIVE_METHOD(System, arraycopyFloatUnchecked, "!([FI[FII)V"),
  NATIVE_METHOD(System, arraycopyDoubleUnchecked, "!([DI[DII)V"),
  NATIVE_METHOD(System, arraycopyBooleanUnchecked, "!([ZI[ZII)V"),
  NATIVE_METHOD(System, identityHashCode, "!(Ljava/lang/Object;)I"),
};

void register_java_lang_System(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/System");
}

}  // namespace art
