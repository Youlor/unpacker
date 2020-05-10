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

#include "java_lang_reflect_Field.h"

#include "class_linker.h"
#include "class_linker-inl.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/field.h"
#include "reflection-inl.h"
#include "scoped_fast_native_object_access.h"
#include "utils.h"

namespace art {

template<bool kIsSet>
ALWAYS_INLINE inline static bool VerifyFieldAccess(Thread* self, mirror::Field* field,
                                                   mirror::Object* obj)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (kIsSet && field->IsFinal()) {
    ThrowIllegalAccessException(
            StringPrintf("Cannot set %s field %s of class %s",
                PrettyJavaAccessFlags(field->GetAccessFlags()).c_str(),
                PrettyField(field->GetArtField()).c_str(),
                field->GetDeclaringClass() == nullptr ? "null" :
                    PrettyClass(field->GetDeclaringClass()).c_str()).c_str());
    return false;
  }
  mirror::Class* calling_class = nullptr;
  if (!VerifyAccess(self, obj, field->GetDeclaringClass(), field->GetAccessFlags(),
                    &calling_class, 1)) {
    ThrowIllegalAccessException(
            StringPrintf("Class %s cannot access %s field %s of class %s",
                calling_class == nullptr ? "null" : PrettyClass(calling_class).c_str(),
                PrettyJavaAccessFlags(field->GetAccessFlags()).c_str(),
                PrettyField(field->GetArtField()).c_str(),
                field->GetDeclaringClass() == nullptr ? "null" :
                    PrettyClass(field->GetDeclaringClass()).c_str()).c_str());
    return false;
  }
  return true;
}

template<bool kAllowReferences>
ALWAYS_INLINE inline static bool GetFieldValue(mirror::Object* o, mirror::Field* f,
                                               Primitive::Type field_type, JValue* value)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK_EQ(value->GetJ(), INT64_C(0));
  MemberOffset offset(f->GetOffset());
  const bool is_volatile = f->IsVolatile();
  switch (field_type) {
    case Primitive::kPrimBoolean:
      value->SetZ(is_volatile ? o->GetFieldBooleanVolatile(offset) : o->GetFieldBoolean(offset));
      return true;
    case Primitive::kPrimByte:
      value->SetB(is_volatile ? o->GetFieldByteVolatile(offset) : o->GetFieldByte(offset));
      return true;
    case Primitive::kPrimChar:
      value->SetC(is_volatile ? o->GetFieldCharVolatile(offset) : o->GetFieldChar(offset));
      return true;
    case Primitive::kPrimInt:
    case Primitive::kPrimFloat:
      value->SetI(is_volatile ? o->GetField32Volatile(offset) : o->GetField32(offset));
      return true;
    case Primitive::kPrimLong:
    case Primitive::kPrimDouble:
      value->SetJ(is_volatile ? o->GetField64Volatile(offset) : o->GetField64(offset));
      return true;
    case Primitive::kPrimShort:
      value->SetS(is_volatile ? o->GetFieldShortVolatile(offset) : o->GetFieldShort(offset));
      return true;
    case Primitive::kPrimNot:
      if (kAllowReferences) {
        value->SetL(is_volatile ? o->GetFieldObjectVolatile<mirror::Object>(offset) :
            o->GetFieldObject<mirror::Object>(offset));
        return true;
      }
      // Else break to report an error.
      break;
    case Primitive::kPrimVoid:
      // Never okay.
      break;
  }
  ThrowIllegalArgumentException(
      StringPrintf("Not a primitive field: %s", PrettyField(f->GetArtField()).c_str()).c_str());
  return false;
}

ALWAYS_INLINE inline static bool CheckReceiver(const ScopedFastNativeObjectAccess& soa,
                                               jobject j_rcvr, mirror::Field** f,
                                               mirror::Object** class_or_rcvr)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  soa.Self()->AssertThreadSuspensionIsAllowable();
  mirror::Class* declaringClass = (*f)->GetDeclaringClass();
  if ((*f)->IsStatic()) {
    if (UNLIKELY(!declaringClass->IsInitialized())) {
      StackHandleScope<2> hs(soa.Self());
      HandleWrapper<mirror::Field> h_f(hs.NewHandleWrapper(f));
      HandleWrapper<mirror::Class> h_klass(hs.NewHandleWrapper(&declaringClass));
      ClassLinker* const class_linker = Runtime::Current()->GetClassLinker();
      if (UNLIKELY(!class_linker->EnsureInitialized(soa.Self(), h_klass, true, true))) {
        DCHECK(soa.Self()->IsExceptionPending());
        return false;
      }
    }
    *class_or_rcvr = declaringClass;
    return true;
  }
  *class_or_rcvr = soa.Decode<mirror::Object*>(j_rcvr);
  if (!VerifyObjectIsClass(*class_or_rcvr, declaringClass)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return false;
  }
  return true;
}

static jobject Field_get(JNIEnv* env, jobject javaField, jobject javaObj) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Field* f = soa.Decode<mirror::Field*>(javaField);
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, &f, &o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }
  // If field is not set to be accessible, verify it can be accessed by the caller.
  if (!f->IsAccessible() && !VerifyFieldAccess<false>(soa.Self(), f, o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }
  // We now don't expect suspension unless an exception is thrown.
  // Get the field's value, boxing if necessary.
  Primitive::Type field_type = f->GetTypeAsPrimitiveType();
  JValue value;
  if (!GetFieldValue<true>(o, f, field_type, &value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(BoxPrimitive(field_type, value));
}

template<Primitive::Type kPrimitiveType>
ALWAYS_INLINE inline static JValue GetPrimitiveField(JNIEnv* env, jobject javaField,
                                                     jobject javaObj) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Field* f = soa.Decode<mirror::Field*>(javaField);
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, &f, &o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return JValue();
  }

  // If field is not set to be accessible, verify it can be accessed by the caller.
  if (!f->IsAccessible() && !VerifyFieldAccess<false>(soa.Self(), f, o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return JValue();
  }

  // We now don't expect suspension unless an exception is thrown.
  // Read the value.
  Primitive::Type field_type = f->GetTypeAsPrimitiveType();
  JValue field_value;
  if (field_type == kPrimitiveType) {
    // This if statement should get optimized out since we only pass in valid primitive types.
    if (UNLIKELY(!GetFieldValue<false>(o, f, kPrimitiveType, &field_value))) {
      DCHECK(soa.Self()->IsExceptionPending());
      return JValue();
    }
    return field_value;
  }
  if (!GetFieldValue<false>(o, f, field_type, &field_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return JValue();
  }
  // Widen it if necessary (and possible).
  JValue wide_value;
  if (!ConvertPrimitiveValue(false, field_type, kPrimitiveType, field_value,
                             &wide_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return JValue();
  }
  return wide_value;
}

static jboolean Field_getBoolean(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimBoolean>(env, javaField, javaObj).GetZ();
}

static jbyte Field_getByte(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimByte>(env, javaField, javaObj).GetB();
}

static jchar Field_getChar(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimChar>(env, javaField, javaObj).GetC();
}

static jdouble Field_getDouble(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimDouble>(env, javaField, javaObj).GetD();
}

static jfloat Field_getFloat(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimFloat>(env, javaField, javaObj).GetF();
}

static jint Field_getInt(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimInt>(env, javaField, javaObj).GetI();
}

static jlong Field_getLong(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimLong>(env, javaField, javaObj).GetJ();
}

static jshort Field_getShort(JNIEnv* env, jobject javaField, jobject javaObj) {
  return GetPrimitiveField<Primitive::kPrimShort>(env, javaField, javaObj).GetS();
}

ALWAYS_INLINE inline static void SetFieldValue(mirror::Object* o, mirror::Field* f,
                                               Primitive::Type field_type, bool allow_references,
                                               const JValue& new_value)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK(f->GetDeclaringClass()->IsInitialized());
  MemberOffset offset(f->GetOffset());
  const bool is_volatile = f->IsVolatile();
  switch (field_type) {
  case Primitive::kPrimBoolean:
    if (is_volatile) {
      o->SetFieldBooleanVolatile<false>(offset, new_value.GetZ());
    } else {
      o->SetFieldBoolean<false>(offset, new_value.GetZ());
    }
    break;
  case Primitive::kPrimByte:
    if (is_volatile) {
      o->SetFieldBooleanVolatile<false>(offset, new_value.GetB());
    } else {
      o->SetFieldBoolean<false>(offset, new_value.GetB());
    }
    break;
  case Primitive::kPrimChar:
    if (is_volatile) {
      o->SetFieldCharVolatile<false>(offset, new_value.GetC());
    } else {
      o->SetFieldChar<false>(offset, new_value.GetC());
    }
    break;
  case Primitive::kPrimInt:
  case Primitive::kPrimFloat:
    if (is_volatile) {
      o->SetField32Volatile<false>(offset, new_value.GetI());
    } else {
      o->SetField32<false>(offset, new_value.GetI());
    }
    break;
  case Primitive::kPrimLong:
  case Primitive::kPrimDouble:
    if (is_volatile) {
      o->SetField64Volatile<false>(offset, new_value.GetJ());
    } else {
      o->SetField64<false>(offset, new_value.GetJ());
    }
    break;
  case Primitive::kPrimShort:
    if (is_volatile) {
      o->SetFieldShortVolatile<false>(offset, new_value.GetS());
    } else {
      o->SetFieldShort<false>(offset, new_value.GetS());
    }
    break;
  case Primitive::kPrimNot:
    if (allow_references) {
      if (is_volatile) {
        o->SetFieldObjectVolatile<false>(offset, new_value.GetL());
      } else {
        o->SetFieldObject<false>(offset, new_value.GetL());
      }
      break;
    }
    // Else fall through to report an error.
    FALLTHROUGH_INTENDED;
  case Primitive::kPrimVoid:
    // Never okay.
    ThrowIllegalArgumentException(StringPrintf("Not a primitive field: %s",
                                               PrettyField(f->GetArtField()).c_str()).c_str());
    return;
  }
}

static void Field_set(JNIEnv* env, jobject javaField, jobject javaObj, jobject javaValue) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Field* f = soa.Decode<mirror::Field*>(javaField);
  // Check that the receiver is non-null and an instance of the field's declaring class.
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, &f, &o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }
  mirror::Class* field_type;
  const char* field_type_desciptor = f->GetArtField()->GetTypeDescriptor();
  Primitive::Type field_prim_type = Primitive::GetType(field_type_desciptor[0]);
  if (field_prim_type == Primitive::kPrimNot) {
    field_type = f->GetType();
    DCHECK(field_type != nullptr);
  } else {
    field_type = Runtime::Current()->GetClassLinker()->FindPrimitiveClass(field_type_desciptor[0]);
  }
  // We now don't expect suspension unless an exception is thrown.
  // Unbox the value, if necessary.
  mirror::Object* boxed_value = soa.Decode<mirror::Object*>(javaValue);
  JValue unboxed_value;
  if (!UnboxPrimitiveForField(boxed_value, field_type, f->GetArtField(), &unboxed_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }
  // If field is not set to be accessible, verify it can be accessed by the caller.
  if (!f->IsAccessible() && !VerifyFieldAccess<true>(soa.Self(), f, o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }
  SetFieldValue(o, f, field_prim_type, true, unboxed_value);
}

template<Primitive::Type kPrimitiveType>
static void SetPrimitiveField(JNIEnv* env, jobject javaField, jobject javaObj,
                              const JValue& new_value) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Field* f = soa.Decode<mirror::Field*>(javaField);
  mirror::Object* o = nullptr;
  if (!CheckReceiver(soa, javaObj, &f, &o)) {
    return;
  }
  Primitive::Type field_type = f->GetTypeAsPrimitiveType();
  if (UNLIKELY(field_type == Primitive::kPrimNot)) {
    ThrowIllegalArgumentException(StringPrintf("Not a primitive field: %s",
                                               PrettyField(f->GetArtField()).c_str()).c_str());
    return;
  }

  // Widen the value if necessary (and possible).
  JValue wide_value;
  if (!ConvertPrimitiveValue(false, kPrimitiveType, field_type, new_value, &wide_value)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }

  // If field is not set to be accessible, verify it can be accessed by the caller.
  if (!f->IsAccessible() && !VerifyFieldAccess<true>(soa.Self(), f, o)) {
    DCHECK(soa.Self()->IsExceptionPending());
    return;
  }

  // Write the value.
  SetFieldValue(o, f, field_type, false, wide_value);
}

static void Field_setBoolean(JNIEnv* env, jobject javaField, jobject javaObj, jboolean z) {
  JValue value;
  value.SetZ(z);
  SetPrimitiveField<Primitive::kPrimBoolean>(env, javaField, javaObj, value);
}

static void Field_setByte(JNIEnv* env, jobject javaField, jobject javaObj, jbyte b) {
  JValue value;
  value.SetB(b);
  SetPrimitiveField<Primitive::kPrimByte>(env, javaField, javaObj, value);
}

static void Field_setChar(JNIEnv* env, jobject javaField, jobject javaObj, jchar c) {
  JValue value;
  value.SetC(c);
  SetPrimitiveField<Primitive::kPrimChar>(env, javaField, javaObj, value);
}

static void Field_setDouble(JNIEnv* env, jobject javaField, jobject javaObj, jdouble d) {
  JValue value;
  value.SetD(d);
  SetPrimitiveField<Primitive::kPrimDouble>(env, javaField, javaObj, value);
}

static void Field_setFloat(JNIEnv* env, jobject javaField, jobject javaObj, jfloat f) {
  JValue value;
  value.SetF(f);
  SetPrimitiveField<Primitive::kPrimFloat>(env, javaField, javaObj, value);
}

static void Field_setInt(JNIEnv* env, jobject javaField, jobject javaObj, jint i) {
  JValue value;
  value.SetI(i);
  SetPrimitiveField<Primitive::kPrimInt>(env, javaField, javaObj, value);
}

static void Field_setLong(JNIEnv* env, jobject javaField, jobject javaObj, jlong j) {
  JValue value;
  value.SetJ(j);
  SetPrimitiveField<Primitive::kPrimLong>(env, javaField, javaObj, value);
}

static void Field_setShort(JNIEnv* env, jobject javaField, jobject javaObj, jshort s) {
  JValue value;
  value.SetS(s);
  SetPrimitiveField<Primitive::kPrimShort>(env, javaField, javaObj, value);
}

static jobject Field_getAnnotationNative(JNIEnv* env, jobject javaField, jclass annotationType) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  ArtField* field = soa.Decode<mirror::Field*>(javaField)->GetArtField();
  if (field->GetDeclaringClass()->IsProxyClass()) {
    return nullptr;
  }
  Handle<mirror::Class> klass(hs.NewHandle(soa.Decode<mirror::Class*>(annotationType)));
  return soa.AddLocalReference<jobject>(field->GetDexFile()->GetAnnotationForField(field, klass));
}

static jobjectArray Field_getDeclaredAnnotations(JNIEnv* env, jobject javaField) {
  ScopedFastNativeObjectAccess soa(env);
  ArtField* field = soa.Decode<mirror::Field*>(javaField)->GetArtField();
  if (field->GetDeclaringClass()->IsProxyClass()) {
    // Return an empty array instead of a null pointer.
    mirror::Class* annotation_array_class =
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_annotation_Annotation__array);
    mirror::ObjectArray<mirror::Object>* empty_array =
        mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), annotation_array_class, 0);
    return soa.AddLocalReference<jobjectArray>(empty_array);
  }
  return soa.AddLocalReference<jobjectArray>(field->GetDexFile()->GetAnnotationsForField(field));
}

static jobjectArray Field_getSignatureAnnotation(JNIEnv* env, jobject javaField) {
  ScopedFastNativeObjectAccess soa(env);
  ArtField* field = soa.Decode<mirror::Field*>(javaField)->GetArtField();
  if (field->GetDeclaringClass()->IsProxyClass()) {
    return nullptr;
  }
  return soa.AddLocalReference<jobjectArray>(
      field->GetDexFile()->GetSignatureAnnotationForField(field));
}

static jboolean Field_isAnnotationPresentNative(JNIEnv* env, jobject javaField,
                                                jclass annotationType) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  ArtField* field = soa.Decode<mirror::Field*>(javaField)->GetArtField();
  if (field->GetDeclaringClass()->IsProxyClass()) {
    return false;
  }
  Handle<mirror::Class> klass(hs.NewHandle(soa.Decode<mirror::Class*>(annotationType)));
  return field->GetDexFile()->IsFieldAnnotationPresent(field, klass);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Field, get,        "!(Ljava/lang/Object;)Ljava/lang/Object;"),
  NATIVE_METHOD(Field, getBoolean, "!(Ljava/lang/Object;)Z"),
  NATIVE_METHOD(Field, getByte,    "!(Ljava/lang/Object;)B"),
  NATIVE_METHOD(Field, getChar,    "!(Ljava/lang/Object;)C"),
  NATIVE_METHOD(Field, getAnnotationNative,
                "!(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Field, getDeclaredAnnotations, "!()[Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Field, getSignatureAnnotation, "!()[Ljava/lang/String;"),
  NATIVE_METHOD(Field, getDouble,  "!(Ljava/lang/Object;)D"),
  NATIVE_METHOD(Field, getFloat,   "!(Ljava/lang/Object;)F"),
  NATIVE_METHOD(Field, getInt,     "!(Ljava/lang/Object;)I"),
  NATIVE_METHOD(Field, getLong,    "!(Ljava/lang/Object;)J"),
  NATIVE_METHOD(Field, getShort,   "!(Ljava/lang/Object;)S"),
  NATIVE_METHOD(Field, isAnnotationPresentNative, "!(Ljava/lang/Class;)Z"),
  NATIVE_METHOD(Field, set,        "!(Ljava/lang/Object;Ljava/lang/Object;)V"),
  NATIVE_METHOD(Field, setBoolean, "!(Ljava/lang/Object;Z)V"),
  NATIVE_METHOD(Field, setByte,    "!(Ljava/lang/Object;B)V"),
  NATIVE_METHOD(Field, setChar,    "!(Ljava/lang/Object;C)V"),
  NATIVE_METHOD(Field, setDouble,  "!(Ljava/lang/Object;D)V"),
  NATIVE_METHOD(Field, setFloat,   "!(Ljava/lang/Object;F)V"),
  NATIVE_METHOD(Field, setInt,     "!(Ljava/lang/Object;I)V"),
  NATIVE_METHOD(Field, setLong,    "!(Ljava/lang/Object;J)V"),
  NATIVE_METHOD(Field, setShort,   "!(Ljava/lang/Object;S)V"),
};

void register_java_lang_reflect_Field(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Field");
}

}  // namespace art
