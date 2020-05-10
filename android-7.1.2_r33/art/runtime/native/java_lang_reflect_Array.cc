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

#include "java_lang_reflect_Array.h"

#include "class_linker-inl.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "scoped_fast_native_object_access.h"
#include "handle_scope-inl.h"

namespace art {

static jobject Array_createMultiArray(
    JNIEnv* env, jclass, jclass javaElementClass, jobject javaDimArray) {
  ScopedFastNativeObjectAccess soa(env);
  DCHECK(javaElementClass != nullptr);
  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::Class> element_class(hs.NewHandle(soa.Decode<mirror::Class*>(javaElementClass)));
  DCHECK(element_class->IsClass());
  DCHECK(javaDimArray != nullptr);
  mirror::Object* dimensions_obj = soa.Decode<mirror::Object*>(javaDimArray);
  DCHECK(dimensions_obj->IsArrayInstance());
  DCHECK_EQ(dimensions_obj->GetClass()->GetComponentType()->GetPrimitiveType(),
            Primitive::kPrimInt);
  Handle<mirror::IntArray> dimensions_array(
      hs.NewHandle(down_cast<mirror::IntArray*>(dimensions_obj)));
  mirror::Array* new_array = mirror::Array::CreateMultiArray(soa.Self(), element_class,
                                                             dimensions_array);
  return soa.AddLocalReference<jobject>(new_array);
}

static jobject Array_createObjectArray(JNIEnv* env, jclass, jclass javaElementClass, jint length) {
  ScopedFastNativeObjectAccess soa(env);
  DCHECK(javaElementClass != nullptr);
  if (UNLIKELY(length < 0)) {
    ThrowNegativeArraySizeException(length);
    return nullptr;
  }
  mirror::Class* element_class = soa.Decode<mirror::Class*>(javaElementClass);
  Runtime* runtime = Runtime::Current();
  ClassLinker* class_linker = runtime->GetClassLinker();
  mirror::Class* array_class = class_linker->FindArrayClass(soa.Self(), &element_class);
  if (UNLIKELY(array_class == nullptr)) {
    CHECK(soa.Self()->IsExceptionPending());
    return nullptr;
  }
  DCHECK(array_class->IsObjectArrayClass());
  mirror::Array* new_array = mirror::ObjectArray<mirror::Object*>::Alloc(
      soa.Self(), array_class, length, runtime->GetHeap()->GetCurrentAllocator());
  return soa.AddLocalReference<jobject>(new_array);
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Array, createMultiArray, "!(Ljava/lang/Class;[I)Ljava/lang/Object;"),
  NATIVE_METHOD(Array, createObjectArray, "!(Ljava/lang/Class;I)Ljava/lang/Object;"),
};

void register_java_lang_reflect_Array(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Array");
}

}  // namespace art
