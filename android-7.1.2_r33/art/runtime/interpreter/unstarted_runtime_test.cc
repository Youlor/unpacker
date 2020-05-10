/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "unstarted_runtime.h"

#include <limits>
#include <locale>

#include "base/casts.h"
#include "base/memory_tool.h"
#include "class_linker.h"
#include "common_runtime_test.h"
#include "dex_instruction.h"
#include "handle.h"
#include "handle_scope-inl.h"
#include "interpreter/interpreter_common.h"
#include "mirror/class_loader.h"
#include "mirror/string-inl.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "transaction.h"

namespace art {
namespace interpreter {

class UnstartedRuntimeTest : public CommonRuntimeTest {
 protected:
  // Re-expose all UnstartedRuntime implementations so we don't need to declare a million
  // test friends.

  // Methods that intercept available libcore implementations.
#define UNSTARTED_DIRECT(Name, SigIgnored)                 \
  static void Unstarted ## Name(Thread* self,              \
                                ShadowFrame* shadow_frame, \
                                JValue* result,            \
                                size_t arg_offset)         \
      SHARED_REQUIRES(Locks::mutator_lock_) {        \
    interpreter::UnstartedRuntime::Unstarted ## Name(self, shadow_frame, result, arg_offset); \
  }
#include "unstarted_runtime_list.h"
  UNSTARTED_RUNTIME_DIRECT_LIST(UNSTARTED_DIRECT)
#undef UNSTARTED_RUNTIME_DIRECT_LIST
#undef UNSTARTED_RUNTIME_JNI_LIST
#undef UNSTARTED_DIRECT

  // Methods that are native.
#define UNSTARTED_JNI(Name, SigIgnored)                       \
  static void UnstartedJNI ## Name(Thread* self,              \
                                   ArtMethod* method,         \
                                   mirror::Object* receiver,  \
                                   uint32_t* args,            \
                                   JValue* result)            \
      SHARED_REQUIRES(Locks::mutator_lock_) {           \
    interpreter::UnstartedRuntime::UnstartedJNI ## Name(self, method, receiver, args, result); \
  }
#include "unstarted_runtime_list.h"
  UNSTARTED_RUNTIME_JNI_LIST(UNSTARTED_JNI)
#undef UNSTARTED_RUNTIME_DIRECT_LIST
#undef UNSTARTED_RUNTIME_JNI_LIST
#undef UNSTARTED_JNI

  // Helpers for ArrayCopy.
  //
  // Note: as we have to use handles, we use StackHandleScope to transfer data. Hardcode a size
  //       of three everywhere. That is enough to test all cases.

  static mirror::ObjectArray<mirror::Object>* CreateObjectArray(
      Thread* self,
      mirror::Class* component_type,
      const StackHandleScope<3>& data)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    mirror::Class* array_type = runtime->GetClassLinker()->FindArrayClass(self, &component_type);
    CHECK(array_type != nullptr);
    mirror::ObjectArray<mirror::Object>* result =
        mirror::ObjectArray<mirror::Object>::Alloc(self, array_type, 3);
    CHECK(result != nullptr);
    for (size_t i = 0; i < 3; ++i) {
      result->Set(static_cast<int32_t>(i), data.GetReference(i));
      CHECK(!self->IsExceptionPending());
    }
    return result;
  }

  static void CheckObjectArray(mirror::ObjectArray<mirror::Object>* array,
                               const StackHandleScope<3>& data)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    CHECK_EQ(array->GetLength(), 3);
    CHECK_EQ(data.NumberOfReferences(), 3U);
    for (size_t i = 0; i < 3; ++i) {
      EXPECT_EQ(data.GetReference(i), array->Get(static_cast<int32_t>(i))) << i;
    }
  }

  void RunArrayCopy(Thread* self,
                    ShadowFrame* tmp,
                    bool expect_exception,
                    mirror::ObjectArray<mirror::Object>* src,
                    int32_t src_pos,
                    mirror::ObjectArray<mirror::Object>* dst,
                    int32_t dst_pos,
                    int32_t length)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    JValue result;
    tmp->SetVRegReference(0, src);
    tmp->SetVReg(1, src_pos);
    tmp->SetVRegReference(2, dst);
    tmp->SetVReg(3, dst_pos);
    tmp->SetVReg(4, length);
    UnstartedSystemArraycopy(self, tmp, &result, 0);
    bool exception_pending = self->IsExceptionPending();
    EXPECT_EQ(exception_pending, expect_exception);
    if (exception_pending) {
      self->ClearException();
    }
  }

  void RunArrayCopy(Thread* self,
                    ShadowFrame* tmp,
                    bool expect_exception,
                    mirror::Class* src_component_class,
                    mirror::Class* dst_component_class,
                    const StackHandleScope<3>& src_data,
                    int32_t src_pos,
                    const StackHandleScope<3>& dst_data,
                    int32_t dst_pos,
                    int32_t length,
                    const StackHandleScope<3>& expected_result)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    StackHandleScope<3> hs_misc(self);
    Handle<mirror::Class> dst_component_handle(hs_misc.NewHandle(dst_component_class));

    Handle<mirror::ObjectArray<mirror::Object>> src_handle(
        hs_misc.NewHandle(CreateObjectArray(self, src_component_class, src_data)));

    Handle<mirror::ObjectArray<mirror::Object>> dst_handle(
        hs_misc.NewHandle(CreateObjectArray(self, dst_component_handle.Get(), dst_data)));

    RunArrayCopy(self,
                 tmp,
                 expect_exception,
                 src_handle.Get(),
                 src_pos,
                 dst_handle.Get(),
                 dst_pos,
                 length);
    CheckObjectArray(dst_handle.Get(), expected_result);
  }

  void TestCeilFloor(bool ceil,
                     Thread* self,
                     ShadowFrame* tmp,
                     double const test_pairs[][2],
                     size_t num_pairs)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    for (size_t i = 0; i < num_pairs; ++i) {
      tmp->SetVRegDouble(0, test_pairs[i][0]);

      JValue result;
      if (ceil) {
        UnstartedMathCeil(self, tmp, &result, 0);
      } else {
        UnstartedMathFloor(self, tmp, &result, 0);
      }

      ASSERT_FALSE(self->IsExceptionPending());

      // We want precise results.
      int64_t result_int64t = bit_cast<int64_t, double>(result.GetD());
      int64_t expect_int64t = bit_cast<int64_t, double>(test_pairs[i][1]);
      EXPECT_EQ(expect_int64t, result_int64t) << result.GetD() << " vs " << test_pairs[i][1];
    }
  }

  // Prepare for aborts. Aborts assume that the exception class is already resolved, as the
  // loading code doesn't work under transactions.
  void PrepareForAborts() SHARED_REQUIRES(Locks::mutator_lock_) {
    mirror::Object* result = Runtime::Current()->GetClassLinker()->FindClass(
        Thread::Current(),
        Transaction::kAbortExceptionSignature,
        ScopedNullHandle<mirror::ClassLoader>());
    CHECK(result != nullptr);
  }
};

TEST_F(UnstartedRuntimeTest, MemoryPeekByte) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  for (int32_t i = 0; i < kBaseLen; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekByte(self, tmp, &result, 0);

    EXPECT_EQ(result.GetB(), static_cast<int8_t>(base_array[i]));
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, MemoryPeekShort) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int16_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekShort(self, tmp, &result, 0);

    typedef int16_t unaligned_short __attribute__ ((aligned (1)));
    const unaligned_short* short_ptr = reinterpret_cast<const unaligned_short*>(base_ptr + i);
    EXPECT_EQ(result.GetS(), *short_ptr);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, MemoryPeekInt) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int32_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekInt(self, tmp, &result, 0);

    typedef int32_t unaligned_int __attribute__ ((aligned (1)));
    const unaligned_int* int_ptr = reinterpret_cast<const unaligned_int*>(base_ptr + i);
    EXPECT_EQ(result.GetI(), *int_ptr);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, MemoryPeekLong) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  constexpr const uint8_t base_array[] = "abcdefghijklmnop";
  constexpr int32_t kBaseLen = sizeof(base_array) / sizeof(uint8_t);
  const uint8_t* base_ptr = base_array;

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  int32_t adjusted_length = kBaseLen - sizeof(int64_t);
  for (int32_t i = 0; i < adjusted_length; ++i) {
    tmp->SetVRegLong(0, static_cast<int64_t>(reinterpret_cast<intptr_t>(base_ptr + i)));

    UnstartedMemoryPeekLong(self, tmp, &result, 0);

    typedef int64_t unaligned_long __attribute__ ((aligned (1)));
    const unaligned_long* long_ptr = reinterpret_cast<const unaligned_long*>(base_ptr + i);
    EXPECT_EQ(result.GetJ(), *long_ptr);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, StringGetCharsNoCheck) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  StackHandleScope<2> hs(self);
  // TODO: Actual UTF.
  constexpr const char base_string[] = "abcdefghijklmnop";
  Handle<mirror::String> h_test_string(hs.NewHandle(
      mirror::String::AllocFromModifiedUtf8(self, base_string)));
  constexpr int32_t kBaseLen = sizeof(base_string) / sizeof(char) - 1;
  Handle<mirror::CharArray> h_char_array(hs.NewHandle(
      mirror::CharArray::Alloc(self, kBaseLen)));
  // A buffer so we can make sure we only modify the elements targetted.
  uint16_t buf[kBaseLen];

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  for (int32_t start_index = 0; start_index < kBaseLen; ++start_index) {
    for (int32_t count = 0; count <= kBaseLen; ++count) {
      for (int32_t trg_offset = 0; trg_offset < kBaseLen; ++trg_offset) {
        // Only do it when in bounds.
        if (start_index + count <= kBaseLen && trg_offset + count <= kBaseLen) {
          tmp->SetVRegReference(0, h_test_string.Get());
          tmp->SetVReg(1, start_index);
          tmp->SetVReg(2, count);
          tmp->SetVRegReference(3, h_char_array.Get());
          tmp->SetVReg(3, trg_offset);

          // Copy the char_array into buf.
          memcpy(buf, h_char_array->GetData(), kBaseLen * sizeof(uint16_t));

          UnstartedStringCharAt(self, tmp, &result, 0);

          uint16_t* data = h_char_array->GetData();

          bool success = true;

          // First segment should be unchanged.
          for (int32_t i = 0; i < trg_offset; ++i) {
            success = success && (data[i] == buf[i]);
          }
          // Second segment should be a copy.
          for (int32_t i = trg_offset; i < trg_offset + count; ++i) {
            success = success && (data[i] == buf[i - trg_offset + start_index]);
          }
          // Third segment should be unchanged.
          for (int32_t i = trg_offset + count; i < kBaseLen; ++i) {
            success = success && (data[i] == buf[i]);
          }

          EXPECT_TRUE(success);
        }
      }
    }
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, StringCharAt) {
  Thread* self = Thread::Current();

  ScopedObjectAccess soa(self);
  // TODO: Actual UTF.
  constexpr const char* base_string = "abcdefghijklmnop";
  int32_t base_len = static_cast<int32_t>(strlen(base_string));
  mirror::String* test_string = mirror::String::AllocFromModifiedUtf8(self, base_string);

  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  for (int32_t i = 0; i < base_len; ++i) {
    tmp->SetVRegReference(0, test_string);
    tmp->SetVReg(1, i);

    UnstartedStringCharAt(self, tmp, &result, 0);

    EXPECT_EQ(result.GetI(), base_string[i]);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, StringInit) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  mirror::Class* klass = mirror::String::GetJavaLangString();
  ArtMethod* method = klass->FindDeclaredDirectMethod("<init>", "(Ljava/lang/String;)V",
                                                      sizeof(void*));

  // create instruction data for invoke-direct {v0, v1} of method with fake index
  uint16_t inst_data[3] = { 0x2070, 0x0000, 0x0010 };
  const Instruction* inst = Instruction::At(inst_data);

  JValue result;
  ShadowFrame* shadow_frame = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, method, 0);
  const char* base_string = "hello_world";
  mirror::String* string_arg = mirror::String::AllocFromModifiedUtf8(self, base_string);
  mirror::String* reference_empty_string = mirror::String::AllocFromModifiedUtf8(self, "");
  shadow_frame->SetVRegReference(0, reference_empty_string);
  shadow_frame->SetVRegReference(1, string_arg);

  interpreter::DoCall<false, false>(method, self, *shadow_frame, inst, inst_data[0], &result);
  mirror::String* string_result = reinterpret_cast<mirror::String*>(result.GetL());
  EXPECT_EQ(string_arg->GetLength(), string_result->GetLength());
  EXPECT_EQ(memcmp(string_arg->GetValue(), string_result->GetValue(),
                   string_arg->GetLength() * sizeof(uint16_t)), 0);

  ShadowFrame::DeleteDeoptimizedFrame(shadow_frame);
}

// Tests the exceptions that should be checked before modifying the destination.
// (Doesn't check the object vs primitive case ATM.)
TEST_F(UnstartedRuntimeTest, SystemArrayCopyObjectArrayTestExceptions) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  // Note: all tests are not GC safe. Assume there's no GC running here with the few objects we
  //       allocate.
  StackHandleScope<2> hs_misc(self);
  Handle<mirror::Class> object_class(
      hs_misc.NewHandle(mirror::Class::GetJavaLangClass()->GetSuperClass()));

  StackHandleScope<3> hs_data(self);
  hs_data.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
  hs_data.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "2"));
  hs_data.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

  Handle<mirror::ObjectArray<mirror::Object>> array(
      hs_misc.NewHandle(CreateObjectArray(self, object_class.Get(), hs_data)));

  RunArrayCopy(self, tmp, true, array.Get(), -1, array.Get(), 0, 0);
  RunArrayCopy(self, tmp, true, array.Get(), 0, array.Get(), -1, 0);
  RunArrayCopy(self, tmp, true, array.Get(), 0, array.Get(), 0, -1);
  RunArrayCopy(self, tmp, true, array.Get(), 0, array.Get(), 0, 4);
  RunArrayCopy(self, tmp, true, array.Get(), 0, array.Get(), 1, 3);
  RunArrayCopy(self, tmp, true, array.Get(), 1, array.Get(), 0, 3);

  mirror::ObjectArray<mirror::Object>* class_as_array =
      reinterpret_cast<mirror::ObjectArray<mirror::Object>*>(object_class.Get());
  RunArrayCopy(self, tmp, true, class_as_array, 0, array.Get(), 0, 0);
  RunArrayCopy(self, tmp, true, array.Get(), 0, class_as_array, 0, 0);

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, SystemArrayCopyObjectArrayTest) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  JValue result;
  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  StackHandleScope<1> hs_object(self);
  Handle<mirror::Class> object_class(
      hs_object.NewHandle(mirror::Class::GetJavaLangClass()->GetSuperClass()));

  // Simple test:
  // [1,2,3]{1 @ 2} into [4,5,6] = [4,2,6]
  {
    StackHandleScope<3> hs_src(self);
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "2"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

    StackHandleScope<3> hs_dst(self);
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "4"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "5"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "6"));

    StackHandleScope<3> hs_expected(self);
    hs_expected.NewHandle(hs_dst.GetReference(0));
    hs_expected.NewHandle(hs_dst.GetReference(1));
    hs_expected.NewHandle(hs_src.GetReference(1));

    RunArrayCopy(self,
                 tmp,
                 false,
                 object_class.Get(),
                 object_class.Get(),
                 hs_src,
                 1,
                 hs_dst,
                 2,
                 1,
                 hs_expected);
  }

  // Simple test:
  // [1,2,3]{1 @ 1} into [4,5,6] = [4,2,6]  (with dst String[])
  {
    StackHandleScope<3> hs_src(self);
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "2"));
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

    StackHandleScope<3> hs_dst(self);
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "4"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "5"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "6"));

    StackHandleScope<3> hs_expected(self);
    hs_expected.NewHandle(hs_dst.GetReference(0));
    hs_expected.NewHandle(hs_src.GetReference(1));
    hs_expected.NewHandle(hs_dst.GetReference(2));

    RunArrayCopy(self,
                 tmp,
                 false,
                 object_class.Get(),
                 mirror::String::GetJavaLangString(),
                 hs_src,
                 1,
                 hs_dst,
                 1,
                 1,
                 hs_expected);
  }

  // Simple test:
  // [1,*,3] into [4,5,6] = [1,5,6] + exc
  {
    StackHandleScope<3> hs_src(self);
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "1"));
    hs_src.NewHandle(mirror::String::GetJavaLangString());
    hs_src.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "3"));

    StackHandleScope<3> hs_dst(self);
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "4"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "5"));
    hs_dst.NewHandle(mirror::String::AllocFromModifiedUtf8(self, "6"));

    StackHandleScope<3> hs_expected(self);
    hs_expected.NewHandle(hs_src.GetReference(0));
    hs_expected.NewHandle(hs_dst.GetReference(1));
    hs_expected.NewHandle(hs_dst.GetReference(2));

    RunArrayCopy(self,
                 tmp,
                 true,
                 object_class.Get(),
                 mirror::String::GetJavaLangString(),
                 hs_src,
                 0,
                 hs_dst,
                 0,
                 3,
                 hs_expected);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, IntegerParseIntTest) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  // Test string. Should be valid, and between minimal values of LONG_MIN and LONG_MAX (for all
  // suffixes).
  constexpr const char* test_string = "-2147483646";
  constexpr int32_t test_values[] = {
                6,
               46,
              646,
             3646,
            83646,
           483646,
          7483646,
         47483646,
        147483646,
       2147483646,
      -2147483646
  };

  static_assert(arraysize(test_values) == 11U, "test_values");
  CHECK_EQ(strlen(test_string), 11U);

  for (size_t i = 0; i <= 10; ++i) {
    const char* test_value = &test_string[10 - i];

    StackHandleScope<1> hs_str(self);
    Handle<mirror::String> h_str(
        hs_str.NewHandle(mirror::String::AllocFromModifiedUtf8(self, test_value)));
    ASSERT_NE(h_str.Get(), nullptr);
    ASSERT_FALSE(self->IsExceptionPending());

    tmp->SetVRegReference(0, h_str.Get());

    JValue result;
    UnstartedIntegerParseInt(self, tmp, &result, 0);

    ASSERT_FALSE(self->IsExceptionPending());
    EXPECT_EQ(result.GetI(), test_values[i]);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

// Right now the same as Integer.Parse
TEST_F(UnstartedRuntimeTest, LongParseLongTest) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  // Test string. Should be valid, and between minimal values of LONG_MIN and LONG_MAX (for all
  // suffixes).
  constexpr const char* test_string = "-2147483646";
  constexpr int64_t test_values[] = {
                6,
               46,
              646,
             3646,
            83646,
           483646,
          7483646,
         47483646,
        147483646,
       2147483646,
      -2147483646
  };

  static_assert(arraysize(test_values) == 11U, "test_values");
  CHECK_EQ(strlen(test_string), 11U);

  for (size_t i = 0; i <= 10; ++i) {
    const char* test_value = &test_string[10 - i];

    StackHandleScope<1> hs_str(self);
    Handle<mirror::String> h_str(
        hs_str.NewHandle(mirror::String::AllocFromModifiedUtf8(self, test_value)));
    ASSERT_NE(h_str.Get(), nullptr);
    ASSERT_FALSE(self->IsExceptionPending());

    tmp->SetVRegReference(0, h_str.Get());

    JValue result;
    UnstartedLongParseLong(self, tmp, &result, 0);

    ASSERT_FALSE(self->IsExceptionPending());
    EXPECT_EQ(result.GetJ(), test_values[i]);
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, Ceil) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  constexpr double inf = std::numeric_limits<double>::infinity();
  constexpr double ld1 = static_cast<double>((UINT64_C(1) << 53) - 1);
  constexpr double ld2 = static_cast<double>(UINT64_C(1) << 55);
  constexpr double test_pairs[][2] = {
      { -0.0, -0.0 },
      {  0.0,  0.0 },
      { -0.5, -0.0 },
      { -1.0, -1.0 },
      {  0.5,  1.0 },
      {  1.0,  1.0 },
      {  nan,  nan },
      {  inf,  inf },
      { -inf, -inf },
      {  ld1,  ld1 },
      {  ld2,  ld2 }
  };

  TestCeilFloor(true /* ceil */, self, tmp, test_pairs, arraysize(test_pairs));

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, Floor) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  constexpr double nan = std::numeric_limits<double>::quiet_NaN();
  constexpr double inf = std::numeric_limits<double>::infinity();
  constexpr double ld1 = static_cast<double>((UINT64_C(1) << 53) - 1);
  constexpr double ld2 = static_cast<double>(UINT64_C(1) << 55);
  constexpr double test_pairs[][2] = {
      { -0.0, -0.0 },
      {  0.0,  0.0 },
      { -0.5, -1.0 },
      { -1.0, -1.0 },
      {  0.5,  0.0 },
      {  1.0,  1.0 },
      {  nan,  nan },
      {  inf,  inf },
      { -inf, -inf },
      {  ld1,  ld1 },
      {  ld2,  ld2 }
  };

  TestCeilFloor(false /* floor */, self, tmp, test_pairs, arraysize(test_pairs));

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, ToLowerUpper) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  std::locale c_locale("C");

  // Check ASCII.
  for (uint32_t i = 0; i < 128; ++i) {
    bool c_upper = std::isupper(static_cast<char>(i), c_locale);
    bool c_lower = std::islower(static_cast<char>(i), c_locale);
    EXPECT_FALSE(c_upper && c_lower) << i;

    // Check toLowerCase.
    {
      JValue result;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      UnstartedCharacterToLowerCase(self, tmp, &result, 0);
      ASSERT_FALSE(self->IsExceptionPending());
      uint32_t lower_result = static_cast<uint32_t>(result.GetI());
      if (c_lower) {
        EXPECT_EQ(i, lower_result);
      } else if (c_upper) {
        EXPECT_EQ(static_cast<uint32_t>(std::tolower(static_cast<char>(i), c_locale)),
                  lower_result);
      } else {
        EXPECT_EQ(i, lower_result);
      }
    }

    // Check toUpperCase.
    {
      JValue result2;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      UnstartedCharacterToUpperCase(self, tmp, &result2, 0);
      ASSERT_FALSE(self->IsExceptionPending());
      uint32_t upper_result = static_cast<uint32_t>(result2.GetI());
      if (c_upper) {
        EXPECT_EQ(i, upper_result);
      } else if (c_lower) {
        EXPECT_EQ(static_cast<uint32_t>(std::toupper(static_cast<char>(i), c_locale)),
                  upper_result);
      } else {
        EXPECT_EQ(i, upper_result);
      }
    }
  }

  // Check abort for other things. Can't test all.

  PrepareForAborts();

  for (uint32_t i = 128; i < 256; ++i) {
    {
      JValue result;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      Transaction transaction;
      Runtime::Current()->EnterTransactionMode(&transaction);
      UnstartedCharacterToLowerCase(self, tmp, &result, 0);
      Runtime::Current()->ExitTransactionMode();
      ASSERT_TRUE(self->IsExceptionPending());
      ASSERT_TRUE(transaction.IsAborted());
    }
    {
      JValue result;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      Transaction transaction;
      Runtime::Current()->EnterTransactionMode(&transaction);
      UnstartedCharacterToUpperCase(self, tmp, &result, 0);
      Runtime::Current()->ExitTransactionMode();
      ASSERT_TRUE(self->IsExceptionPending());
      ASSERT_TRUE(transaction.IsAborted());
    }
  }
  for (uint64_t i = 256; i <= std::numeric_limits<uint32_t>::max(); i <<= 1) {
    {
      JValue result;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      Transaction transaction;
      Runtime::Current()->EnterTransactionMode(&transaction);
      UnstartedCharacterToLowerCase(self, tmp, &result, 0);
      Runtime::Current()->ExitTransactionMode();
      ASSERT_TRUE(self->IsExceptionPending());
      ASSERT_TRUE(transaction.IsAborted());
    }
    {
      JValue result;
      tmp->SetVReg(0, static_cast<int32_t>(i));
      Transaction transaction;
      Runtime::Current()->EnterTransactionMode(&transaction);
      UnstartedCharacterToUpperCase(self, tmp, &result, 0);
      Runtime::Current()->ExitTransactionMode();
      ASSERT_TRUE(self->IsExceptionPending());
      ASSERT_TRUE(transaction.IsAborted());
    }
  }

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, Sin) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  // Test an important value, PI/6. That's the one we see in practice.
  constexpr uint64_t lvalue = UINT64_C(0x3fe0c152382d7365);
  tmp->SetVRegLong(0, static_cast<int64_t>(lvalue));

  JValue result;
  UnstartedMathSin(self, tmp, &result, 0);

  const uint64_t lresult = static_cast<uint64_t>(result.GetJ());
  EXPECT_EQ(UINT64_C(0x3fdfffffffffffff), lresult);

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, Cos) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  // Test an important value, PI/6. That's the one we see in practice.
  constexpr uint64_t lvalue = UINT64_C(0x3fe0c152382d7365);
  tmp->SetVRegLong(0, static_cast<int64_t>(lvalue));

  JValue result;
  UnstartedMathCos(self, tmp, &result, 0);

  const uint64_t lresult = static_cast<uint64_t>(result.GetJ());
  EXPECT_EQ(UINT64_C(0x3febb67ae8584cab), lresult);

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

TEST_F(UnstartedRuntimeTest, Pow) {
  // Valgrind seems to get this wrong, actually. Disable for valgrind.
  if (RUNNING_ON_MEMORY_TOOL != 0 && kMemoryToolIsValgrind) {
    return;
  }

  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  ShadowFrame* tmp = ShadowFrame::CreateDeoptimizedFrame(10, nullptr, nullptr, 0);

  // Test an important pair.
  constexpr uint64_t lvalue1 = UINT64_C(0x4079000000000000);
  constexpr uint64_t lvalue2 = UINT64_C(0xbfe6db6dc0000000);

  tmp->SetVRegLong(0, static_cast<int64_t>(lvalue1));
  tmp->SetVRegLong(2, static_cast<int64_t>(lvalue2));

  JValue result;
  UnstartedMathPow(self, tmp, &result, 0);

  const uint64_t lresult = static_cast<uint64_t>(result.GetJ());
  EXPECT_EQ(UINT64_C(0x3f8c5c51326aa7ee), lresult);

  ShadowFrame::DeleteDeoptimizedFrame(tmp);
}

}  // namespace interpreter
}  // namespace art
