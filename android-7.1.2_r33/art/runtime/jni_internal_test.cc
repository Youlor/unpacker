/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "jni_internal.h"

#include "art_method-inl.h"
#include "common_compiler_test.h"
#include "indirect_reference_table.h"
#include "java_vm_ext.h"
#include "jni_env_ext.h"
#include "mirror/string-inl.h"
#include "scoped_thread_state_change.h"
#include "ScopedLocalRef.h"

namespace art {

// TODO: Convert to CommonRuntimeTest. Currently MakeExecutable is used.
class JniInternalTest : public CommonCompilerTest {
 protected:
  virtual void SetUp() {
    CommonCompilerTest::SetUp();

    vm_ = Runtime::Current()->GetJavaVM();

    // Turn on -verbose:jni for the JNI tests.
    // gLogVerbosity.jni = true;

    vm_->AttachCurrentThread(&env_, nullptr);

    ScopedLocalRef<jclass> aioobe(env_,
                                  env_->FindClass("java/lang/ArrayIndexOutOfBoundsException"));
    CHECK(aioobe.get() != nullptr);
    aioobe_ = reinterpret_cast<jclass>(env_->NewGlobalRef(aioobe.get()));

    ScopedLocalRef<jclass> ase(env_, env_->FindClass("java/lang/ArrayStoreException"));
    CHECK(ase.get() != nullptr);
    ase_ = reinterpret_cast<jclass>(env_->NewGlobalRef(ase.get()));

    ScopedLocalRef<jclass> sioobe(env_,
                                  env_->FindClass("java/lang/StringIndexOutOfBoundsException"));
    CHECK(sioobe.get() != nullptr);
    sioobe_ = reinterpret_cast<jclass>(env_->NewGlobalRef(sioobe.get()));
  }

  void ExpectException(jclass exception_class) {
    ScopedObjectAccess soa(env_);
    EXPECT_TRUE(env_->ExceptionCheck())
        << PrettyDescriptor(soa.Decode<mirror::Class*>(exception_class));
    jthrowable exception = env_->ExceptionOccurred();
    EXPECT_NE(nullptr, exception);
    env_->ExceptionClear();
    EXPECT_TRUE(env_->IsInstanceOf(exception, exception_class));
  }

  void CleanUpJniEnv() {
    if (aioobe_ != nullptr) {
      env_->DeleteGlobalRef(aioobe_);
      aioobe_ = nullptr;
    }
    if (ase_ != nullptr) {
      env_->DeleteGlobalRef(ase_);
      ase_ = nullptr;
    }
    if (sioobe_ != nullptr) {
      env_->DeleteGlobalRef(sioobe_);
      sioobe_ = nullptr;
    }
  }

  virtual void TearDown() OVERRIDE {
    CleanUpJniEnv();
    CommonCompilerTest::TearDown();
  }

  jclass GetPrimitiveClass(char descriptor) {
    ScopedObjectAccess soa(env_);
    mirror::Class* c = class_linker_->FindPrimitiveClass(descriptor);
    CHECK(c != nullptr);
    return soa.AddLocalReference<jclass>(c);
  }

  void ExpectClassFound(const char* name) {
    EXPECT_NE(env_->FindClass(name), nullptr) << name;
    EXPECT_FALSE(env_->ExceptionCheck()) << name;
  }

  void ExpectClassNotFound(const char* name, bool check_jni, const char* check_jni_msg,
                           CheckJniAbortCatcher* abort_catcher) {
    EXPECT_EQ(env_->FindClass(name), nullptr) << name;
    if (!check_jni || check_jni_msg == nullptr) {
      EXPECT_TRUE(env_->ExceptionCheck()) << name;
      env_->ExceptionClear();
    } else {
      abort_catcher->Check(check_jni_msg);
    }
  }

  void FindClassTest(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher check_jni_abort_catcher;

    // Null argument is always an abort.
    env_->FindClass(nullptr);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "name == null");

    // Reference types...
    ExpectClassFound("java/lang/String");
    // ...for arrays too, where you must include "L;".
    ExpectClassFound("[Ljava/lang/String;");
    // Primitive arrays are okay too, if the primitive type is valid.
    ExpectClassFound("[C");

    // But primitive types aren't allowed...
    ExpectClassNotFound("C", check_jni, nullptr, &check_jni_abort_catcher);
    ExpectClassNotFound("V", check_jni, nullptr, &check_jni_abort_catcher);
    ExpectClassNotFound("K", check_jni, nullptr, &check_jni_abort_catcher);

    if (check_jni) {
      // Check JNI will reject invalid class names as aborts but without pending exceptions.
      EXPECT_EQ(env_->FindClass("java.lang.String"), nullptr);
      EXPECT_FALSE(env_->ExceptionCheck());
      check_jni_abort_catcher.Check("illegal class name 'java.lang.String'");

      EXPECT_EQ(env_->FindClass("[Ljava.lang.String;"), nullptr);
      EXPECT_FALSE(env_->ExceptionCheck());
      check_jni_abort_catcher.Check("illegal class name '[Ljava.lang.String;'");
    } else {
      // Without check JNI we're tolerant and replace '.' with '/'.
      ExpectClassFound("java.lang.String");
      ExpectClassFound("[Ljava.lang.String;");
    }

    ExpectClassNotFound("Ljava.lang.String;", check_jni, "illegal class name 'Ljava.lang.String;'",
                        &check_jni_abort_catcher);
    ExpectClassNotFound("[java.lang.String", check_jni, "illegal class name '[java.lang.String'",
                        &check_jni_abort_catcher);

    // You can't include the "L;" in a JNI class descriptor.
    ExpectClassNotFound("Ljava/lang/String;", check_jni, "illegal class name 'Ljava/lang/String;'",
                        &check_jni_abort_catcher);

    // But you must include it for an array of any reference type.
    ExpectClassNotFound("[java/lang/String", check_jni, "illegal class name '[java/lang/String'",
                        &check_jni_abort_catcher);

    ExpectClassNotFound("[K", check_jni, "illegal class name '[K'", &check_jni_abort_catcher);

    // Void arrays aren't allowed.
    ExpectClassNotFound("[V", check_jni, "illegal class name '[V'", &check_jni_abort_catcher);

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetFieldIdBadArgumentTest(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher check_jni_abort_catcher;

    jclass c = env_->FindClass("java/lang/String");
    ASSERT_NE(c, nullptr);

    jfieldID fid = env_->GetFieldID(nullptr, "count", "I");
    EXPECT_EQ(nullptr, fid);
    check_jni_abort_catcher.Check(check_jni ? "GetFieldID received NULL jclass"
                                            : "java_class == null");
    fid = env_->GetFieldID(c, nullptr, "I");
    EXPECT_EQ(nullptr, fid);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "name == null");
    fid = env_->GetFieldID(c, "count", nullptr);
    EXPECT_EQ(nullptr, fid);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "sig == null");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetStaticFieldIdBadArgumentTest(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher check_jni_abort_catcher;

    jclass c = env_->FindClass("java/lang/String");
    ASSERT_NE(c, nullptr);

    jfieldID fid = env_->GetStaticFieldID(nullptr, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
    EXPECT_EQ(nullptr, fid);
    check_jni_abort_catcher.Check(check_jni ? "GetStaticFieldID received NULL jclass"
                                            : "java_class == null");
    fid = env_->GetStaticFieldID(c, nullptr, "Ljava/util/Comparator;");
    EXPECT_EQ(nullptr, fid);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "name == null");
    fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", nullptr);
    EXPECT_EQ(nullptr, fid);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "sig == null");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetMethodIdBadArgumentTest(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher check_jni_abort_catcher;

    jmethodID method = env_->GetMethodID(nullptr, "<init>", "(Ljava/lang/String;)V");
    EXPECT_EQ(nullptr, method);
    check_jni_abort_catcher.Check(check_jni ? "GetMethodID received NULL jclass"
                                            : "java_class == null");
    jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");
    ASSERT_TRUE(jlnsme != nullptr);
    method = env_->GetMethodID(jlnsme, nullptr, "(Ljava/lang/String;)V");
    EXPECT_EQ(nullptr, method);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "name == null");
    method = env_->GetMethodID(jlnsme, "<init>", nullptr);
    EXPECT_EQ(nullptr, method);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "sig == null");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetStaticMethodIdBadArgumentTest(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher check_jni_abort_catcher;

    jmethodID method = env_->GetStaticMethodID(nullptr, "valueOf", "(I)Ljava/lang/String;");
    EXPECT_EQ(nullptr, method);
    check_jni_abort_catcher.Check(check_jni ? "GetStaticMethodID received NULL jclass"
                                            : "java_class == null");
    jclass jlstring = env_->FindClass("java/lang/String");
    method = env_->GetStaticMethodID(jlstring, nullptr, "(I)Ljava/lang/String;");
    EXPECT_EQ(nullptr, method);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "name == null");
    method = env_->GetStaticMethodID(jlstring, "valueOf", nullptr);
    EXPECT_EQ(nullptr, method);
    check_jni_abort_catcher.Check(check_jni ? "non-nullable const char* was NULL"
                                            : "sig == null");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetFromReflectedField_ToReflectedFieldBadArgumentTest(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher check_jni_abort_catcher;

    jclass c = env_->FindClass("java/lang/String");
    ASSERT_NE(c, nullptr);
    jfieldID fid = env_->GetFieldID(c, "count", "I");
    ASSERT_NE(fid, nullptr);

    // Check class argument for null argument, not checked in non-check JNI.
    jobject field = env_->ToReflectedField(nullptr, fid, JNI_FALSE);
    if (check_jni) {
      EXPECT_EQ(field, nullptr);
      check_jni_abort_catcher.Check("ToReflectedField received NULL jclass");
    } else {
      EXPECT_NE(field, nullptr);
    }

    field = env_->ToReflectedField(c, nullptr, JNI_FALSE);
    EXPECT_EQ(field, nullptr);
    check_jni_abort_catcher.Check(check_jni ? "jfieldID was NULL"
                                            : "fid == null");

    fid = env_->FromReflectedField(nullptr);
    ASSERT_EQ(fid, nullptr);
    check_jni_abort_catcher.Check(check_jni ? "expected non-null java.lang.reflect.Field"
                                            : "jlr_field == null");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetFromReflectedMethod_ToReflectedMethodBadArgumentTest(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher check_jni_abort_catcher;

    jclass c = env_->FindClass("java/lang/String");
    ASSERT_NE(c, nullptr);
    jmethodID mid = env_->GetMethodID(c, "<init>", "()V");
    ASSERT_NE(mid, nullptr);

    // Check class argument for null argument, not checked in non-check JNI.
    jobject method = env_->ToReflectedMethod(nullptr, mid, JNI_FALSE);
    if (check_jni) {
      EXPECT_EQ(method, nullptr);
      check_jni_abort_catcher.Check("ToReflectedMethod received NULL jclass");
    } else {
      EXPECT_NE(method, nullptr);
    }

    method = env_->ToReflectedMethod(c, nullptr, JNI_FALSE);
    EXPECT_EQ(method, nullptr);
    check_jni_abort_catcher.Check(check_jni ? "jmethodID was NULL"
                                            : "mid == null");
    mid = env_->FromReflectedMethod(method);
    ASSERT_EQ(mid, nullptr);
    check_jni_abort_catcher.Check(check_jni ? "expected non-null method" : "jlr_method == null");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void RegisterAndUnregisterNativesBadArguments(bool check_jni,
                                                CheckJniAbortCatcher* check_jni_abort_catcher) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    // Passing a class of null is a failure.
    {
      JNINativeMethod methods[] = { };
      EXPECT_EQ(env_->RegisterNatives(nullptr, methods, 0), JNI_ERR);
      check_jni_abort_catcher->Check(check_jni ? "RegisterNatives received NULL jclass"
                                               : "java_class == null");
    }

    // Passing methods as null is a failure.
    jclass jlobject = env_->FindClass("java/lang/Object");
    EXPECT_EQ(env_->RegisterNatives(jlobject, nullptr, 1), JNI_ERR);
    check_jni_abort_catcher->Check("methods == null");

    // Unregisters null is a failure.
    EXPECT_EQ(env_->UnregisterNatives(nullptr), JNI_ERR);
    check_jni_abort_catcher->Check(check_jni ? "UnregisterNatives received NULL jclass"
                                             : "java_class == null");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }


  void GetPrimitiveArrayElementsOfWrongType(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher jni_abort_catcher;

    jbooleanArray array = env_->NewBooleanArray(10);
    jboolean is_copy;
    EXPECT_EQ(env_->GetByteArrayElements(reinterpret_cast<jbyteArray>(array), &is_copy), nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected byte[]"
            : "attempt to get byte primitive array elements with an object of type boolean[]");
    EXPECT_EQ(env_->GetShortArrayElements(reinterpret_cast<jshortArray>(array), &is_copy), nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected short[]"
            : "attempt to get short primitive array elements with an object of type boolean[]");
    EXPECT_EQ(env_->GetCharArrayElements(reinterpret_cast<jcharArray>(array), &is_copy), nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected char[]"
            : "attempt to get char primitive array elements with an object of type boolean[]");
    EXPECT_EQ(env_->GetIntArrayElements(reinterpret_cast<jintArray>(array), &is_copy), nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected int[]"
            : "attempt to get int primitive array elements with an object of type boolean[]");
    EXPECT_EQ(env_->GetLongArrayElements(reinterpret_cast<jlongArray>(array), &is_copy), nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected long[]"
            : "attempt to get long primitive array elements with an object of type boolean[]");
    EXPECT_EQ(env_->GetFloatArrayElements(reinterpret_cast<jfloatArray>(array), &is_copy), nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected float[]"
            : "attempt to get float primitive array elements with an object of type boolean[]");
    EXPECT_EQ(env_->GetDoubleArrayElements(reinterpret_cast<jdoubleArray>(array), &is_copy), nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected double[]"
            : "attempt to get double primitive array elements with an object of type boolean[]");
    jbyteArray array2 = env_->NewByteArray(10);
    EXPECT_EQ(env_->GetBooleanArrayElements(reinterpret_cast<jbooleanArray>(array2), &is_copy),
              nullptr);
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type byte[] expected boolean[]"
            : "attempt to get boolean primitive array elements with an object of type byte[]");
    jobject object = env_->NewStringUTF("Test String");
    EXPECT_EQ(env_->GetBooleanArrayElements(reinterpret_cast<jbooleanArray>(object), &is_copy),
              nullptr);
    jni_abort_catcher.Check(
        check_jni ? "jarray argument has non-array type: java.lang.String"
        : "attempt to get boolean primitive array elements with an object of type java.lang.String");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void ReleasePrimitiveArrayElementsOfWrongType(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher jni_abort_catcher;
    {
      jbooleanArray array = env_->NewBooleanArray(10);
      ASSERT_TRUE(array != nullptr);
      jboolean is_copy;
      jboolean* elements = env_->GetBooleanArrayElements(array, &is_copy);
      ASSERT_TRUE(elements != nullptr);
      env_->ReleaseByteArrayElements(reinterpret_cast<jbyteArray>(array),
                                     reinterpret_cast<jbyte*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type boolean[] expected byte[]"
              : "attempt to release byte primitive array elements with an object of type boolean[]");
      env_->ReleaseShortArrayElements(reinterpret_cast<jshortArray>(array),
                                      reinterpret_cast<jshort*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type boolean[] expected short[]"
              : "attempt to release short primitive array elements with an object of type boolean[]");
      env_->ReleaseCharArrayElements(reinterpret_cast<jcharArray>(array),
                                     reinterpret_cast<jchar*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type boolean[] expected char[]"
              : "attempt to release char primitive array elements with an object of type boolean[]");
      env_->ReleaseIntArrayElements(reinterpret_cast<jintArray>(array),
                                    reinterpret_cast<jint*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type boolean[] expected int[]"
              : "attempt to release int primitive array elements with an object of type boolean[]");
      env_->ReleaseLongArrayElements(reinterpret_cast<jlongArray>(array),
                                     reinterpret_cast<jlong*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type boolean[] expected long[]"
              : "attempt to release long primitive array elements with an object of type boolean[]");
      env_->ReleaseFloatArrayElements(reinterpret_cast<jfloatArray>(array),
                                      reinterpret_cast<jfloat*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type boolean[] expected float[]"
              : "attempt to release float primitive array elements with an object of type boolean[]");
      env_->ReleaseDoubleArrayElements(reinterpret_cast<jdoubleArray>(array),
                                       reinterpret_cast<jdouble*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type boolean[] expected double[]"
              : "attempt to release double primitive array elements with an object of type boolean[]");

      // Don't leak the elements array.
      env_->ReleaseBooleanArrayElements(array, elements, 0);
    }
    {
      jbyteArray array = env_->NewByteArray(10);
      jboolean is_copy;
      jbyte* elements = env_->GetByteArrayElements(array, &is_copy);

      env_->ReleaseBooleanArrayElements(reinterpret_cast<jbooleanArray>(array),
                                        reinterpret_cast<jboolean*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "incompatible array type byte[] expected boolean[]"
              : "attempt to release boolean primitive array elements with an object of type byte[]");
      jobject object = env_->NewStringUTF("Test String");
      env_->ReleaseBooleanArrayElements(reinterpret_cast<jbooleanArray>(object),
                                        reinterpret_cast<jboolean*>(elements), 0);
      jni_abort_catcher.Check(
          check_jni ? "jarray argument has non-array type: java.lang.String"
              : "attempt to release boolean primitive array elements with an object of type "
              "java.lang.String");

      // Don't leak the elements array.
      env_->ReleaseByteArrayElements(array, elements, 0);
    }
    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetReleasePrimitiveArrayCriticalOfWrongType(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher jni_abort_catcher;

    jobject object = env_->NewStringUTF("Test String");
    jboolean is_copy;
    void* elements = env_->GetPrimitiveArrayCritical(reinterpret_cast<jarray>(object), &is_copy);
    jni_abort_catcher.Check(check_jni ? "jarray argument has non-array type: java.lang.String"
        : "expected primitive array, given java.lang.String");
    env_->ReleasePrimitiveArrayCritical(reinterpret_cast<jarray>(object), elements, 0);
    jni_abort_catcher.Check(check_jni ? "jarray argument has non-array type: java.lang.String"
        : "expected primitive array, given java.lang.String");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void GetPrimitiveArrayRegionElementsOfWrongType(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher jni_abort_catcher;
    constexpr size_t kLength = 10;
    jbooleanArray array = env_->NewBooleanArray(kLength);
    ASSERT_TRUE(array != nullptr);
    jboolean elements[kLength];
    env_->GetByteArrayRegion(reinterpret_cast<jbyteArray>(array), 0, kLength,
                             reinterpret_cast<jbyte*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected byte[]"
            : "attempt to get region of byte primitive array elements with an object of type boolean[]");
    env_->GetShortArrayRegion(reinterpret_cast<jshortArray>(array), 0, kLength,
                              reinterpret_cast<jshort*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected short[]"
            : "attempt to get region of short primitive array elements with an object of type boolean[]");
    env_->GetCharArrayRegion(reinterpret_cast<jcharArray>(array), 0, kLength,
                             reinterpret_cast<jchar*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected char[]"
            : "attempt to get region of char primitive array elements with an object of type boolean[]");
    env_->GetIntArrayRegion(reinterpret_cast<jintArray>(array), 0, kLength,
                            reinterpret_cast<jint*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected int[]"
            : "attempt to get region of int primitive array elements with an object of type boolean[]");
    env_->GetLongArrayRegion(reinterpret_cast<jlongArray>(array), 0, kLength,
                             reinterpret_cast<jlong*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected long[]"
            : "attempt to get region of long primitive array elements with an object of type boolean[]");
    env_->GetFloatArrayRegion(reinterpret_cast<jfloatArray>(array), 0, kLength,
                              reinterpret_cast<jfloat*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected float[]"
            : "attempt to get region of float primitive array elements with an object of type boolean[]");
    env_->GetDoubleArrayRegion(reinterpret_cast<jdoubleArray>(array), 0, kLength,
                               reinterpret_cast<jdouble*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected double[]"
            : "attempt to get region of double primitive array elements with an object of type boolean[]");
    jbyteArray array2 = env_->NewByteArray(10);
    env_->GetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(array2), 0, kLength,
                                reinterpret_cast<jboolean*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type byte[] expected boolean[]"
            : "attempt to get region of boolean primitive array elements with an object of type byte[]");
    jobject object = env_->NewStringUTF("Test String");
    env_->GetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(object), 0, kLength,
                                reinterpret_cast<jboolean*>(elements));
    jni_abort_catcher.Check(check_jni ? "jarray argument has non-array type: java.lang.String"
        : "attempt to get region of boolean primitive array elements with an object of type "
          "java.lang.String");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void SetPrimitiveArrayRegionElementsOfWrongType(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher jni_abort_catcher;
    constexpr size_t kLength = 10;
    jbooleanArray array = env_->NewBooleanArray(kLength);
    ASSERT_TRUE(array != nullptr);
    jboolean elements[kLength];
    env_->SetByteArrayRegion(reinterpret_cast<jbyteArray>(array), 0, kLength,
                             reinterpret_cast<jbyte*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected byte[]"
            : "attempt to set region of byte primitive array elements with an object of type boolean[]");
    env_->SetShortArrayRegion(reinterpret_cast<jshortArray>(array), 0, kLength,
                              reinterpret_cast<jshort*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected short[]"
            : "attempt to set region of short primitive array elements with an object of type boolean[]");
    env_->SetCharArrayRegion(reinterpret_cast<jcharArray>(array), 0, kLength,
                             reinterpret_cast<jchar*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected char[]"
            : "attempt to set region of char primitive array elements with an object of type boolean[]");
    env_->SetIntArrayRegion(reinterpret_cast<jintArray>(array), 0, kLength,
                            reinterpret_cast<jint*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected int[]"
            : "attempt to set region of int primitive array elements with an object of type boolean[]");
    env_->SetLongArrayRegion(reinterpret_cast<jlongArray>(array), 0, kLength,
                             reinterpret_cast<jlong*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected long[]"
            : "attempt to set region of long primitive array elements with an object of type boolean[]");
    env_->SetFloatArrayRegion(reinterpret_cast<jfloatArray>(array), 0, kLength,
                              reinterpret_cast<jfloat*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected float[]"
            : "attempt to set region of float primitive array elements with an object of type boolean[]");
    env_->SetDoubleArrayRegion(reinterpret_cast<jdoubleArray>(array), 0, kLength,
                               reinterpret_cast<jdouble*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type boolean[] expected double[]"
            : "attempt to set region of double primitive array elements with an object of type boolean[]");
    jbyteArray array2 = env_->NewByteArray(10);
    env_->SetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(array2), 0, kLength,
                                reinterpret_cast<jboolean*>(elements));
    jni_abort_catcher.Check(
        check_jni ? "incompatible array type byte[] expected boolean[]"
            : "attempt to set region of boolean primitive array elements with an object of type byte[]");
    jobject object = env_->NewStringUTF("Test String");
    env_->SetBooleanArrayRegion(reinterpret_cast<jbooleanArray>(object), 0, kLength,
                                reinterpret_cast<jboolean*>(elements));
    jni_abort_catcher.Check(check_jni ? "jarray argument has non-array type: java.lang.String"
        : "attempt to set region of boolean primitive array elements with an object of type "
          "java.lang.String");
    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void NewObjectArrayBadArguments(bool check_jni) {
    bool old_check_jni = vm_->SetCheckJniEnabled(check_jni);
    CheckJniAbortCatcher jni_abort_catcher;

    jclass element_class = env_->FindClass("java/lang/String");
    ASSERT_NE(element_class, nullptr);

    env_->NewObjectArray(-1, element_class, nullptr);
    jni_abort_catcher.Check(check_jni ? "negative jsize: -1" : "negative array length: -1");

    env_->NewObjectArray(std::numeric_limits<jint>::min(), element_class, nullptr);
    jni_abort_catcher.Check(check_jni ? "negative jsize: -2147483648"
        : "negative array length: -2147483648");

    EXPECT_EQ(check_jni, vm_->SetCheckJniEnabled(old_check_jni));
  }

  void SetUpForTest(bool direct, const char* method_name, const char* method_sig,
                    void* native_fnptr) {
    // Initialize class loader and set generic JNI entrypoint.
    // Note: this code is adapted from the jni_compiler_test, and taken with minimal modifications.
    if (!runtime_->IsStarted()) {
      {
        ScopedObjectAccess soa(Thread::Current());
        class_loader_ = LoadDex("MyClassNatives");
        StackHandleScope<1> hs(soa.Self());
        Handle<mirror::ClassLoader> loader(
            hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader_)));
        mirror::Class* c = class_linker_->FindClass(soa.Self(), "LMyClassNatives;", loader);
        const auto pointer_size = class_linker_->GetImagePointerSize();
        ArtMethod* method = direct ? c->FindDirectMethod(method_name, method_sig, pointer_size) :
            c->FindVirtualMethod(method_name, method_sig, pointer_size);
        ASSERT_TRUE(method != nullptr) << method_name << " " << method_sig;
        method->SetEntryPointFromQuickCompiledCode(class_linker_->GetRuntimeQuickGenericJniStub());
      }
      // Start runtime.
      Thread::Current()->TransitionFromSuspendedToRunnable();
      bool started = runtime_->Start();
      CHECK(started);
    }
    // JNI operations after runtime start.
    env_ = Thread::Current()->GetJniEnv();
    jklass_ = env_->FindClass("MyClassNatives");
    ASSERT_TRUE(jklass_ != nullptr) << method_name << " " << method_sig;

    if (direct) {
      jmethod_ = env_->GetStaticMethodID(jklass_, method_name, method_sig);
    } else {
      jmethod_ = env_->GetMethodID(jklass_, method_name, method_sig);
    }
    ASSERT_TRUE(jmethod_ != nullptr) << method_name << " " << method_sig;

    if (native_fnptr != nullptr) {
      JNINativeMethod methods[] = { { method_name, method_sig, native_fnptr } };
      ASSERT_EQ(JNI_OK, env_->RegisterNatives(jklass_, methods, 1))
          << method_name << " " << method_sig;
    } else {
      env_->UnregisterNatives(jklass_);
    }

    jmethodID constructor = env_->GetMethodID(jklass_, "<init>", "()V");
    jobj_ = env_->NewObject(jklass_, constructor);
    ASSERT_TRUE(jobj_ != nullptr) << method_name << " " << method_sig;
  }

  JavaVMExt* vm_;
  JNIEnv* env_;
  jclass aioobe_;
  jclass ase_;
  jclass sioobe_;

  jclass jklass_;
  jobject jobj_;
  jobject class_loader_;
  jmethodID jmethod_;
};

TEST_F(JniInternalTest, AllocObject) {
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);
  jobject o = env_->AllocObject(c);
  ASSERT_NE(o, nullptr);

  // We have an instance of the class we asked for...
  ASSERT_TRUE(env_->IsInstanceOf(o, c));
  // ...whose fields haven't been initialized because
  // we didn't call a constructor.
  ASSERT_EQ(0, env_->GetIntField(o, env_->GetFieldID(c, "count", "I")));
}

TEST_F(JniInternalTest, GetVersion) {
  ASSERT_EQ(JNI_VERSION_1_6, env_->GetVersion());
}

TEST_F(JniInternalTest, FindClass) {
  // This tests leads to warnings in the log.
  ScopedLogSeverity sls(LogSeverity::ERROR);

  FindClassTest(false);
  FindClassTest(true);
}

TEST_F(JniInternalTest, GetFieldID) {
  jclass jlnsfe = env_->FindClass("java/lang/NoSuchFieldError");
  ASSERT_NE(jlnsfe, nullptr);
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);

  // Wrong type.
  jfieldID fid = env_->GetFieldID(c, "count", "J");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong type where type doesn't exist.
  fid = env_->GetFieldID(c, "count", "Lrod/jane/freddy;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong name.
  fid = env_->GetFieldID(c, "Count", "I");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Good declared field lookup.
  fid = env_->GetFieldID(c, "count", "I");
  EXPECT_NE(nullptr, fid);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Good superclass field lookup.
  c = env_->FindClass("java/lang/StringBuilder");
  fid = env_->GetFieldID(c, "count", "I");
  EXPECT_NE(nullptr, fid);
  EXPECT_NE(fid, nullptr);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Not instance.
  fid = env_->GetFieldID(c, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Bad arguments.
  GetFieldIdBadArgumentTest(false);
  GetFieldIdBadArgumentTest(true);
}

TEST_F(JniInternalTest, GetStaticFieldID) {
  jclass jlnsfe = env_->FindClass("java/lang/NoSuchFieldError");
  ASSERT_NE(jlnsfe, nullptr);
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);

  // Wrong type.
  jfieldID fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "J");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong type where type doesn't exist.
  fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "Lrod/jane/freddy;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Wrong name.
  fid = env_->GetStaticFieldID(c, "cASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Good declared field lookup.
  fid = env_->GetStaticFieldID(c, "CASE_INSENSITIVE_ORDER", "Ljava/util/Comparator;");
  EXPECT_NE(nullptr, fid);
  EXPECT_NE(fid, nullptr);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Not static.
  fid = env_->GetStaticFieldID(c, "count", "I");
  EXPECT_EQ(nullptr, fid);
  ExpectException(jlnsfe);

  // Bad arguments.
  GetStaticFieldIdBadArgumentTest(false);
  GetStaticFieldIdBadArgumentTest(true);
}

TEST_F(JniInternalTest, GetMethodID) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlstring = env_->FindClass("java/lang/String");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");
  jclass jncrbc = env_->FindClass("java/nio/channels/ReadableByteChannel");

  // Sanity check that no exceptions are pending.
  ASSERT_FALSE(env_->ExceptionCheck());

  // Check that java.lang.Object.foo() doesn't exist and NoSuchMethodError is
  // a pending exception.
  jmethodID method = env_->GetMethodID(jlobject, "foo", "()V");
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that java.lang.Object.equals() does exist.
  method = env_->GetMethodID(jlobject, "equals", "(Ljava/lang/Object;)Z");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Check that GetMethodID for java.lang.String.valueOf(int) fails as the
  // method is static.
  method = env_->GetMethodID(jlstring, "valueOf", "(I)Ljava/lang/String;");
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that GetMethodID for java.lang.NoSuchMethodError.<init>(String) finds the constructor.
  method = env_->GetMethodID(jlnsme, "<init>", "(Ljava/lang/String;)V");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Check that GetMethodID can find a interface method inherited from another interface.
  method = env_->GetMethodID(jncrbc, "close", "()V");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Bad arguments.
  GetMethodIdBadArgumentTest(false);
  GetMethodIdBadArgumentTest(true);
}

TEST_F(JniInternalTest, CallVoidMethodNullReceiver) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jmethodID method;

  // Check that GetMethodID for java.lang.NoSuchMethodError.<init>(String) finds the constructor.
  method = env_->GetMethodID(jlobject, "<init>", "()V");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Null object to CallVoidMethod.
  CheckJniAbortCatcher check_jni_abort_catcher;
  env_->CallVoidMethod(nullptr, method);
  check_jni_abort_catcher.Check("null");
}

TEST_F(JniInternalTest, GetStaticMethodID) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");

  // Sanity check that no exceptions are pending
  ASSERT_FALSE(env_->ExceptionCheck());

  // Check that java.lang.Object.foo() doesn't exist and NoSuchMethodError is
  // a pending exception
  jmethodID method = env_->GetStaticMethodID(jlobject, "foo", "()V");
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that GetStaticMethodID for java.lang.Object.equals(Object) fails as
  // the method is not static
  method = env_->GetStaticMethodID(jlobject, "equals", "(Ljava/lang/Object;)Z");
  EXPECT_EQ(nullptr, method);
  ExpectException(jlnsme);

  // Check that java.lang.String.valueOf(int) does exist
  jclass jlstring = env_->FindClass("java/lang/String");
  method = env_->GetStaticMethodID(jlstring, "valueOf", "(I)Ljava/lang/String;");
  EXPECT_NE(nullptr, method);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Bad arguments.
  GetStaticMethodIdBadArgumentTest(false);
  GetStaticMethodIdBadArgumentTest(true);
}

TEST_F(JniInternalTest, FromReflectedField_ToReflectedField) {
  jclass jlrField = env_->FindClass("java/lang/reflect/Field");
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);
  jfieldID fid = env_->GetFieldID(c, "count", "I");
  ASSERT_NE(fid, nullptr);
  // Turn the fid into a java.lang.reflect.Field...
  jobject field = env_->ToReflectedField(c, fid, JNI_FALSE);
  for (size_t i = 0; i <= kLocalsMax; ++i) {
    // Regression test for b/18396311, ToReflectedField leaking local refs causing a local
    // reference table overflows with 512 references to ArtField
    env_->DeleteLocalRef(env_->ToReflectedField(c, fid, JNI_FALSE));
  }
  ASSERT_NE(c, nullptr);
  ASSERT_TRUE(env_->IsInstanceOf(field, jlrField));
  // ...and back again.
  jfieldID fid2 = env_->FromReflectedField(field);
  ASSERT_NE(fid2, nullptr);
  // Make sure we can actually use it.
  jstring s = env_->NewStringUTF("poop");
  ASSERT_EQ(4, env_->GetIntField(s, fid2));

  // Bad arguments.
  GetFromReflectedField_ToReflectedFieldBadArgumentTest(false);
  GetFromReflectedField_ToReflectedFieldBadArgumentTest(true);
}

TEST_F(JniInternalTest, FromReflectedMethod_ToReflectedMethod) {
  jclass jlrMethod = env_->FindClass("java/lang/reflect/Method");
  ASSERT_NE(jlrMethod, nullptr);
  jclass jlrConstructor = env_->FindClass("java/lang/reflect/Constructor");
  ASSERT_NE(jlrConstructor, nullptr);
  jclass c = env_->FindClass("java/lang/String");
  ASSERT_NE(c, nullptr);

  jmethodID mid = env_->GetMethodID(c, "<init>", "()V");
  ASSERT_NE(mid, nullptr);
  // Turn the mid into a java.lang.reflect.Constructor...
  jobject method = env_->ToReflectedMethod(c, mid, JNI_FALSE);
  for (size_t i = 0; i <= kLocalsMax; ++i) {
    // Regression test for b/18396311, ToReflectedMethod leaking local refs causing a local
    // reference table overflows with 512 references to ArtMethod
    env_->DeleteLocalRef(env_->ToReflectedMethod(c, mid, JNI_FALSE));
  }
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(env_->IsInstanceOf(method, jlrConstructor));
  // ...and back again.
  jmethodID mid2 = env_->FromReflectedMethod(method);
  ASSERT_NE(mid2, nullptr);
  // Make sure we can actually use it.
  jstring s = reinterpret_cast<jstring>(env_->AllocObject(c));
  ASSERT_NE(s, nullptr);
  env_->CallVoidMethod(s, mid2);
  ASSERT_EQ(JNI_FALSE, env_->ExceptionCheck());
  env_->ExceptionClear();

  mid = env_->GetMethodID(c, "length", "()I");
  ASSERT_NE(mid, nullptr);
  // Turn the mid into a java.lang.reflect.Method...
  method = env_->ToReflectedMethod(c, mid, JNI_FALSE);
  ASSERT_NE(method, nullptr);
  ASSERT_TRUE(env_->IsInstanceOf(method, jlrMethod));
  // ...and back again.
  mid2 = env_->FromReflectedMethod(method);
  ASSERT_NE(mid2, nullptr);
  // Make sure we can actually use it.
  s = env_->NewStringUTF("poop");
  ASSERT_NE(s, nullptr);
  ASSERT_EQ(4, env_->CallIntMethod(s, mid2));

  // Bad arguments.
  GetFromReflectedMethod_ToReflectedMethodBadArgumentTest(false);
  GetFromReflectedMethod_ToReflectedMethodBadArgumentTest(true);
}

static void BogusMethod() {
  // You can't pass null function pointers to RegisterNatives.
}

TEST_F(JniInternalTest, RegisterAndUnregisterNatives) {
  jclass jlobject = env_->FindClass("java/lang/Object");
  jclass jlnsme = env_->FindClass("java/lang/NoSuchMethodError");
  void* native_function = reinterpret_cast<void*>(BogusMethod);

  // Sanity check that no exceptions are pending.
  ASSERT_FALSE(env_->ExceptionCheck());

  // The following can print errors to the log we'd like to ignore.
  {
    ScopedLogSeverity sls(LogSeverity::FATAL);
    // Check that registering method without name causes a NoSuchMethodError.
    {
      JNINativeMethod methods[] = { { nullptr, "()V", native_function } };
      EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_ERR);
    }
    ExpectException(jlnsme);

    // Check that registering method without signature causes a NoSuchMethodError.
    {
      JNINativeMethod methods[] = { { "notify", nullptr, native_function } };
      EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_ERR);
    }
    ExpectException(jlnsme);

    // Check that registering method without function causes a NoSuchMethodError.
    {
      JNINativeMethod methods[] = { { "notify", "()V", nullptr } };
      EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_ERR);
    }
    ExpectException(jlnsme);

    // Check that registering to a non-existent java.lang.Object.foo() causes a NoSuchMethodError.
    {
      JNINativeMethod methods[] = { { "foo", "()V", native_function } };
      EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_ERR);
    }
    ExpectException(jlnsme);

    // Check that registering non-native methods causes a NoSuchMethodError.
    {
      JNINativeMethod methods[] = { { "equals", "(Ljava/lang/Object;)Z", native_function } };
      EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_ERR);
    }
    ExpectException(jlnsme);
  }

  // Check that registering native methods is successful.
  {
    JNINativeMethod methods[] = { { "notify", "()V", native_function } };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 1), JNI_OK);
  }
  EXPECT_FALSE(env_->ExceptionCheck());
  EXPECT_EQ(env_->UnregisterNatives(jlobject), JNI_OK);

  // Check that registering no methods isn't a failure.
  {
    JNINativeMethod methods[] = { };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, 0), JNI_OK);
  }
  EXPECT_FALSE(env_->ExceptionCheck());
  EXPECT_EQ(env_->UnregisterNatives(jlobject), JNI_OK);

  // Check that registering a -ve number of methods is a failure.
  CheckJniAbortCatcher check_jni_abort_catcher;
  for (int i = -10; i < 0; ++i) {
    JNINativeMethod methods[] = { };
    EXPECT_EQ(env_->RegisterNatives(jlobject, methods, i), JNI_ERR);
    check_jni_abort_catcher.Check("negative method count: ");
  }
  EXPECT_FALSE(env_->ExceptionCheck());

  // Unregistering a class with no natives is a warning.
  EXPECT_EQ(env_->UnregisterNatives(jlnsme), JNI_OK);

  RegisterAndUnregisterNativesBadArguments(false, &check_jni_abort_catcher);
  RegisterAndUnregisterNativesBadArguments(true, &check_jni_abort_catcher);
}

#define EXPECT_PRIMITIVE_ARRAY(new_fn, \
                               get_region_fn, \
                               set_region_fn, \
                               get_elements_fn, \
                               release_elements_fn, \
                               scalar_type, \
                               expected_class_descriptor) \
  jsize size = 4; \
  \
  { \
    CheckJniAbortCatcher jni_abort_catcher; \
    down_cast<JNIEnvExt*>(env_)->SetCheckJniEnabled(false); \
    /* Allocate an negative sized array and check it has the right failure type. */ \
    EXPECT_EQ(env_->new_fn(-1), nullptr); \
    jni_abort_catcher.Check("negative array length: -1"); \
    EXPECT_EQ(env_->new_fn(std::numeric_limits<jint>::min()), nullptr); \
    jni_abort_catcher.Check("negative array length: -2147483648"); \
    /* Pass the array as null. */ \
    EXPECT_EQ(0, env_->GetArrayLength(nullptr)); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->get_region_fn(nullptr, 0, 0, nullptr); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->set_region_fn(nullptr, 0, 0, nullptr); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->get_elements_fn(nullptr, nullptr); \
    jni_abort_catcher.Check("java_array == null"); \
    env_->release_elements_fn(nullptr, nullptr, 0); \
    jni_abort_catcher.Check("java_array == null"); \
    /* Pass the elements for region as null. */ \
    scalar_type ## Array a = env_->new_fn(size); \
    env_->get_region_fn(a, 0, size, nullptr); \
    jni_abort_catcher.Check("buf == null"); \
    env_->set_region_fn(a, 0, size, nullptr); \
    jni_abort_catcher.Check("buf == null"); \
    down_cast<JNIEnvExt*>(env_)->SetCheckJniEnabled(true); \
  } \
  /* Allocate an array and check it has the right type and length. */ \
  scalar_type ## Array a = env_->new_fn(size); \
  EXPECT_NE(a, nullptr); \
  EXPECT_TRUE(env_->IsInstanceOf(a, env_->FindClass(expected_class_descriptor))); \
  EXPECT_EQ(size, env_->GetArrayLength(a)); \
  \
  /* GetPrimitiveArrayRegion/SetPrimitiveArrayRegion */ \
  /* AIOOBE for negative start offset. */ \
  env_->get_region_fn(a, -1, 1, nullptr); \
  ExpectException(aioobe_); \
  env_->set_region_fn(a, -1, 1, nullptr); \
  ExpectException(aioobe_); \
  \
  /* AIOOBE for negative length. */ \
  env_->get_region_fn(a, 0, -1, nullptr); \
  ExpectException(aioobe_); \
  env_->set_region_fn(a, 0, -1, nullptr); \
  ExpectException(aioobe_); \
  \
  /* AIOOBE for buffer overrun. */ \
  env_->get_region_fn(a, size - 1, size, nullptr); \
  ExpectException(aioobe_); \
  env_->set_region_fn(a, size - 1, size, nullptr); \
  ExpectException(aioobe_); \
  \
  /* Regression test against integer overflow in range check. */ \
  env_->get_region_fn(a, 0x7fffffff, 0x7fffffff, nullptr); \
  ExpectException(aioobe_); \
  env_->set_region_fn(a, 0x7fffffff, 0x7fffffff, nullptr); \
  ExpectException(aioobe_); \
  \
  /* It's okay for the buffer to be null as long as the length is 0. */ \
  env_->get_region_fn(a, 2, 0, nullptr); \
  /* Even if the offset is invalid... */ \
  env_->get_region_fn(a, 123, 0, nullptr); \
  ExpectException(aioobe_); \
  \
  /* It's okay for the buffer to be null as long as the length is 0. */ \
  env_->set_region_fn(a, 2, 0, nullptr); \
  /* Even if the offset is invalid... */ \
  env_->set_region_fn(a, 123, 0, nullptr); \
  ExpectException(aioobe_); \
  \
  /* Prepare a couple of buffers. */ \
  std::unique_ptr<scalar_type[]> src_buf(new scalar_type[size]); \
  std::unique_ptr<scalar_type[]> dst_buf(new scalar_type[size]); \
  for (jsize i = 0; i < size; ++i) { src_buf[i] = scalar_type(i); } \
  for (jsize i = 0; i < size; ++i) { dst_buf[i] = scalar_type(-1); } \
  \
  /* Copy all of src_buf onto the heap. */ \
  env_->set_region_fn(a, 0, size, &src_buf[0]); \
  /* Copy back only part. */ \
  env_->get_region_fn(a, 1, size - 2, &dst_buf[1]); \
  EXPECT_NE(memcmp(&src_buf[0], &dst_buf[0], size * sizeof(scalar_type)), 0) \
    << "short copy equal"; \
  /* Copy the missing pieces. */ \
  env_->get_region_fn(a, 0, 1, &dst_buf[0]); \
  env_->get_region_fn(a, size - 1, 1, &dst_buf[size - 1]); \
  EXPECT_EQ(memcmp(&src_buf[0], &dst_buf[0], size * sizeof(scalar_type)), 0) \
    << "fixed copy not equal"; \
  /* Copy back the whole array. */ \
  env_->get_region_fn(a, 0, size, &dst_buf[0]); \
  EXPECT_EQ(memcmp(&src_buf[0], &dst_buf[0], size * sizeof(scalar_type)), 0) \
    << "full copy not equal"; \
  /* GetPrimitiveArrayCritical */ \
  void* v = env_->GetPrimitiveArrayCritical(a, nullptr); \
  EXPECT_EQ(memcmp(&src_buf[0], v, size * sizeof(scalar_type)), 0) \
    << "GetPrimitiveArrayCritical not equal"; \
  env_->ReleasePrimitiveArrayCritical(a, v, 0); \
  /* GetXArrayElements */ \
  scalar_type* xs = env_->get_elements_fn(a, nullptr); \
  EXPECT_EQ(memcmp(&src_buf[0], xs, size * sizeof(scalar_type)), 0) \
    << # get_elements_fn " not equal"; \
  env_->release_elements_fn(a, xs, 0); \

TEST_F(JniInternalTest, BooleanArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewBooleanArray, GetBooleanArrayRegion, SetBooleanArrayRegion,
                         GetBooleanArrayElements, ReleaseBooleanArrayElements, jboolean, "[Z");
}
TEST_F(JniInternalTest, ByteArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewByteArray, GetByteArrayRegion, SetByteArrayRegion,
                         GetByteArrayElements, ReleaseByteArrayElements, jbyte, "[B");
}
TEST_F(JniInternalTest, CharArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewCharArray, GetCharArrayRegion, SetCharArrayRegion,
                         GetCharArrayElements, ReleaseCharArrayElements, jchar, "[C");
}
TEST_F(JniInternalTest, DoubleArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewDoubleArray, GetDoubleArrayRegion, SetDoubleArrayRegion,
                         GetDoubleArrayElements, ReleaseDoubleArrayElements, jdouble, "[D");
}
TEST_F(JniInternalTest, FloatArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewFloatArray, GetFloatArrayRegion, SetFloatArrayRegion,
                         GetFloatArrayElements, ReleaseFloatArrayElements, jfloat, "[F");
}
TEST_F(JniInternalTest, IntArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewIntArray, GetIntArrayRegion, SetIntArrayRegion,
                         GetIntArrayElements, ReleaseIntArrayElements, jint, "[I");
}
TEST_F(JniInternalTest, LongArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewLongArray, GetLongArrayRegion, SetLongArrayRegion,
                         GetLongArrayElements, ReleaseLongArrayElements, jlong, "[J");
}
TEST_F(JniInternalTest, ShortArrays) {
  EXPECT_PRIMITIVE_ARRAY(NewShortArray, GetShortArrayRegion, SetShortArrayRegion,
                         GetShortArrayElements, ReleaseShortArrayElements, jshort, "[S");
}

TEST_F(JniInternalTest, GetPrimitiveArrayElementsOfWrongType) {
  GetPrimitiveArrayElementsOfWrongType(false);
  GetPrimitiveArrayElementsOfWrongType(true);
}

TEST_F(JniInternalTest, ReleasePrimitiveArrayElementsOfWrongType) {
  ReleasePrimitiveArrayElementsOfWrongType(false);
  ReleasePrimitiveArrayElementsOfWrongType(true);
}

TEST_F(JniInternalTest, GetReleasePrimitiveArrayCriticalOfWrongType) {
  GetReleasePrimitiveArrayCriticalOfWrongType(false);
  GetReleasePrimitiveArrayCriticalOfWrongType(true);
}

TEST_F(JniInternalTest, GetPrimitiveArrayRegionElementsOfWrongType) {
  GetPrimitiveArrayRegionElementsOfWrongType(false);
  GetPrimitiveArrayRegionElementsOfWrongType(true);
}

TEST_F(JniInternalTest, SetPrimitiveArrayRegionElementsOfWrongType) {
  SetPrimitiveArrayRegionElementsOfWrongType(false);
  SetPrimitiveArrayRegionElementsOfWrongType(true);
}

TEST_F(JniInternalTest, NewObjectArray) {
  jclass element_class = env_->FindClass("java/lang/String");
  ASSERT_NE(element_class, nullptr);
  jclass array_class = env_->FindClass("[Ljava/lang/String;");
  ASSERT_NE(array_class, nullptr);

  jobjectArray a = env_->NewObjectArray(0, element_class, nullptr);
  EXPECT_NE(a, nullptr);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(0, env_->GetArrayLength(a));

  a = env_->NewObjectArray(1, element_class, nullptr);
  EXPECT_NE(a, nullptr);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(1, env_->GetArrayLength(a));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 0), nullptr));

  // Negative array length checks.
  NewObjectArrayBadArguments(false);
  NewObjectArrayBadArguments(true);
}

TEST_F(JniInternalTest, NewObjectArrayWithPrimitiveClasses) {
  const char* primitive_descriptors = "VZBSCIJFD";
  const char* primitive_names[] = {
      "void", "boolean", "byte", "short", "char", "int", "long", "float", "double"
  };
  ASSERT_EQ(strlen(primitive_descriptors), arraysize(primitive_names));

  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  CheckJniAbortCatcher jni_abort_catcher;
  for (size_t i = 0; i < strlen(primitive_descriptors); ++i) {
    env_->NewObjectArray(0, nullptr, nullptr);
    jni_abort_catcher.Check("element_jclass == null");
    jclass primitive_class = GetPrimitiveClass(primitive_descriptors[i]);
    env_->NewObjectArray(1, primitive_class, nullptr);
    std::string error_msg(StringPrintf("not an object type: %s", primitive_names[i]));
    jni_abort_catcher.Check(error_msg.c_str());
  }
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  for (size_t i = 0; i < strlen(primitive_descriptors); ++i) {
    env_->NewObjectArray(0, nullptr, nullptr);
    jni_abort_catcher.Check("NewObjectArray received NULL jclass");
    jclass primitive_class = GetPrimitiveClass(primitive_descriptors[i]);
    env_->NewObjectArray(1, primitive_class, nullptr);
    std::string error_msg(StringPrintf("not an object type: %s", primitive_names[i]));
    jni_abort_catcher.Check(error_msg.c_str());
  }
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

TEST_F(JniInternalTest, NewObjectArrayWithInitialValue) {
  jclass element_class = env_->FindClass("java/lang/String");
  ASSERT_NE(element_class, nullptr);
  jclass array_class = env_->FindClass("[Ljava/lang/String;");
  ASSERT_NE(array_class, nullptr);

  jstring s = env_->NewStringUTF("poop");
  jobjectArray a = env_->NewObjectArray(2, element_class, s);
  EXPECT_NE(a, nullptr);
  EXPECT_TRUE(env_->IsInstanceOf(a, array_class));
  EXPECT_EQ(2, env_->GetArrayLength(a));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 0), s));
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(a, 1), s));

  // Attempt to incorrect create an array of strings with initial value of string arrays.
  CheckJniAbortCatcher jni_abort_catcher;
  env_->NewObjectArray(2, element_class, a);
  jni_abort_catcher.Check("cannot assign object of type 'java.lang.String[]' to array with element "
                          "type of 'java.lang.String'");
}

TEST_F(JniInternalTest, GetArrayLength) {
  // Already tested in NewObjectArray/NewPrimitiveArray except for null.
  CheckJniAbortCatcher jni_abort_catcher;
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  EXPECT_EQ(0, env_->GetArrayLength(nullptr));
  jni_abort_catcher.Check("java_array == null");
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  EXPECT_EQ(JNI_ERR, env_->GetArrayLength(nullptr));
  jni_abort_catcher.Check("jarray was NULL");
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

TEST_F(JniInternalTest, GetObjectClass) {
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_NE(string_class, nullptr);
  jclass class_class = env_->FindClass("java/lang/Class");
  ASSERT_NE(class_class, nullptr);

  jstring s = env_->NewStringUTF("poop");
  jclass c = env_->GetObjectClass(s);
  ASSERT_TRUE(env_->IsSameObject(string_class, c));

  jclass c2 = env_->GetObjectClass(c);
  ASSERT_TRUE(env_->IsSameObject(class_class, env_->GetObjectClass(c2)));

  // Null as object should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  EXPECT_EQ(env_->GetObjectClass(nullptr), nullptr);
  jni_abort_catcher.Check("java_object == null");
}

TEST_F(JniInternalTest, GetSuperclass) {
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_NE(object_class, nullptr);
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_NE(string_class, nullptr);
  jclass runnable_interface = env_->FindClass("java/lang/Runnable");
  ASSERT_NE(runnable_interface, nullptr);
  ASSERT_TRUE(env_->IsSameObject(object_class, env_->GetSuperclass(string_class)));
  ASSERT_EQ(env_->GetSuperclass(object_class), nullptr);
  ASSERT_EQ(env_->GetSuperclass(runnable_interface), nullptr);

  // Null as class should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  EXPECT_EQ(env_->GetSuperclass(nullptr), nullptr);
  jni_abort_catcher.Check("java_class == null");
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  EXPECT_EQ(env_->GetSuperclass(nullptr), nullptr);
  jni_abort_catcher.Check("GetSuperclass received NULL jclass");
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

TEST_F(JniInternalTest, IsAssignableFrom) {
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_NE(object_class, nullptr);
  jclass string_class = env_->FindClass("java/lang/String");
  ASSERT_NE(string_class, nullptr);

  // A superclass is assignable from an instance of its
  // subclass but not vice versa.
  ASSERT_TRUE(env_->IsAssignableFrom(string_class, object_class));
  ASSERT_FALSE(env_->IsAssignableFrom(object_class, string_class));

  jclass charsequence_interface = env_->FindClass("java/lang/CharSequence");
  ASSERT_NE(charsequence_interface, nullptr);

  // An interface is assignable from an instance of an implementing
  // class but not vice versa.
  ASSERT_TRUE(env_->IsAssignableFrom(string_class, charsequence_interface));
  ASSERT_FALSE(env_->IsAssignableFrom(charsequence_interface, string_class));

  // Check that arrays are covariant.
  jclass string_array_class = env_->FindClass("[Ljava/lang/String;");
  ASSERT_NE(string_array_class, nullptr);
  jclass object_array_class = env_->FindClass("[Ljava/lang/Object;");
  ASSERT_NE(object_array_class, nullptr);
  ASSERT_TRUE(env_->IsAssignableFrom(string_array_class, object_array_class));
  ASSERT_FALSE(env_->IsAssignableFrom(object_array_class, string_array_class));

  // Primitive types are tested in 004-JniTest.

  // Null as either class should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  EXPECT_EQ(env_->IsAssignableFrom(nullptr, string_class), JNI_FALSE);
  jni_abort_catcher.Check("java_class1 == null");
  EXPECT_EQ(env_->IsAssignableFrom(object_class, nullptr), JNI_FALSE);
  jni_abort_catcher.Check("java_class2 == null");
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  EXPECT_EQ(env_->IsAssignableFrom(nullptr, string_class), JNI_FALSE);
  jni_abort_catcher.Check("IsAssignableFrom received NULL jclass");
  EXPECT_EQ(env_->IsAssignableFrom(object_class, nullptr), JNI_FALSE);
  jni_abort_catcher.Check("IsAssignableFrom received NULL jclass");
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

TEST_F(JniInternalTest, GetObjectRefType) {
  jclass local = env_->FindClass("java/lang/Object");
  ASSERT_TRUE(local != nullptr);
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(local));

  jobject global = env_->NewGlobalRef(local);
  EXPECT_EQ(JNIGlobalRefType, env_->GetObjectRefType(global));

  jweak weak_global = env_->NewWeakGlobalRef(local);
  EXPECT_EQ(JNIWeakGlobalRefType, env_->GetObjectRefType(weak_global));

  {
    CheckJniAbortCatcher jni_abort_catcher;
    jobject invalid = reinterpret_cast<jobject>(this);
    EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(invalid));
    jni_abort_catcher.Check("use of invalid jobject");
  }

  // TODO: invoke a native method and test that its arguments are considered local references.

  // Null as pointer should not fail and return invalid-ref. b/18820997
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(nullptr));

  // TODO: Null as reference should return the original type.
  // This requires running a GC so a non-null object gets freed.
}

TEST_F(JniInternalTest, StaleWeakGlobal) {
  jclass java_lang_Class = env_->FindClass("java/lang/Class");
  ASSERT_NE(java_lang_Class, nullptr);
  jobjectArray local_ref = env_->NewObjectArray(1, java_lang_Class, nullptr);
  ASSERT_NE(local_ref, nullptr);
  jweak weak_global = env_->NewWeakGlobalRef(local_ref);
  ASSERT_NE(weak_global, nullptr);
  env_->DeleteLocalRef(local_ref);
  Runtime::Current()->GetHeap()->CollectGarbage(false);  // GC should clear the weak global.
  jobject new_global_ref = env_->NewGlobalRef(weak_global);
  EXPECT_EQ(new_global_ref, nullptr);
  jobject new_local_ref = env_->NewLocalRef(weak_global);
  EXPECT_EQ(new_local_ref, nullptr);
}

TEST_F(JniInternalTest, NewStringUTF) {
  EXPECT_EQ(env_->NewStringUTF(nullptr), nullptr);
  jstring s;

  s = env_->NewStringUTF("");
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(0, env_->GetStringLength(s));
  EXPECT_EQ(0, env_->GetStringUTFLength(s));
  s = env_->NewStringUTF("hello");
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(5, env_->GetStringLength(s));
  EXPECT_EQ(5, env_->GetStringUTFLength(s));

  // Encoded surrogate pair.
  s = env_->NewStringUTF("\xed\xa0\x81\xed\xb0\x80");
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(2, env_->GetStringLength(s));

  // The surrogate pair gets encoded into a 4 byte UTF sequence..
  EXPECT_EQ(4, env_->GetStringUTFLength(s));
  const char* chars = env_->GetStringUTFChars(s, nullptr);
  EXPECT_STREQ("\xf0\x90\x90\x80", chars);
  env_->ReleaseStringUTFChars(s, chars);

  // .. but is stored as is in the utf-16 representation.
  const jchar* jchars = env_->GetStringChars(s, nullptr);
  EXPECT_EQ(0xd801, jchars[0]);
  EXPECT_EQ(0xdc00, jchars[1]);
  env_->ReleaseStringChars(s, jchars);

  // 4 byte UTF sequence appended to an encoded surrogate pair.
  s = env_->NewStringUTF("\xed\xa0\x81\xed\xb0\x80 \xf0\x9f\x8f\xa0");
  EXPECT_NE(s, nullptr);

  // The 4 byte sequence {0xf0, 0x9f, 0x8f, 0xa0} is converted into a surrogate
  // pair {0xd83c, 0xdfe0}.
  EXPECT_EQ(5, env_->GetStringLength(s));
  jchars = env_->GetStringChars(s, nullptr);
  // The first surrogate pair, encoded as such in the input.
  EXPECT_EQ(0xd801, jchars[0]);
  EXPECT_EQ(0xdc00, jchars[1]);
  // The second surrogate pair, from the 4 byte UTF sequence in the input.
  EXPECT_EQ(0xd83c, jchars[3]);
  EXPECT_EQ(0xdfe0, jchars[4]);
  env_->ReleaseStringChars(s, jchars);

  EXPECT_EQ(9, env_->GetStringUTFLength(s));
  chars = env_->GetStringUTFChars(s, nullptr);
  EXPECT_STREQ("\xf0\x90\x90\x80 \xf0\x9f\x8f\xa0", chars);
  env_->ReleaseStringUTFChars(s, chars);

  // A string with 1, 2, 3 and 4 byte UTF sequences with spaces
  // between them
  s = env_->NewStringUTF("\x24 \xc2\xa2 \xe2\x82\xac \xf0\x9f\x8f\xa0");
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(8, env_->GetStringLength(s));
  EXPECT_EQ(13, env_->GetStringUTFLength(s));
}

TEST_F(JniInternalTest, NewString) {
  jchar chars[] = { 'h', 'i' };
  jstring s;
  s = env_->NewString(chars, 0);
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(0, env_->GetStringLength(s));
  EXPECT_EQ(0, env_->GetStringUTFLength(s));
  s = env_->NewString(chars, 2);
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(2, env_->GetStringLength(s));
  EXPECT_EQ(2, env_->GetStringUTFLength(s));

  // TODO: check some non-ASCII strings.
}

TEST_F(JniInternalTest, NewStringNullCharsZeroLength) {
  jstring s = env_->NewString(nullptr, 0);
  EXPECT_NE(s, nullptr);
  EXPECT_EQ(0, env_->GetStringLength(s));
}

TEST_F(JniInternalTest, NewStringNullCharsNonzeroLength) {
  CheckJniAbortCatcher jni_abort_catcher;
  env_->NewString(nullptr, 1);
  jni_abort_catcher.Check("chars == null && char_count > 0");
}

TEST_F(JniInternalTest, NewStringNegativeLength) {
  CheckJniAbortCatcher jni_abort_catcher;
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  env_->NewString(nullptr, -1);
  jni_abort_catcher.Check("char_count < 0: -1");
  env_->NewString(nullptr, std::numeric_limits<jint>::min());
  jni_abort_catcher.Check("char_count < 0: -2147483648");
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  env_->NewString(nullptr, -1);
  jni_abort_catcher.Check("negative jsize: -1");
  env_->NewString(nullptr, std::numeric_limits<jint>::min());
  jni_abort_catcher.Check("negative jsize: -2147483648");
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

TEST_F(JniInternalTest, GetStringLength_GetStringUTFLength) {
  // Already tested in the NewString/NewStringUTF tests.
}

TEST_F(JniInternalTest, GetStringRegion_GetStringUTFRegion) {
  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != nullptr);

  env_->GetStringRegion(s, -1, 0, nullptr);
  ExpectException(sioobe_);
  env_->GetStringRegion(s, 0, -1, nullptr);
  ExpectException(sioobe_);
  env_->GetStringRegion(s, 0, 10, nullptr);
  ExpectException(sioobe_);
  env_->GetStringRegion(s, 10, 1, nullptr);
  ExpectException(sioobe_);
  // Regression test against integer overflow in range check.
  env_->GetStringRegion(s, 0x7fffffff, 0x7fffffff, nullptr);
  ExpectException(sioobe_);

  jchar chars[4] = { 'x', 'x', 'x', 'x' };
  env_->GetStringRegion(s, 1, 2, &chars[1]);
  EXPECT_EQ('x', chars[0]);
  EXPECT_EQ('e', chars[1]);
  EXPECT_EQ('l', chars[2]);
  EXPECT_EQ('x', chars[3]);

  // It's okay for the buffer to be null as long as the length is 0.
  env_->GetStringRegion(s, 2, 0, nullptr);
  // Even if the offset is invalid...
  env_->GetStringRegion(s, 123, 0, nullptr);
  ExpectException(sioobe_);

  env_->GetStringUTFRegion(s, -1, 0, nullptr);
  ExpectException(sioobe_);
  env_->GetStringUTFRegion(s, 0, -1, nullptr);
  ExpectException(sioobe_);
  env_->GetStringUTFRegion(s, 0, 10, nullptr);
  ExpectException(sioobe_);
  env_->GetStringUTFRegion(s, 10, 1, nullptr);
  ExpectException(sioobe_);
  // Regression test against integer overflow in range check.
  env_->GetStringUTFRegion(s, 0x7fffffff, 0x7fffffff, nullptr);
  ExpectException(sioobe_);

  char bytes[4] = { 'x', 'x', 'x', 'x' };
  env_->GetStringUTFRegion(s, 1, 2, &bytes[1]);
  EXPECT_EQ('x', bytes[0]);
  EXPECT_EQ('e', bytes[1]);
  EXPECT_EQ('l', bytes[2]);
  EXPECT_EQ('x', bytes[3]);

  // It's okay for the buffer to be null as long as the length is 0.
  env_->GetStringUTFRegion(s, 2, 0, nullptr);
  // Even if the offset is invalid...
  env_->GetStringUTFRegion(s, 123, 0, nullptr);
  ExpectException(sioobe_);
}

TEST_F(JniInternalTest, GetStringUTFChars_ReleaseStringUTFChars) {
  // Passing in a null jstring is ignored normally, but caught by -Xcheck:jni.
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    EXPECT_EQ(env_->GetStringUTFChars(nullptr, nullptr), nullptr);
  }
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
    EXPECT_EQ(env_->GetStringUTFChars(nullptr, nullptr), nullptr);
    check_jni_abort_catcher.Check("GetStringUTFChars received NULL jstring");
    EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
  }

  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != nullptr);

  const char* utf = env_->GetStringUTFChars(s, nullptr);
  EXPECT_STREQ("hello", utf);
  env_->ReleaseStringUTFChars(s, utf);

  jboolean is_copy = JNI_FALSE;
  utf = env_->GetStringUTFChars(s, &is_copy);
  EXPECT_EQ(JNI_TRUE, is_copy);
  EXPECT_STREQ("hello", utf);
  env_->ReleaseStringUTFChars(s, utf);
}

TEST_F(JniInternalTest, GetStringChars_ReleaseStringChars) {
  jstring s = env_->NewStringUTF("hello");
  ScopedObjectAccess soa(env_);
  mirror::String* s_m = soa.Decode<mirror::String*>(s);
  ASSERT_TRUE(s != nullptr);

  jchar expected[] = { 'h', 'e', 'l', 'l', 'o' };
  const jchar* chars = env_->GetStringChars(s, nullptr);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringChars(s, chars);

  jboolean is_copy = JNI_FALSE;
  chars = env_->GetStringChars(s, &is_copy);
  if (Runtime::Current()->GetHeap()->IsMovableObject(s_m)) {
    EXPECT_EQ(JNI_TRUE, is_copy);
  } else {
    EXPECT_EQ(JNI_FALSE, is_copy);
  }
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringChars(s, chars);
}

TEST_F(JniInternalTest, GetStringCritical_ReleaseStringCritical) {
  jstring s = env_->NewStringUTF("hello");
  ASSERT_TRUE(s != nullptr);

  jchar expected[] = { 'h', 'e', 'l', 'l', 'o' };
  const jchar* chars = env_->GetStringCritical(s, nullptr);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringCritical(s, chars);

  jboolean is_copy = JNI_TRUE;
  chars = env_->GetStringCritical(s, &is_copy);
  EXPECT_EQ(JNI_FALSE, is_copy);
  EXPECT_EQ(expected[0], chars[0]);
  EXPECT_EQ(expected[1], chars[1]);
  EXPECT_EQ(expected[2], chars[2]);
  EXPECT_EQ(expected[3], chars[3]);
  EXPECT_EQ(expected[4], chars[4]);
  env_->ReleaseStringCritical(s, chars);
}

TEST_F(JniInternalTest, GetObjectArrayElement_SetObjectArrayElement) {
  jclass java_lang_Class = env_->FindClass("java/lang/Class");
  ASSERT_TRUE(java_lang_Class != nullptr);

  jobjectArray array = env_->NewObjectArray(1, java_lang_Class, nullptr);
  EXPECT_NE(array, nullptr);
  EXPECT_EQ(env_->GetObjectArrayElement(array, 0), nullptr);
  env_->SetObjectArrayElement(array, 0, java_lang_Class);
  EXPECT_TRUE(env_->IsSameObject(env_->GetObjectArrayElement(array, 0), java_lang_Class));

  // ArrayIndexOutOfBounds for negative index.
  env_->SetObjectArrayElement(array, -1, java_lang_Class);
  ExpectException(aioobe_);

  // ArrayIndexOutOfBounds for too-large index.
  env_->SetObjectArrayElement(array, 1, java_lang_Class);
  ExpectException(aioobe_);

  // ArrayStoreException thrown for bad types.
  env_->SetObjectArrayElement(array, 0, env_->NewStringUTF("not a jclass!"));
  ExpectException(ase_);

  // Null as array should fail.
  CheckJniAbortCatcher jni_abort_catcher;
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  EXPECT_EQ(nullptr, env_->GetObjectArrayElement(nullptr, 0));
  jni_abort_catcher.Check("java_array == null");
  env_->SetObjectArrayElement(nullptr, 0, nullptr);
  jni_abort_catcher.Check("java_array == null");
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  EXPECT_EQ(nullptr, env_->GetObjectArrayElement(nullptr, 0));
  jni_abort_catcher.Check("jarray was NULL");
  env_->SetObjectArrayElement(nullptr, 0, nullptr);
  jni_abort_catcher.Check("jarray was NULL");
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

#define EXPECT_STATIC_PRIMITIVE_FIELD(expect_eq, type, field_name, sig, value1, value2) \
  do { \
    jfieldID fid = env_->GetStaticFieldID(c, field_name, sig); \
    EXPECT_NE(fid, nullptr); \
    env_->SetStatic ## type ## Field(c, fid, value1); \
    expect_eq(value1, env_->GetStatic ## type ## Field(c, fid)); \
    env_->SetStatic ## type ## Field(c, fid, value2); \
    expect_eq(value2, env_->GetStatic ## type ## Field(c, fid)); \
    \
    bool old_check_jni = vm_->SetCheckJniEnabled(false); \
    { \
      CheckJniAbortCatcher jni_abort_catcher; \
      env_->GetStatic ## type ## Field(nullptr, fid); \
      env_->SetStatic ## type ## Field(nullptr, fid, value1); \
    } \
    CheckJniAbortCatcher jni_abort_catcher; \
    env_->GetStatic ## type ## Field(c, nullptr); \
    jni_abort_catcher.Check("fid == null"); \
    env_->SetStatic ## type ## Field(c, nullptr, value1); \
    jni_abort_catcher.Check("fid == null"); \
    \
    EXPECT_FALSE(vm_->SetCheckJniEnabled(true)); \
    env_->GetStatic ## type ## Field(nullptr, fid); \
    jni_abort_catcher.Check("received NULL jclass"); \
    env_->SetStatic ## type ## Field(nullptr, fid, value1); \
    jni_abort_catcher.Check("received NULL jclass"); \
    env_->GetStatic ## type ## Field(c, nullptr); \
    jni_abort_catcher.Check("jfieldID was NULL"); \
    env_->SetStatic ## type ## Field(c, nullptr, value1); \
    jni_abort_catcher.Check("jfieldID was NULL"); \
    EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni)); \
  } while (false)

#define EXPECT_PRIMITIVE_FIELD(expect_eq, instance, type, field_name, sig, value1, value2) \
  do { \
    jfieldID fid = env_->GetFieldID(c, field_name, sig); \
    EXPECT_NE(fid, nullptr); \
    env_->Set ## type ## Field(instance, fid, value1); \
    expect_eq(value1, env_->Get ## type ## Field(instance, fid)); \
    env_->Set ## type ## Field(instance, fid, value2); \
    expect_eq(value2, env_->Get ## type ## Field(instance, fid)); \
    \
    bool old_check_jni = vm_->SetCheckJniEnabled(false); \
    CheckJniAbortCatcher jni_abort_catcher; \
    env_->Get ## type ## Field(nullptr, fid); \
    jni_abort_catcher.Check("obj == null"); \
    env_->Set ## type ## Field(nullptr, fid, value1); \
    jni_abort_catcher.Check("obj == null"); \
    env_->Get ## type ## Field(instance, nullptr); \
    jni_abort_catcher.Check("fid == null"); \
    env_->Set ## type ## Field(instance, nullptr, value1); \
    jni_abort_catcher.Check("fid == null"); \
    EXPECT_FALSE(vm_->SetCheckJniEnabled(true)); \
    env_->Get ## type ## Field(nullptr, fid); \
    jni_abort_catcher.Check("field operation on NULL object:"); \
    env_->Set ## type ## Field(nullptr, fid, value1); \
    jni_abort_catcher.Check("field operation on NULL object:"); \
    env_->Get ## type ## Field(instance, nullptr); \
    jni_abort_catcher.Check("jfieldID was NULL"); \
    env_->Set ## type ## Field(instance, nullptr, value1); \
    jni_abort_catcher.Check("jfieldID was NULL"); \
    EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni)); \
  } while (false)


TEST_F(JniInternalTest, GetPrimitiveField_SetPrimitiveField) {
  Thread::Current()->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  bool started = runtime_->Start();
  ASSERT_TRUE(started);

  jclass c = env_->FindClass("AllFields");
  ASSERT_NE(c, nullptr);
  jobject o = env_->AllocObject(c);
  ASSERT_NE(o, nullptr);

  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_EQ, Boolean, "sZ", "Z", JNI_TRUE, JNI_FALSE);
  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_EQ, Byte, "sB", "B", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_EQ, Char, "sC", "C", 'a', 'b');
  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_DOUBLE_EQ, Double, "sD", "D", 1.0, 2.0);
  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_FLOAT_EQ, Float, "sF", "F", 1.0, 2.0);
  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_EQ, Int, "sI", "I", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_EQ, Long, "sJ", "J", 1, 2);
  EXPECT_STATIC_PRIMITIVE_FIELD(EXPECT_EQ, Short, "sS", "S", 1, 2);

  EXPECT_PRIMITIVE_FIELD(EXPECT_EQ, o, Boolean, "iZ", "Z", JNI_TRUE, JNI_FALSE);
  EXPECT_PRIMITIVE_FIELD(EXPECT_EQ, o, Byte, "iB", "B", 1, 2);
  EXPECT_PRIMITIVE_FIELD(EXPECT_EQ, o, Char, "iC", "C", 'a', 'b');
  EXPECT_PRIMITIVE_FIELD(EXPECT_DOUBLE_EQ, o, Double, "iD", "D", 1.0, 2.0);
  EXPECT_PRIMITIVE_FIELD(EXPECT_FLOAT_EQ, o, Float, "iF", "F", 1.0, 2.0);
  EXPECT_PRIMITIVE_FIELD(EXPECT_EQ, o, Int, "iI", "I", 1, 2);
  EXPECT_PRIMITIVE_FIELD(EXPECT_EQ, o, Long, "iJ", "J", 1, 2);
  EXPECT_PRIMITIVE_FIELD(EXPECT_EQ, o, Short, "iS", "S", 1, 2);
}

TEST_F(JniInternalTest, GetObjectField_SetObjectField) {
  Thread::Current()->TransitionFromSuspendedToRunnable();
  LoadDex("AllFields");
  runtime_->Start();

  jclass c = env_->FindClass("AllFields");
  ASSERT_NE(c, nullptr);
  jobject o = env_->AllocObject(c);
  ASSERT_NE(o, nullptr);

  jstring s1 = env_->NewStringUTF("hello");
  ASSERT_NE(s1, nullptr);
  jstring s2 = env_->NewStringUTF("world");
  ASSERT_NE(s2, nullptr);

  jfieldID s_fid = env_->GetStaticFieldID(c, "sObject", "Ljava/lang/Object;");
  ASSERT_NE(s_fid, nullptr);
  jfieldID i_fid = env_->GetFieldID(c, "iObject", "Ljava/lang/Object;");
  ASSERT_NE(i_fid, nullptr);

  env_->SetStaticObjectField(c, s_fid, s1);
  ASSERT_TRUE(env_->IsSameObject(s1, env_->GetStaticObjectField(c, s_fid)));
  env_->SetStaticObjectField(c, s_fid, s2);
  ASSERT_TRUE(env_->IsSameObject(s2, env_->GetStaticObjectField(c, s_fid)));

  env_->SetObjectField(o, i_fid, s1);
  ASSERT_TRUE(env_->IsSameObject(s1, env_->GetObjectField(o, i_fid)));
  env_->SetObjectField(o, i_fid, s2);
  ASSERT_TRUE(env_->IsSameObject(s2, env_->GetObjectField(o, i_fid)));
}

TEST_F(JniInternalTest, NewLocalRef_nullptr) {
  EXPECT_EQ(env_->NewLocalRef(nullptr), nullptr);
}

TEST_F(JniInternalTest, NewLocalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewLocalRef(s);
  EXPECT_NE(o, nullptr);
  EXPECT_NE(o, s);

  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(o));
}

TEST_F(JniInternalTest, DeleteLocalRef_nullptr) {
  env_->DeleteLocalRef(nullptr);
}

TEST_F(JniInternalTest, DeleteLocalRef) {
  // This tests leads to warnings and errors in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  env_->DeleteLocalRef(s);

  // Currently, deleting an already-deleted reference is just a CheckJNI warning.
  {
    bool old_check_jni = vm_->SetCheckJniEnabled(false);
    {
      CheckJniAbortCatcher check_jni_abort_catcher;
      env_->DeleteLocalRef(s);
    }
    CheckJniAbortCatcher check_jni_abort_catcher;
    EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
    env_->DeleteLocalRef(s);
    std::string expected(StringPrintf("use of deleted local reference %p", s));
    check_jni_abort_catcher.Check(expected.c_str());
    EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
  }

  s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewLocalRef(s);
  ASSERT_NE(o, nullptr);

  env_->DeleteLocalRef(s);
  env_->DeleteLocalRef(o);
}

TEST_F(JniInternalTest, PushLocalFrame_10395422) {
  // The JNI specification is ambiguous about whether the given capacity is to be interpreted as a
  // maximum or as a minimum, but it seems like it's supposed to be a minimum, and that's how
  // Android historically treated it, and it's how the RI treats it. It's also the more useful
  // interpretation!
  ASSERT_EQ(JNI_OK, env_->PushLocalFrame(0));
  env_->PopLocalFrame(nullptr);

  // The following two tests will print errors to the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  // Negative capacities are not allowed.
  ASSERT_EQ(JNI_ERR, env_->PushLocalFrame(-1));

  // And it's okay to have an upper limit. Ours is currently 512.
  ASSERT_EQ(JNI_ERR, env_->PushLocalFrame(8192));
}

TEST_F(JniInternalTest, PushLocalFrame_PopLocalFrame) {
  // This tests leads to errors in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  jobject original = env_->NewStringUTF("");
  ASSERT_NE(original, nullptr);

  jobject outer;
  jobject inner1, inner2;
  ScopedObjectAccess soa(env_);
  {
    ASSERT_EQ(JNI_OK, env_->PushLocalFrame(4));
    outer = env_->NewLocalRef(original);

    {
      ASSERT_EQ(JNI_OK, env_->PushLocalFrame(4));
      inner1 = env_->NewLocalRef(outer);
      inner2 = env_->NewStringUTF("survivor");
      EXPECT_NE(env_->PopLocalFrame(inner2), nullptr);
    }

    EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(original));
    EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(outer));
    {
      CheckJniAbortCatcher check_jni_abort_catcher;
      EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner1));
      check_jni_abort_catcher.Check("use of deleted local reference");
    }

    // Our local reference for the survivor is invalid because the survivor
    // gets a new local reference...
    {
      CheckJniAbortCatcher check_jni_abort_catcher;
      EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner2));
      check_jni_abort_catcher.Check("use of deleted local reference");
    }

    EXPECT_EQ(env_->PopLocalFrame(nullptr), nullptr);
  }
  EXPECT_EQ(JNILocalRefType, env_->GetObjectRefType(original));
  CheckJniAbortCatcher check_jni_abort_catcher;
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(outer));
  check_jni_abort_catcher.Check("use of deleted local reference");
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner1));
  check_jni_abort_catcher.Check("use of deleted local reference");
  EXPECT_EQ(JNIInvalidRefType, env_->GetObjectRefType(inner2));
  check_jni_abort_catcher.Check("use of deleted local reference");
}

TEST_F(JniInternalTest, NewGlobalRef_nullptr) {
  EXPECT_EQ(env_->NewGlobalRef(nullptr), nullptr);
}

TEST_F(JniInternalTest, NewGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewGlobalRef(s);
  EXPECT_NE(o, nullptr);
  EXPECT_NE(o, s);

  EXPECT_EQ(env_->GetObjectRefType(o), JNIGlobalRefType);
}

TEST_F(JniInternalTest, DeleteGlobalRef_nullptr) {
  env_->DeleteGlobalRef(nullptr);
}

TEST_F(JniInternalTest, DeleteGlobalRef) {
  // This tests leads to warnings and errors in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);

  jobject o = env_->NewGlobalRef(s);
  ASSERT_NE(o, nullptr);
  env_->DeleteGlobalRef(o);

  // Currently, deleting an already-deleted reference is just a CheckJNI warning.
  {
    bool old_check_jni = vm_->SetCheckJniEnabled(false);
    {
      CheckJniAbortCatcher check_jni_abort_catcher;
      env_->DeleteGlobalRef(o);
    }
    CheckJniAbortCatcher check_jni_abort_catcher;
    EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
    env_->DeleteGlobalRef(o);
    std::string expected(StringPrintf("use of deleted global reference %p", o));
    check_jni_abort_catcher.Check(expected.c_str());
    EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
  }

  jobject o1 = env_->NewGlobalRef(s);
  ASSERT_NE(o1, nullptr);
  jobject o2 = env_->NewGlobalRef(s);
  ASSERT_NE(o2, nullptr);

  env_->DeleteGlobalRef(o1);
  env_->DeleteGlobalRef(o2);
}

TEST_F(JniInternalTest, NewWeakGlobalRef_nullptr) {
  EXPECT_EQ(env_->NewWeakGlobalRef(nullptr),   nullptr);
}

TEST_F(JniInternalTest, NewWeakGlobalRef) {
  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);
  jobject o = env_->NewWeakGlobalRef(s);
  EXPECT_NE(o, nullptr);
  EXPECT_NE(o, s);

  EXPECT_EQ(env_->GetObjectRefType(o), JNIWeakGlobalRefType);
}

TEST_F(JniInternalTest, DeleteWeakGlobalRef_nullptr) {
  env_->DeleteWeakGlobalRef(nullptr);
}

TEST_F(JniInternalTest, DeleteWeakGlobalRef) {
  // This tests leads to warnings and errors in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  jstring s = env_->NewStringUTF("");
  ASSERT_NE(s, nullptr);

  jobject o = env_->NewWeakGlobalRef(s);
  ASSERT_NE(o, nullptr);
  env_->DeleteWeakGlobalRef(o);

  // Currently, deleting an already-deleted reference is just a CheckJNI warning.
  {
    bool old_check_jni = vm_->SetCheckJniEnabled(false);
    {
      CheckJniAbortCatcher check_jni_abort_catcher;
      env_->DeleteWeakGlobalRef(o);
    }
    CheckJniAbortCatcher check_jni_abort_catcher;
    EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
    env_->DeleteWeakGlobalRef(o);
    std::string expected(StringPrintf("use of deleted weak global reference %p", o));
    check_jni_abort_catcher.Check(expected.c_str());
    EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
  }

  jobject o1 = env_->NewWeakGlobalRef(s);
  ASSERT_NE(o1, nullptr);
  jobject o2 = env_->NewWeakGlobalRef(s);
  ASSERT_NE(o2, nullptr);

  env_->DeleteWeakGlobalRef(o1);
  env_->DeleteWeakGlobalRef(o2);
}

TEST_F(JniInternalTest, ExceptionDescribe) {
  // This checks how ExceptionDescribe handles call without exception.
  env_->ExceptionClear();
  env_->ExceptionDescribe();
}

TEST_F(JniInternalTest, Throw) {
  jclass exception_class = env_->FindClass("java/lang/RuntimeException");
  ASSERT_TRUE(exception_class != nullptr);
  jthrowable exception = reinterpret_cast<jthrowable>(env_->AllocObject(exception_class));
  ASSERT_TRUE(exception != nullptr);

  EXPECT_EQ(JNI_OK, env_->Throw(exception));
  EXPECT_TRUE(env_->ExceptionCheck());
  jthrowable thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsSameObject(exception, thrown_exception));

  // Bad argument.
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  EXPECT_EQ(JNI_ERR, env_->Throw(nullptr));
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  CheckJniAbortCatcher check_jni_abort_catcher;
  EXPECT_EQ(JNI_ERR, env_->Throw(nullptr));
  check_jni_abort_catcher.Check("Throw received NULL jthrowable");
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

TEST_F(JniInternalTest, ThrowNew) {
  jclass exception_class = env_->FindClass("java/lang/RuntimeException");
  ASSERT_TRUE(exception_class != nullptr);

  jthrowable thrown_exception;

  EXPECT_EQ(JNI_OK, env_->ThrowNew(exception_class, "hello world"));
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, exception_class));

  EXPECT_EQ(JNI_OK, env_->ThrowNew(exception_class, nullptr));
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, exception_class));

  // Bad argument.
  bool old_check_jni = vm_->SetCheckJniEnabled(false);
  CheckJniAbortCatcher check_jni_abort_catcher;
  EXPECT_EQ(JNI_ERR, env_->ThrowNew(nullptr, nullptr));
  check_jni_abort_catcher.Check("c == null");
  EXPECT_FALSE(vm_->SetCheckJniEnabled(true));
  EXPECT_EQ(JNI_ERR, env_->ThrowNew(nullptr, nullptr));
  check_jni_abort_catcher.Check("ThrowNew received NULL jclass");
  EXPECT_TRUE(vm_->SetCheckJniEnabled(old_check_jni));
}

TEST_F(JniInternalTest, NewDirectBuffer_GetDirectBufferAddress_GetDirectBufferCapacity) {
  // Start runtime.
  Thread* self = Thread::Current();
  self->TransitionFromSuspendedToRunnable();
  MakeExecutable(nullptr, "java.lang.Class");
  MakeExecutable(nullptr, "java.lang.Object");
  MakeExecutable(nullptr, "java.nio.DirectByteBuffer");
  MakeExecutable(nullptr, "java.nio.Bits");
  MakeExecutable(nullptr, "java.nio.MappedByteBuffer");
  MakeExecutable(nullptr, "java.nio.ByteBuffer");
  MakeExecutable(nullptr, "java.nio.Buffer");
  // TODO: we only load a dex file here as starting the runtime relies upon it.
  const char* class_name = "StaticLeafMethods";
  LoadDex(class_name);
  bool started = runtime_->Start();
  ASSERT_TRUE(started);

  jclass buffer_class = env_->FindClass("java/nio/Buffer");
  ASSERT_NE(buffer_class, nullptr);

  char bytes[1024];
  jobject buffer = env_->NewDirectByteBuffer(bytes, sizeof(bytes));
  ASSERT_NE(buffer, nullptr);
  ASSERT_TRUE(env_->IsInstanceOf(buffer, buffer_class));
  ASSERT_EQ(env_->GetDirectBufferAddress(buffer), bytes);
  ASSERT_EQ(env_->GetDirectBufferCapacity(buffer), static_cast<jlong>(sizeof(bytes)));

  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    env_->NewDirectByteBuffer(bytes, static_cast<jlong>(INT_MAX) + 1);
    check_jni_abort_catcher.Check("in call to NewDirectByteBuffer");
  }
}

TEST_F(JniInternalTest, MonitorEnterExit) {
  // This will print some error messages. Suppress.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  // Create an object to torture.
  jclass object_class = env_->FindClass("java/lang/Object");
  ASSERT_NE(object_class, nullptr);
  jobject object = env_->AllocObject(object_class);
  ASSERT_NE(object, nullptr);

  // Expected class of exceptions
  jclass imse_class = env_->FindClass("java/lang/IllegalMonitorStateException");
  ASSERT_NE(imse_class, nullptr);

  jthrowable thrown_exception;

  // Unlock of unowned monitor
  env_->MonitorExit(object);
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, imse_class));

  // Lock of unowned monitor
  env_->MonitorEnter(object);
  EXPECT_FALSE(env_->ExceptionCheck());
  // Regular unlock
  env_->MonitorExit(object);
  EXPECT_FALSE(env_->ExceptionCheck());

  // Recursively lock a lot
  size_t max_recursive_lock = 1024;
  for (size_t i = 0; i < max_recursive_lock; i++) {
    env_->MonitorEnter(object);
    EXPECT_FALSE(env_->ExceptionCheck());
  }
  // Recursively unlock a lot
  for (size_t i = 0; i < max_recursive_lock; i++) {
    env_->MonitorExit(object);
    EXPECT_FALSE(env_->ExceptionCheck());
  }

  // Unlock of unowned monitor
  env_->MonitorExit(object);
  EXPECT_TRUE(env_->ExceptionCheck());
  thrown_exception = env_->ExceptionOccurred();
  env_->ExceptionClear();
  EXPECT_TRUE(env_->IsInstanceOf(thrown_exception, imse_class));

  // It's an error to call MonitorEnter or MonitorExit on null.
  {
    CheckJniAbortCatcher check_jni_abort_catcher;
    env_->MonitorEnter(nullptr);
    check_jni_abort_catcher.Check("in call to MonitorEnter");
    env_->MonitorExit(nullptr);
    check_jni_abort_catcher.Check("in call to MonitorExit");
  }
}

void Java_MyClassNatives_foo_exit(JNIEnv* env, jobject thisObj) {
  // Release the monitor on self. This should trigger an abort.
  env->MonitorExit(thisObj);
}

TEST_F(JniInternalTest, MonitorExitLockedInDifferentCall) {
  SetUpForTest(false, "foo", "()V", reinterpret_cast<void*>(&Java_MyClassNatives_foo_exit));
  ASSERT_NE(jobj_, nullptr);

  env_->MonitorEnter(jobj_);
  EXPECT_FALSE(env_->ExceptionCheck());

  CheckJniAbortCatcher check_jni_abort_catcher;
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  check_jni_abort_catcher.Check("Unlocking monitor that wasn't locked here");
}

void Java_MyClassNatives_foo_enter_no_exit(JNIEnv* env, jobject thisObj) {
  // Acquire but don't release the monitor on self. This should trigger an abort on return.
  env->MonitorEnter(thisObj);
}

TEST_F(JniInternalTest, MonitorExitNotAllUnlocked) {
  SetUpForTest(false,
               "foo",
               "()V",
               reinterpret_cast<void*>(&Java_MyClassNatives_foo_enter_no_exit));
  ASSERT_NE(jobj_, nullptr);

  CheckJniAbortCatcher check_jni_abort_catcher;
  env_->CallNonvirtualVoidMethod(jobj_, jklass_, jmethod_);
  check_jni_abort_catcher.Check("Still holding a locked object on JNI end");
}

static bool IsLocked(JNIEnv* env, jobject jobj) {
  ScopedObjectAccess soa(env);
  LockWord lock_word = soa.Decode<mirror::Object*>(jobj)->GetLockWord(true);
  switch (lock_word.GetState()) {
    case LockWord::kHashCode:
    case LockWord::kUnlocked:
      return false;
    case LockWord::kThinLocked:
      return true;
    case LockWord::kFatLocked:
      return lock_word.FatLockMonitor()->IsLocked();
    default: {
      LOG(FATAL) << "Invalid monitor state " << lock_word.GetState();
      UNREACHABLE();
    }
  }
}

TEST_F(JniInternalTest, DetachThreadUnlockJNIMonitors) {
  // We need to lock an object, detach, reattach, and check the locks.
  //
  // As re-attaching will create a different thread, we need to use a global
  // ref to keep the object around.

  // Create an object to torture.
  jobject global_ref;
  {
    jclass object_class = env_->FindClass("java/lang/Object");
    ASSERT_NE(object_class, nullptr);
    jobject object = env_->AllocObject(object_class);
    ASSERT_NE(object, nullptr);
    global_ref = env_->NewGlobalRef(object);
  }

  // Lock it.
  env_->MonitorEnter(global_ref);
  ASSERT_TRUE(IsLocked(env_, global_ref));

  // Detach and re-attach.
  jint detach_result = vm_->DetachCurrentThread();
  ASSERT_EQ(detach_result, JNI_OK);
  jint attach_result = vm_->AttachCurrentThread(&env_, nullptr);
  ASSERT_EQ(attach_result, JNI_OK);

  // Look at the global ref, check whether it's still locked.
  ASSERT_FALSE(IsLocked(env_, global_ref));

  // Delete the global ref.
  env_->DeleteGlobalRef(global_ref);
}

// Test the offset computation of IndirectReferenceTable offsets. b/26071368.
TEST_F(JniInternalTest, IndirectReferenceTableOffsets) {
  // The segment_state_ field is private, and we want to avoid friend declaration. So we'll check
  // by modifying memory.
  // The parameters don't really matter here.
  IndirectReferenceTable irt(5, 5, IndirectRefKind::kGlobal, true);
  uint32_t old_state = irt.GetSegmentState();

  // Write some new state directly. We invert parts of old_state to ensure a new value.
  uint32_t new_state = old_state ^ 0x07705005;
  ASSERT_NE(old_state, new_state);

  uint8_t* base = reinterpret_cast<uint8_t*>(&irt);
  int32_t segment_state_offset =
      IndirectReferenceTable::SegmentStateOffset(sizeof(void*)).Int32Value();
  *reinterpret_cast<uint32_t*>(base + segment_state_offset) = new_state;

  // Read and compare.
  EXPECT_EQ(new_state, irt.GetSegmentState());
}

// Test the offset computation of JNIEnvExt offsets. b/26071368.
TEST_F(JniInternalTest, JNIEnvExtOffsets) {
  EXPECT_EQ(OFFSETOF_MEMBER(JNIEnvExt, local_ref_cookie),
            JNIEnvExt::LocalRefCookieOffset(sizeof(void*)).Uint32Value());

  EXPECT_EQ(OFFSETOF_MEMBER(JNIEnvExt, self), JNIEnvExt::SelfOffset(sizeof(void*)).Uint32Value());

  // segment_state_ is private in the IndirectReferenceTable. So this test isn't as good as we'd
  // hope it to be.
  uint32_t segment_state_now =
      OFFSETOF_MEMBER(JNIEnvExt, locals) +
      IndirectReferenceTable::SegmentStateOffset(sizeof(void*)).Uint32Value();
  uint32_t segment_state_computed = JNIEnvExt::SegmentStateOffset(sizeof(void*)).Uint32Value();
  EXPECT_EQ(segment_state_now, segment_state_computed);
}

}  // namespace art
