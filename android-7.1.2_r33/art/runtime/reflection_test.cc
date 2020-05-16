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

#include "reflection.h"

#include <float.h>
#include <limits.h>
#include "ScopedLocalRef.h"

#include "art_method-inl.h"
#include "common_compiler_test.h"
#include "scoped_thread_state_change.h"

namespace art {

// TODO: Convert to CommonRuntimeTest. Currently MakeExecutable is used.
class ReflectionTest : public CommonCompilerTest {
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

  virtual void TearDown() {
    CleanUpJniEnv();
    CommonCompilerTest::TearDown();
  }

  jclass GetPrimitiveClass(char descriptor) {
    ScopedObjectAccess soa(env_);
    mirror::Class* c = class_linker_->FindPrimitiveClass(descriptor);
    CHECK(c != nullptr);
    return soa.AddLocalReference<jclass>(c);
  }

  void ReflectionTestMakeExecutable(ArtMethod** method,
                                    mirror::Object** receiver,
                                    bool is_static, const char* method_name,
                                    const char* method_signature)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    const char* class_name = is_static ? "StaticLeafMethods" : "NonStaticLeafMethods";
    jobject jclass_loader(LoadDex(class_name));
    Thread* self = Thread::Current();
    StackHandleScope<2> hs(self);
    Handle<mirror::ClassLoader> class_loader(
        hs.NewHandle(
            ScopedObjectAccessUnchecked(self).Decode<mirror::ClassLoader*>(jclass_loader)));
    if (is_static) {
      MakeExecutable(ScopedObjectAccessUnchecked(self).Decode<mirror::ClassLoader*>(jclass_loader),
                     class_name);
    } else {
      MakeExecutable(nullptr, "java.lang.Class");
      MakeExecutable(nullptr, "java.lang.Object");
      MakeExecutable(ScopedObjectAccessUnchecked(self).Decode<mirror::ClassLoader*>(jclass_loader),
                     class_name);
    }

    mirror::Class* c = class_linker_->FindClass(self, DotToDescriptor(class_name).c_str(),
                                                class_loader);
    CHECK(c != nullptr);

    *method = is_static ? c->FindDirectMethod(method_name, method_signature, sizeof(void*))
                        : c->FindVirtualMethod(method_name, method_signature, sizeof(void*));
    CHECK(method != nullptr);

    if (is_static) {
      *receiver = nullptr;
    } else {
      // Ensure class is initialized before allocating object
      StackHandleScope<1> hs2(self);
      Handle<mirror::Class> h_class(hs2.NewHandle(c));
      bool initialized = class_linker_->EnsureInitialized(self, h_class, true, true);
      CHECK(initialized);
      *receiver = c->AllocObject(self);
    }

    // Start runtime.
    bool started = runtime_->Start();
    CHECK(started);
    self->TransitionFromSuspendedToRunnable();
  }

  void InvokeNopMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "nop", "()V");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), nullptr);
  }

  void InvokeIdentityByteMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "identity", "(B)B");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[1];

    args[0].b = 0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(0, result.GetB());

    args[0].b = -1;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-1, result.GetB());

    args[0].b = SCHAR_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(SCHAR_MAX, result.GetB());

    static_assert(SCHAR_MIN == -128, "SCHAR_MIN unexpected");
    args[0].b = SCHAR_MIN;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(SCHAR_MIN, result.GetB());
  }

  void InvokeIdentityIntMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "identity", "(I)I");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[1];

    args[0].i = 0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(0, result.GetI());

    args[0].i = -1;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-1, result.GetI());

    args[0].i = INT_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(INT_MAX, result.GetI());

    args[0].i = INT_MIN;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(INT_MIN, result.GetI());
  }

  void InvokeIdentityDoubleMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "identity", "(D)D");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[1];

    args[0].d = 0.0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(0.0, result.GetD());

    args[0].d = -1.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(-1.0, result.GetD());

    args[0].d = DBL_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(DBL_MAX, result.GetD());

    args[0].d = DBL_MIN;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(DBL_MIN, result.GetD());
  }

  void InvokeSumIntIntMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(II)I");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[2];

    args[0].i = 1;
    args[1].i = 2;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(3, result.GetI());

    args[0].i = -2;
    args[1].i = 5;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(3, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MIN;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-1, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-2, result.GetI());
  }

  void InvokeSumIntIntIntMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(III)I");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[3];

    args[0].i = 0;
    args[1].i = 0;
    args[2].i = 0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(0, result.GetI());

    args[0].i = 1;
    args[1].i = 2;
    args[2].i = 3;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(6, result.GetI());

    args[0].i = -1;
    args[1].i = 2;
    args[2].i = -3;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-2, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MIN;
    args[2].i = INT_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(2147483646, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MAX;
    args[2].i = INT_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(2147483645, result.GetI());
  }

  void InvokeSumIntIntIntIntMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(IIII)I");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[4];

    args[0].i = 0;
    args[1].i = 0;
    args[2].i = 0;
    args[3].i = 0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(0, result.GetI());

    args[0].i = 1;
    args[1].i = 2;
    args[2].i = 3;
    args[3].i = 4;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(10, result.GetI());

    args[0].i = -1;
    args[1].i = 2;
    args[2].i = -3;
    args[3].i = 4;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(2, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MIN;
    args[2].i = INT_MAX;
    args[3].i = INT_MIN;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-2, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MAX;
    args[2].i = INT_MAX;
    args[3].i = INT_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-4, result.GetI());
  }

  void InvokeSumIntIntIntIntIntMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(IIIII)I");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[5];

    args[0].i = 0;
    args[1].i = 0;
    args[2].i = 0;
    args[3].i = 0;
    args[4].i = 0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(0, result.GetI());

    args[0].i = 1;
    args[1].i = 2;
    args[2].i = 3;
    args[3].i = 4;
    args[4].i = 5;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(15, result.GetI());

    args[0].i = -1;
    args[1].i = 2;
    args[2].i = -3;
    args[3].i = 4;
    args[4].i = -5;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(-3, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MIN;
    args[2].i = INT_MAX;
    args[3].i = INT_MIN;
    args[4].i = INT_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(2147483645, result.GetI());

    args[0].i = INT_MAX;
    args[1].i = INT_MAX;
    args[2].i = INT_MAX;
    args[3].i = INT_MAX;
    args[4].i = INT_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_EQ(2147483643, result.GetI());
  }

  void InvokeSumDoubleDoubleMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(DD)D");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[2];

    args[0].d = 0.0;
    args[1].d = 0.0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(0.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = 2.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(3.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = -2.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(-1.0, result.GetD());

    args[0].d = DBL_MAX;
    args[1].d = DBL_MIN;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(1.7976931348623157e308, result.GetD());

    args[0].d = DBL_MAX;
    args[1].d = DBL_MAX;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(INFINITY, result.GetD());
  }

  void InvokeSumDoubleDoubleDoubleMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(DDD)D");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[3];

    args[0].d = 0.0;
    args[1].d = 0.0;
    args[2].d = 0.0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(0.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = 2.0;
    args[2].d = 3.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(6.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = -2.0;
    args[2].d = 3.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(2.0, result.GetD());
  }

  void InvokeSumDoubleDoubleDoubleDoubleMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(DDDD)D");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[4];

    args[0].d = 0.0;
    args[1].d = 0.0;
    args[2].d = 0.0;
    args[3].d = 0.0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(0.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = 2.0;
    args[2].d = 3.0;
    args[3].d = 4.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(10.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = -2.0;
    args[2].d = 3.0;
    args[3].d = -4.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(-2.0, result.GetD());
  }

  void InvokeSumDoubleDoubleDoubleDoubleDoubleMethod(bool is_static) {
    ScopedObjectAccess soa(env_);
    ArtMethod* method;
    mirror::Object* receiver;
    ReflectionTestMakeExecutable(&method, &receiver, is_static, "sum", "(DDDDD)D");
    ScopedLocalRef<jobject> receiver_ref(soa.Env(), soa.AddLocalReference<jobject>(receiver));
    jvalue args[5];

    args[0].d = 0.0;
    args[1].d = 0.0;
    args[2].d = 0.0;
    args[3].d = 0.0;
    args[4].d = 0.0;
    JValue result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(0.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = 2.0;
    args[2].d = 3.0;
    args[3].d = 4.0;
    args[4].d = 5.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(15.0, result.GetD());

    args[0].d = 1.0;
    args[1].d = -2.0;
    args[2].d = 3.0;
    args[3].d = -4.0;
    args[4].d = 5.0;
    result = InvokeWithJValues(soa, receiver_ref.get(), soa.EncodeMethod(method), args);
    EXPECT_DOUBLE_EQ(3.0, result.GetD());
  }

  JavaVMExt* vm_;
  JNIEnv* env_;
  jclass aioobe_;
  jclass ase_;
  jclass sioobe_;
};

TEST_F(ReflectionTest, StaticMainMethod) {
  TEST_DISABLED_FOR_READ_BARRIER_WITH_OPTIMIZING_FOR_UNSUPPORTED_INSTRUCTION_SETS();
  ScopedObjectAccess soa(Thread::Current());
  jobject jclass_loader = LoadDex("Main");
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader(
      hs.NewHandle(soa.Decode<mirror::ClassLoader*>(jclass_loader)));
  CompileDirectMethod(class_loader, "Main", "main", "([Ljava/lang/String;)V");

  mirror::Class* klass = class_linker_->FindClass(soa.Self(), "LMain;", class_loader);
  ASSERT_TRUE(klass != nullptr);

  ArtMethod* method = klass->FindDirectMethod("main", "([Ljava/lang/String;)V", sizeof(void*));
  ASSERT_TRUE(method != nullptr);

  // Start runtime.
  bool started = runtime_->Start();
  CHECK(started);
  soa.Self()->TransitionFromSuspendedToRunnable();

  jvalue args[1];
  args[0].l = nullptr;
  InvokeWithJValues(soa, nullptr, soa.EncodeMethod(method), args);
}

TEST_F(ReflectionTest, StaticNopMethod) {
  InvokeNopMethod(true);
}

TEST_F(ReflectionTest, NonStaticNopMethod) {
  InvokeNopMethod(false);
}

TEST_F(ReflectionTest, StaticIdentityByteMethod) {
  InvokeIdentityByteMethod(true);
}

TEST_F(ReflectionTest, NonStaticIdentityByteMethod) {
  InvokeIdentityByteMethod(false);
}

TEST_F(ReflectionTest, StaticIdentityIntMethod) {
  InvokeIdentityIntMethod(true);
}

TEST_F(ReflectionTest, NonStaticIdentityIntMethod) {
  InvokeIdentityIntMethod(false);
}

TEST_F(ReflectionTest, StaticIdentityDoubleMethod) {
  InvokeIdentityDoubleMethod(true);
}

TEST_F(ReflectionTest, NonStaticIdentityDoubleMethod) {
  InvokeIdentityDoubleMethod(false);
}

TEST_F(ReflectionTest, StaticSumIntIntMethod) {
  InvokeSumIntIntMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumIntIntMethod) {
  InvokeSumIntIntMethod(false);
}

TEST_F(ReflectionTest, StaticSumIntIntIntMethod) {
  InvokeSumIntIntIntMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumIntIntIntMethod) {
  InvokeSumIntIntIntMethod(false);
}

TEST_F(ReflectionTest, StaticSumIntIntIntIntMethod) {
  InvokeSumIntIntIntIntMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumIntIntIntIntMethod) {
  InvokeSumIntIntIntIntMethod(false);
}

TEST_F(ReflectionTest, StaticSumIntIntIntIntIntMethod) {
  InvokeSumIntIntIntIntIntMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumIntIntIntIntIntMethod) {
  InvokeSumIntIntIntIntIntMethod(false);
}

TEST_F(ReflectionTest, StaticSumDoubleDoubleMethod) {
  InvokeSumDoubleDoubleMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumDoubleDoubleMethod) {
  InvokeSumDoubleDoubleMethod(false);
}

TEST_F(ReflectionTest, StaticSumDoubleDoubleDoubleMethod) {
  InvokeSumDoubleDoubleDoubleMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumDoubleDoubleDoubleMethod) {
  InvokeSumDoubleDoubleDoubleMethod(false);
}

TEST_F(ReflectionTest, StaticSumDoubleDoubleDoubleDoubleMethod) {
  InvokeSumDoubleDoubleDoubleDoubleMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumDoubleDoubleDoubleDoubleMethod) {
  InvokeSumDoubleDoubleDoubleDoubleMethod(false);
}

TEST_F(ReflectionTest, StaticSumDoubleDoubleDoubleDoubleDoubleMethod) {
  InvokeSumDoubleDoubleDoubleDoubleDoubleMethod(true);
}

TEST_F(ReflectionTest, NonStaticSumDoubleDoubleDoubleDoubleDoubleMethod) {
  InvokeSumDoubleDoubleDoubleDoubleDoubleMethod(false);
}

}  // namespace art
