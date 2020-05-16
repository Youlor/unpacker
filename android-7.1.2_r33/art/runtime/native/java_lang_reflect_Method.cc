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

#include "java_lang_reflect_Method.h"

#include "art_method-inl.h"
#include "class_linker.h"
#include "class_linker-inl.h"
#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "reflection.h"
#include "scoped_fast_native_object_access.h"
#include "well_known_classes.h"

namespace art {

static jobject Method_getAnnotationNative(JNIEnv* env, jobject javaMethod, jclass annotationType) {
  ScopedFastNativeObjectAccess soa(env);
  ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
  if (method->GetDeclaringClass()->IsProxyClass()) {
    return nullptr;
  }
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::Class> klass(hs.NewHandle(soa.Decode<mirror::Class*>(annotationType)));
  return soa.AddLocalReference<jobject>(
      method->GetDexFile()->GetAnnotationForMethod(method, klass));
}

static jobject Method_getDefaultValue(JNIEnv* env, jobject javaMethod) {
  ScopedFastNativeObjectAccess soa(env);
  ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
  if (!method->GetDeclaringClass()->IsAnnotation()) {
    return nullptr;
  }
  return soa.AddLocalReference<jobject>(method->GetDexFile()->GetAnnotationDefaultValue(method));
}

static jobjectArray Method_getExceptionTypes(JNIEnv* env, jobject javaMethod) {
  ScopedFastNativeObjectAccess soa(env);
  ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
  if (method->GetDeclaringClass()->IsProxyClass()) {
    mirror::Class* klass = method->GetDeclaringClass();
    int throws_index = -1;
    size_t i = 0;
    for (const auto& m : klass->GetDeclaredVirtualMethods(sizeof(void*))) {
      if (&m == method) {
        throws_index = i;
        break;
      }
      ++i;
    }
    CHECK_NE(throws_index, -1);
    mirror::ObjectArray<mirror::Class>* declared_exceptions = klass->GetThrows()->Get(throws_index);
    return soa.AddLocalReference<jobjectArray>(declared_exceptions->Clone(soa.Self()));
  } else {
    mirror::ObjectArray<mirror::Class>* result_array =
        method->GetDexFile()->GetExceptionTypesForMethod(method);
    if (result_array == nullptr) {
      // Return an empty array instead of a null pointer
      mirror::Class* class_class = mirror::Class::GetJavaLangClass();
      mirror::Class* class_array_class =
          Runtime::Current()->GetClassLinker()->FindArrayClass(soa.Self(), &class_class);
      if (class_array_class == nullptr) {
        return nullptr;
      }
      mirror::ObjectArray<mirror::Class>* empty_array =
          mirror::ObjectArray<mirror::Class>::Alloc(soa.Self(), class_array_class, 0);
      return soa.AddLocalReference<jobjectArray>(empty_array);
    } else {
      return soa.AddLocalReference<jobjectArray>(result_array);
    }
  }
}

static jobjectArray Method_getParameterAnnotationsNative(JNIEnv* env, jobject javaMethod) {
  ScopedFastNativeObjectAccess soa(env);
  ArtMethod* method = ArtMethod::FromReflectedMethod(soa, javaMethod);
  if (method->GetDeclaringClass()->IsProxyClass()) {
    return nullptr;
  }
  return soa.AddLocalReference<jobjectArray>(method->GetDexFile()->GetParameterAnnotations(method));
}

static jobject Method_invoke(JNIEnv* env, jobject javaMethod, jobject javaReceiver,
                             jobject javaArgs) {
  ScopedFastNativeObjectAccess soa(env);
  return InvokeMethod(soa, javaMethod, javaReceiver, javaArgs);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Method, getAnnotationNative,
                "!(Ljava/lang/Class;)Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Method, getDefaultValue, "!()Ljava/lang/Object;"),
  NATIVE_METHOD(Method, getExceptionTypes, "!()[Ljava/lang/Class;"),
  NATIVE_METHOD(Method, getParameterAnnotationsNative, "!()[[Ljava/lang/annotation/Annotation;"),
  NATIVE_METHOD(Method, invoke, "!(Ljava/lang/Object;[Ljava/lang/Object;)Ljava/lang/Object;"),
};

void register_java_lang_reflect_Method(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Method");
}

}  // namespace art
