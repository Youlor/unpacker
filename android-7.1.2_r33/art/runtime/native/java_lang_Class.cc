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

#include "java_lang_Class.h"

#include <iostream>

#include "art_field-inl.h"
#include "class_linker.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "nth_caller_visitor.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/field-inl.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string-inl.h"
#include "reflection.h"
#include "scoped_thread_state_change.h"
#include "scoped_fast_native_object_access.h"
#include "ScopedLocalRef.h"
#include "ScopedUtfChars.h"
#include "utf.h"
#include "well_known_classes.h"

namespace art {

ALWAYS_INLINE static inline mirror::Class* DecodeClass(
    const ScopedFastNativeObjectAccess& soa, jobject java_class)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  mirror::Class* c = soa.Decode<mirror::Class*>(java_class);
  DCHECK(c != nullptr);
  DCHECK(c->IsClass());
  // TODO: we could EnsureInitialized here, rather than on every reflective get/set or invoke .
  // For now, we conservatively preserve the old dalvik behavior. A quick "IsInitialized" check
  // every time probably doesn't make much difference to reflection performance anyway.
  return c;
}

// "name" is in "binary name" format, e.g. "dalvik.system.Debug$1".
static jclass Class_classForName(JNIEnv* env, jclass, jstring javaName, jboolean initialize,
                                 jobject javaLoader) {
  ScopedFastNativeObjectAccess soa(env);
  ScopedUtfChars name(env, javaName);
  if (name.c_str() == nullptr) {
    return nullptr;
  }

  // We need to validate and convert the name (from x.y.z to x/y/z).  This
  // is especially handy for array types, since we want to avoid
  // auto-generating bogus array classes.
  if (!IsValidBinaryClassName(name.c_str())) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/ClassNotFoundException;",
                                   "Invalid name: %s", name.c_str());
    return nullptr;
  }

  std::string descriptor(DotToDescriptor(name.c_str()));
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(soa.Decode<mirror::ClassLoader*>(javaLoader)));
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> c(
      hs.NewHandle(class_linker->FindClass(soa.Self(), descriptor.c_str(), class_loader)));
  if (c.Get() == nullptr) {
    ScopedLocalRef<jthrowable> cause(env, env->ExceptionOccurred());
    env->ExceptionClear();
    jthrowable cnfe = reinterpret_cast<jthrowable>(env->NewObject(WellKnownClasses::java_lang_ClassNotFoundException,
                                                                  WellKnownClasses::java_lang_ClassNotFoundException_init,
                                                                  javaName, cause.get()));
    if (cnfe != nullptr) {
      // Make sure allocation didn't fail with an OOME.
      env->Throw(cnfe);
    }
    return nullptr;
  }
  if (initialize) {
    class_linker->EnsureInitialized(soa.Self(), c, true, true);
  }
  return soa.AddLocalReference<jclass>(c.Get());
}

static jstring Class_getNameNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  mirror::Class* const c = DecodeClass(soa, javaThis);
  return soa.AddLocalReference<jstring>(mirror::Class::ComputeName(hs.NewHandle(c)));
}

static jobjectArray Class_getProxyInterfaces(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Class* c = DecodeClass(soa, javaThis);
  return soa.AddLocalReference<jobjectArray>(c->GetInterfaces()->Clone(soa.Self()));
}

static mirror::ObjectArray<mirror::Field>* GetDeclaredFields(
    Thread* self, mirror::Class* klass, bool public_only, bool force_resolve)
      SHARED_REQUIRES(Locks::mutator_lock_) {
  StackHandleScope<1> hs(self);
  IterationRange<StrideIterator<ArtField>> ifields = klass->GetIFields();
  IterationRange<StrideIterator<ArtField>> sfields = klass->GetSFields();
  size_t array_size = klass->NumInstanceFields() + klass->NumStaticFields();
  if (public_only) {
    // Lets go subtract all the non public fields.
    for (ArtField& field : ifields) {
      if (!field.IsPublic()) {
        --array_size;
      }
    }
    for (ArtField& field : sfields) {
      if (!field.IsPublic()) {
        --array_size;
      }
    }
  }
  size_t array_idx = 0;
  auto object_array = hs.NewHandle(mirror::ObjectArray<mirror::Field>::Alloc(
      self, mirror::Field::ArrayClass(), array_size));
  if (object_array.Get() == nullptr) {
    return nullptr;
  }
  for (ArtField& field : ifields) {
    if (!public_only || field.IsPublic()) {
      auto* reflect_field = mirror::Field::CreateFromArtField(self, &field, force_resolve);
      if (reflect_field == nullptr) {
        if (kIsDebugBuild) {
          self->AssertPendingException();
        }
        // Maybe null due to OOME or type resolving exception.
        return nullptr;
      }
      object_array->SetWithoutChecks<false>(array_idx++, reflect_field);
    }
  }
  for (ArtField& field : sfields) {
    if (!public_only || field.IsPublic()) {
      auto* reflect_field = mirror::Field::CreateFromArtField(self, &field, force_resolve);
      if (reflect_field == nullptr) {
        if (kIsDebugBuild) {
          self->AssertPendingException();
        }
        return nullptr;
      }
      object_array->SetWithoutChecks<false>(array_idx++, reflect_field);
    }
  }
  DCHECK_EQ(array_idx, array_size);
  return object_array.Get();
}

static jobjectArray Class_getDeclaredFieldsUnchecked(JNIEnv* env, jobject javaThis,
                                                     jboolean publicOnly) {
  ScopedFastNativeObjectAccess soa(env);
  return soa.AddLocalReference<jobjectArray>(
      GetDeclaredFields(soa.Self(), DecodeClass(soa, javaThis), publicOnly != JNI_FALSE, false));
}

static jobjectArray Class_getDeclaredFields(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  return soa.AddLocalReference<jobjectArray>(
      GetDeclaredFields(soa.Self(), DecodeClass(soa, javaThis), false, true));
}

static jobjectArray Class_getPublicDeclaredFields(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  return soa.AddLocalReference<jobjectArray>(
      GetDeclaredFields(soa.Self(), DecodeClass(soa, javaThis), true, true));
}

// Performs a binary search through an array of fields, TODO: Is this fast enough if we don't use
// the dex cache for lookups? I think CompareModifiedUtf8ToUtf16AsCodePointValues should be fairly
// fast.
ALWAYS_INLINE static inline ArtField* FindFieldByName(
    Thread* self ATTRIBUTE_UNUSED, mirror::String* name, LengthPrefixedArray<ArtField>* fields)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (fields == nullptr) {
    return nullptr;
  }
  size_t low = 0;
  size_t high = fields->size();
  const uint16_t* const data = name->GetValue();
  const size_t length = name->GetLength();
  while (low < high) {
    auto mid = (low + high) / 2;
    ArtField& field = fields->At(mid);
    int result = CompareModifiedUtf8ToUtf16AsCodePointValues(field.GetName(), data, length);
    // Alternate approach, only a few % faster at the cost of more allocations.
    // int result = field->GetStringName(self, true)->CompareTo(name);
    if (result < 0) {
      low = mid + 1;
    } else if (result > 0) {
      high = mid;
    } else {
      return &field;
    }
  }
  if (kIsDebugBuild) {
    for (ArtField& field : MakeIterationRangeFromLengthPrefixedArray(fields)) {
      CHECK_NE(field.GetName(), name->ToModifiedUtf8());
    }
  }
  return nullptr;
}

ALWAYS_INLINE static inline mirror::Field* GetDeclaredField(
    Thread* self, mirror::Class* c, mirror::String* name)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtField* art_field = FindFieldByName(self, name, c->GetIFieldsPtr());
  if (art_field != nullptr) {
    return mirror::Field::CreateFromArtField(self, art_field, true);
  }
  art_field = FindFieldByName(self, name, c->GetSFieldsPtr());
  if (art_field != nullptr) {
    return mirror::Field::CreateFromArtField(self, art_field, true);
  }
  return nullptr;
}

static mirror::Field* GetPublicFieldRecursive(
    Thread* self, mirror::Class* clazz, mirror::String* name)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK(clazz != nullptr);
  DCHECK(name != nullptr);
  DCHECK(self != nullptr);

  StackHandleScope<2> hs(self);
  MutableHandle<mirror::Class> h_clazz(hs.NewHandle(clazz));
  Handle<mirror::String> h_name(hs.NewHandle(name));

  // We search the current class, its direct interfaces then its superclass.
  while (h_clazz.Get() != nullptr) {
    mirror::Field* result = GetDeclaredField(self, h_clazz.Get(), h_name.Get());
    if ((result != nullptr) && (result->GetAccessFlags() & kAccPublic)) {
      return result;
    } else if (UNLIKELY(self->IsExceptionPending())) {
      // Something went wrong. Bail out.
      return nullptr;
    }

    uint32_t num_direct_interfaces = h_clazz->NumDirectInterfaces();
    for (uint32_t i = 0; i < num_direct_interfaces; i++) {
      mirror::Class *iface = mirror::Class::GetDirectInterface(self, h_clazz, i);
      if (UNLIKELY(iface == nullptr)) {
        self->AssertPendingException();
        return nullptr;
      }
      result = GetPublicFieldRecursive(self, iface, h_name.Get());
      if (result != nullptr) {
        DCHECK(result->GetAccessFlags() & kAccPublic);
        return result;
      } else if (UNLIKELY(self->IsExceptionPending())) {
        // Something went wrong. Bail out.
        return nullptr;
      }
    }

    // We don't try the superclass if we are an interface.
    if (h_clazz->IsInterface()) {
      break;
    }

    // Get the next class.
    h_clazz.Assign(h_clazz->GetSuperClass());
  }
  return nullptr;
}

static jobject Class_getPublicFieldRecursive(JNIEnv* env, jobject javaThis, jstring name) {
  ScopedFastNativeObjectAccess soa(env);
  auto* name_string = soa.Decode<mirror::String*>(name);
  if (UNLIKELY(name_string == nullptr)) {
    ThrowNullPointerException("name == null");
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(
      GetPublicFieldRecursive(soa.Self(), DecodeClass(soa, javaThis), name_string));
}

static jobject Class_getDeclaredField(JNIEnv* env, jobject javaThis, jstring name) {
  ScopedFastNativeObjectAccess soa(env);
  auto* name_string = soa.Decode<mirror::String*>(name);
  if (name_string == nullptr) {
    ThrowNullPointerException("name == null");
    return nullptr;
  }
  auto* klass = DecodeClass(soa, javaThis);
  mirror::Field* result = GetDeclaredField(soa.Self(), klass, name_string);
  if (result == nullptr) {
    std::string name_str = name_string->ToModifiedUtf8();
    if (name_str == "value" && klass->IsStringClass()) {
      // We log the error for this specific case, as the user might just swallow the exception.
      // This helps diagnose crashes when applications rely on the String#value field being
      // there.
      // Also print on the error stream to test it through run-test.
      std::string message("The String#value field is not present on Android versions >= 6.0");
      LOG(ERROR) << message;
      std::cerr << message << std::endl;
    }
    // We may have a pending exception if we failed to resolve.
    if (!soa.Self()->IsExceptionPending()) {
      ThrowNoSuchFieldException(DecodeClass(soa, javaThis), name_str.c_str());
    }
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(result);
}

static jobject Class_getDeclaredConstructorInternal(
    JNIEnv* env, jobject javaThis, jobjectArray args) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Constructor* result = mirror::Class::GetDeclaredConstructorInternal(
      soa.Self(),
      DecodeClass(soa, javaThis),
      soa.Decode<mirror::ObjectArray<mirror::Class>*>(args));
  return soa.AddLocalReference<jobject>(result);
}

static ALWAYS_INLINE inline bool MethodMatchesConstructor(ArtMethod* m, bool public_only)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK(m != nullptr);
  return (!public_only || m->IsPublic()) && !m->IsStatic() && m->IsConstructor();
}

static jobjectArray Class_getDeclaredConstructorsInternal(
    JNIEnv* env, jobject javaThis, jboolean publicOnly) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> h_klass = hs.NewHandle(DecodeClass(soa, javaThis));
  size_t constructor_count = 0;
  // Two pass approach for speed.
  for (auto& m : h_klass->GetDirectMethods(sizeof(void*))) {
    constructor_count += MethodMatchesConstructor(&m, publicOnly != JNI_FALSE) ? 1u : 0u;
  }
  auto h_constructors = hs.NewHandle(mirror::ObjectArray<mirror::Constructor>::Alloc(
      soa.Self(), mirror::Constructor::ArrayClass(), constructor_count));
  if (UNLIKELY(h_constructors.Get() == nullptr)) {
    soa.Self()->AssertPendingException();
    return nullptr;
  }
  constructor_count = 0;
  for (auto& m : h_klass->GetDirectMethods(sizeof(void*))) {
    if (MethodMatchesConstructor(&m, publicOnly != JNI_FALSE)) {
      auto* constructor = mirror::Constructor::CreateFromArtMethod(soa.Self(), &m);
      if (UNLIKELY(constructor == nullptr)) {
        soa.Self()->AssertPendingOOMException();
        return nullptr;
      }
      h_constructors->SetWithoutChecks<false>(constructor_count++, constructor);
    }
  }
  return soa.AddLocalReference<jobjectArray>(h_constructors.Get());
}

static jobject Class_getDeclaredMethodInternal(JNIEnv* env, jobject javaThis,
                                               jobject name, jobjectArray args) {
  ScopedFastNativeObjectAccess soa(env);
  mirror::Method* result = mirror::Class::GetDeclaredMethodInternal(
      soa.Self(),
      DecodeClass(soa, javaThis),
      soa.Decode<mirror::String*>(name),
      soa.Decode<mirror::ObjectArray<mirror::Class>*>(args));
  return soa.AddLocalReference<jobject>(result);
}

static jobjectArray Class_getDeclaredMethodsUnchecked(JNIEnv* env, jobject javaThis,
                                                      jboolean publicOnly) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> klass = hs.NewHandle(DecodeClass(soa, javaThis));
  size_t num_methods = 0;
  for (auto& m : klass->GetDeclaredMethods(sizeof(void*))) {
    auto modifiers = m.GetAccessFlags();
    // Add non-constructor declared methods.
    if ((publicOnly == JNI_FALSE || (modifiers & kAccPublic) != 0) &&
        (modifiers & kAccConstructor) == 0) {
      ++num_methods;
    }
  }
  auto ret = hs.NewHandle(mirror::ObjectArray<mirror::Method>::Alloc(
      soa.Self(), mirror::Method::ArrayClass(), num_methods));
  num_methods = 0;
  for (auto& m : klass->GetDeclaredMethods(sizeof(void*))) {
    auto modifiers = m.GetAccessFlags();
    if ((publicOnly == JNI_FALSE || (modifiers & kAccPublic) != 0) &&
        (modifiers & kAccConstructor) == 0) {
      auto* method = mirror::Method::CreateFromArtMethod(soa.Self(), &m);
      if (method == nullptr) {
        soa.Self()->AssertPendingException();
        return nullptr;
      }
      ret->SetWithoutChecks<false>(num_methods++, method);
    }
  }
  return soa.AddLocalReference<jobjectArray>(ret.Get());
}

static jobject Class_getDeclaredAnnotation(JNIEnv* env, jobject javaThis, jclass annotationClass) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));

  // Handle public contract to throw NPE if the "annotationClass" argument was null.
  if (UNLIKELY(annotationClass == nullptr)) {
    ThrowNullPointerException("annotationClass");
    return nullptr;
  }

  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return nullptr;
  }
  Handle<mirror::Class> annotation_class(hs.NewHandle(soa.Decode<mirror::Class*>(annotationClass)));
  return soa.AddLocalReference<jobject>(
      klass->GetDexFile().GetAnnotationForClass(klass, annotation_class));
}

static jobjectArray Class_getDeclaredAnnotations(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    // Return an empty array instead of a null pointer.
    mirror::Class* annotation_array_class =
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_annotation_Annotation__array);
    mirror::ObjectArray<mirror::Object>* empty_array =
        mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), annotation_array_class, 0);
    return soa.AddLocalReference<jobjectArray>(empty_array);
  }
  return soa.AddLocalReference<jobjectArray>(klass->GetDexFile().GetAnnotationsForClass(klass));
}

static jobjectArray Class_getDeclaredClasses(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  mirror::ObjectArray<mirror::Class>* classes = nullptr;
  if (!klass->IsProxyClass() && klass->GetDexCache() != nullptr) {
    classes = klass->GetDexFile().GetDeclaredClasses(klass);
  }
  if (classes == nullptr) {
    // Return an empty array instead of a null pointer.
    if (soa.Self()->IsExceptionPending()) {
      // Pending exception from GetDeclaredClasses.
      return nullptr;
    }
    mirror::Class* class_class = mirror::Class::GetJavaLangClass();
    mirror::Class* class_array_class =
        Runtime::Current()->GetClassLinker()->FindArrayClass(soa.Self(), &class_class);
    if (class_array_class == nullptr) {
      return nullptr;
    }
    mirror::ObjectArray<mirror::Class>* empty_array =
        mirror::ObjectArray<mirror::Class>::Alloc(soa.Self(), class_array_class, 0);
    return soa.AddLocalReference<jobjectArray>(empty_array);
  }
  return soa.AddLocalReference<jobjectArray>(classes);
}

static jclass Class_getEnclosingClass(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return nullptr;
  }
  return soa.AddLocalReference<jclass>(klass->GetDexFile().GetEnclosingClass(klass));
}

static jobject Class_getEnclosingConstructorNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return nullptr;
  }
  mirror::Object* method = klass->GetDexFile().GetEnclosingMethod(klass);
  if (method != nullptr) {
    if (method->GetClass() ==
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_reflect_Constructor)) {
      return soa.AddLocalReference<jobject>(method);
    }
  }
  return nullptr;
}

static jobject Class_getEnclosingMethodNative(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return nullptr;
  }
  mirror::Object* method = klass->GetDexFile().GetEnclosingMethod(klass);
  if (method != nullptr) {
    if (method->GetClass() ==
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_reflect_Method)) {
      return soa.AddLocalReference<jobject>(method);
    }
  }
  return nullptr;
}

static jint Class_getInnerClassFlags(JNIEnv* env, jobject javaThis, jint defaultValue) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  return mirror::Class::GetInnerClassFlags(klass, defaultValue);
}

static jstring Class_getInnerClassName(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return nullptr;
  }
  mirror::String* class_name = nullptr;
  if (!klass->GetDexFile().GetInnerClass(klass, &class_name)) {
    return nullptr;
  }
  return soa.AddLocalReference<jstring>(class_name);
}

static jobjectArray Class_getSignatureAnnotation(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return nullptr;
  }
  return soa.AddLocalReference<jobjectArray>(
      klass->GetDexFile().GetSignatureAnnotationForClass(klass));
}

static jboolean Class_isAnonymousClass(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return false;
  }
  mirror::String* class_name = nullptr;
  if (!klass->GetDexFile().GetInnerClass(klass, &class_name)) {
    return false;
  }
  return class_name == nullptr;
}

static jboolean Class_isDeclaredAnnotationPresent(JNIEnv* env, jobject javaThis,
                                                  jclass annotationType) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return false;
  }
  Handle<mirror::Class> annotation_class(hs.NewHandle(soa.Decode<mirror::Class*>(annotationType)));
  return klass->GetDexFile().IsClassAnnotationPresent(klass, annotation_class);
}

static jclass Class_getDeclaringClass(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(DecodeClass(soa, javaThis)));
  if (klass->IsProxyClass() || klass->GetDexCache() == nullptr) {
    return nullptr;
  }
  // Return null for anonymous classes.
  if (Class_isAnonymousClass(env, javaThis)) {
    return nullptr;
  }
  return soa.AddLocalReference<jclass>(klass->GetDexFile().GetDeclaringClass(klass));
}

static jobject Class_newInstance(JNIEnv* env, jobject javaThis) {
  ScopedFastNativeObjectAccess soa(env);
  StackHandleScope<4> hs(soa.Self());
  Handle<mirror::Class> klass = hs.NewHandle(DecodeClass(soa, javaThis));
  if (UNLIKELY(klass->GetPrimitiveType() != 0 || klass->IsInterface() || klass->IsArrayClass() ||
               klass->IsAbstract())) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
                                   "%s cannot be instantiated", PrettyClass(klass.Get()).c_str());
    return nullptr;
  }
  auto caller = hs.NewHandle<mirror::Class>(nullptr);
  // Verify that we can access the class.
  if (!klass->IsPublic()) {
    caller.Assign(GetCallingClass(soa.Self(), 1));
    if (caller.Get() != nullptr && !caller->CanAccess(klass.Get())) {
      soa.Self()->ThrowNewExceptionF(
          "Ljava/lang/IllegalAccessException;", "%s is not accessible from %s",
          PrettyClass(klass.Get()).c_str(), PrettyClass(caller.Get()).c_str());
      return nullptr;
    }
  }
  auto* constructor = klass->GetDeclaredConstructor(
      soa.Self(),
      ScopedNullHandle<mirror::ObjectArray<mirror::Class>>(),
      sizeof(void*));
  if (UNLIKELY(constructor == nullptr)) {
    soa.Self()->ThrowNewExceptionF("Ljava/lang/InstantiationException;",
                                   "%s has no zero argument constructor",
                                   PrettyClass(klass.Get()).c_str());
    return nullptr;
  }
  // Invoke the string allocator to return an empty string for the string class.
  if (klass->IsStringClass()) {
    gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
    mirror::SetStringCountVisitor visitor(0);
    mirror::Object* obj = mirror::String::Alloc<true>(soa.Self(), 0, allocator_type, visitor);
    if (UNLIKELY(soa.Self()->IsExceptionPending())) {
      return nullptr;
    } else {
      return soa.AddLocalReference<jobject>(obj);
    }
  }
  auto receiver = hs.NewHandle(klass->AllocObject(soa.Self()));
  if (UNLIKELY(receiver.Get() == nullptr)) {
    soa.Self()->AssertPendingOOMException();
    return nullptr;
  }
  // Verify that we can access the constructor.
  auto* declaring_class = constructor->GetDeclaringClass();
  if (!constructor->IsPublic()) {
    if (caller.Get() == nullptr) {
      caller.Assign(GetCallingClass(soa.Self(), 1));
    }
    if (UNLIKELY(caller.Get() != nullptr && !VerifyAccess(
        soa.Self(), receiver.Get(), declaring_class, constructor->GetAccessFlags(),
        caller.Get()))) {
      soa.Self()->ThrowNewExceptionF(
          "Ljava/lang/IllegalAccessException;", "%s is not accessible from %s",
          PrettyMethod(constructor).c_str(), PrettyClass(caller.Get()).c_str());
      return nullptr;
    }
  }
  // Ensure that we are initialized.
  if (UNLIKELY(!declaring_class->IsInitialized())) {
    if (!Runtime::Current()->GetClassLinker()->EnsureInitialized(
        soa.Self(), hs.NewHandle(declaring_class), true, true)) {
      soa.Self()->AssertPendingException();
      return nullptr;
    }
  }
  // Invoke the constructor.
  JValue result;
  uint32_t args[1] = { static_cast<uint32_t>(reinterpret_cast<uintptr_t>(receiver.Get())) };
  constructor->Invoke(soa.Self(), args, sizeof(args), &result, "V");
  if (UNLIKELY(soa.Self()->IsExceptionPending())) {
    return nullptr;
  }
  // Constructors are ()V methods, so we shouldn't touch the result of InvokeMethod.
  return soa.AddLocalReference<jobject>(receiver.Get());
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Class, classForName,
                "!(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getDeclaredAnnotation,
                "!(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Class, getDeclaredAnnotations, "!()[Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Class, getDeclaredClasses, "!()[Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getDeclaredConstructorInternal,
                "!([Ljava/lang/Class;)Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getDeclaredConstructorsInternal, "!(Z)[Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getDeclaredField, "!(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getPublicFieldRecursive, "!(Ljava/lang/String;)Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFields, "!()[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredFieldsUnchecked, "!(Z)[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getDeclaredMethodInternal,
                "!(Ljava/lang/String;[Ljava/lang/Class;)Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getDeclaredMethodsUnchecked,
                "!(Z)[Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getDeclaringClass, "!()Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getEnclosingClass, "!()Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getEnclosingConstructorNative, "!()Ljava/lang/reflect/Constructor;"),
  NATIVE_METHOD(Class, getEnclosingMethodNative, "!()Ljava/lang/reflect/Method;"),
  NATIVE_METHOD(Class, getInnerClassFlags, "!(I)I"),
  NATIVE_METHOD(Class, getInnerClassName, "!()Ljava/lang/String;"),
  NATIVE_METHOD(Class, getNameNative, "!()Ljava/lang/String;"),
  NATIVE_METHOD(Class, getProxyInterfaces, "!()[Ljava/lang/Class;"),
  NATIVE_METHOD(Class, getPublicDeclaredFields, "!()[Ljava/lang/reflect/Field;"),
  NATIVE_METHOD(Class, getSignatureAnnotation, "!()[Ljava/lang/String;"),
  NATIVE_METHOD(Class, isAnonymousClass, "!()Z"),
  NATIVE_METHOD(Class, isDeclaredAnnotationPresent, "!(Ljava/lang/Class;)Z"),
  NATIVE_METHOD(Class, newInstance, "!()Ljava/lang/Object;"),
};

void register_java_lang_Class(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/Class");
}

}  // namespace art
