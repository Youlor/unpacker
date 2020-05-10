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

#include "java_lang_reflect_Proxy.h"

#include "class_linker.h"
#include "jni_internal.h"
#include "mirror/class_loader.h"
#include "mirror/object_array.h"
#include "mirror/string.h"
#include "scoped_fast_native_object_access.h"
#include "verify_object-inl.h"

namespace art {

static jclass Proxy_generateProxy(JNIEnv* env, jclass, jstring name, jobjectArray interfaces,
                                  jobject loader, jobjectArray methods, jobjectArray throws) {
  ScopedFastNativeObjectAccess soa(env);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  return soa.AddLocalReference<jclass>(class_linker->CreateProxyClass(
      soa, name, interfaces, loader, methods, throws));
}

static JNINativeMethod gMethods[] = {
  NATIVE_METHOD(Proxy, generateProxy, "!(Ljava/lang/String;[Ljava/lang/Class;Ljava/lang/ClassLoader;[Ljava/lang/reflect/Method;[[Ljava/lang/Class;)Ljava/lang/Class;"),
};

void register_java_lang_reflect_Proxy(JNIEnv* env) {
  REGISTER_NATIVE_METHODS("java/lang/reflect/Proxy");
}

}  // namespace art
