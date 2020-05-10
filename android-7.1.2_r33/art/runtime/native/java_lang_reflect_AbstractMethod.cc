/*
 * Copyright (C) 2016 The Android Open Source Project
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

#include "java_lang_reflect_AbstractMethod.h"

#include "art_method-inl.h"
#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "reflection.h"
#include "scoped_fast_native_object_access.h"
#include "well_known_classes.h"

namespace art {

static jobjectArray AbstractMethod_getDeclaredAnnotations(JNIEnv* env, jobject javaMethod) {
  ScopedFastNativeObjectAccess soa(env);
  ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
  if (method->GetDeclaringClass()->IsProxyClass()) {
    // Return an empty array instead of a null pointer.
    mirror::Class* annotation_array_class =
        soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_annotation_Annotation__array);
    mirror::ObjectArray<mirror::Object>* empty_array =
        mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), annotation_array_class, 0);
    return soa.AddLocalReference<jobjectArray>(empty_array);
  }
  return soa.AddLocalReference<jobjectArray>(method->GetDexFile()->GetAnnotationsForMethod(method));
}

static jobjectArray AbstractMethod_getSignatureAnnotation(JNIEnv* env, jobject javaMethod) {
  ScopedFastNativeObjectAccess soa(env);
  ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
  if (method->GetDeclaringClass()->IsProxyClass()) {
    return nullptr;
  }
  StackHandleScope<1> hs(soa.Self());
  return soa.AddLocalReference<jobjectArray>(
      method->GetDexFile()->GetSignatureAnnotationForMethod(method));
}


static jboolean AbstractMethod_isAnnotationPresentNative(JNIEnv* env, jobject javaMethod,
                                                         jclass annotationType) {
  ScopedFastNativeObjectAccess soa(env);
  ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
  if (method->GetDeclaringClass()->IsProxyClass()) {
    return false;
  }
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(soa.Decode<mirror::Class*>(annotationType)));
  return method->GetDexFile()->IsMethodAnnotationPresent(method, klass);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(AbstractMethod, getDeclaredAnnotations, "!()[Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(AbstractMethod, getSignatureAnnotation, "!()[Ljava/lang/String;"),
  NATIVE_METHOD(AbstractMethod, isAnnotationPresentNative, "!(Ljava/lang/Class;)Z"),
};

void register_java_lang_reflect_AbstractMethod(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/AbstractMethod");
}

}  // namespace art
