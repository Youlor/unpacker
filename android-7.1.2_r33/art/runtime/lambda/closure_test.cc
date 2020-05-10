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

#include "art_method.h"
#include "lambda/art_lambda_method.h"
#include "lambda/closure.h"
#include "lambda/closure_builder.h"
#include "lambda/closure_builder-inl.h"
#include "utils.h"

#include <numeric>
#include <stdint.h>
#include <type_traits>
#include "gtest/gtest.h"

// Turn this on for some extra printfs to help with debugging, since some code is optimized out.
static constexpr const bool kDebuggingClosureTest = true;

namespace std {
  using Closure = art::lambda::Closure;

  // Specialize std::default_delete so it knows how to properly delete closures
  // through the way we allocate them in this test.
  //
  // This is test-only because we don't want the rest of Art to do this.
  template <>
  struct default_delete<Closure> {
    void operator()(Closure* closure) const {
      delete[] reinterpret_cast<char*>(closure);
    }
  };
}  // namespace std

namespace art {

// Fake lock acquisition to please clang lock checker.
// This doesn't actually acquire any locks because we don't need multiple threads in this gtest.
struct SCOPED_CAPABILITY ScopedFakeLock {
  explicit ScopedFakeLock(MutatorMutex& mu) ACQUIRE(mu)
      : mu_(mu) {
  }

  ~ScopedFakeLock() RELEASE()
  {}

  MutatorMutex& mu_;
};

namespace lambda {

class ClosureTest : public ::testing::Test {
 public:
  ClosureTest() = default;
  ~ClosureTest() = default;

 protected:
  static void SetUpTestCase() {
  }

  virtual void SetUp() {
    // Create a completely dummy method here.
    // It's "OK" because the Closure never needs to look inside of the ArtMethod
    // (it just needs to be non-null).
    uintptr_t ignore = 0xbadbad;
    fake_method_ = reinterpret_cast<ArtMethod*>(ignore);
  }

  static ::testing::AssertionResult IsResultSuccessful(bool result) {
    if (result) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure();
    }
  }

  // Create a closure that captures the static variables from 'args' by-value.
  // The lambda method's captured variables types must match the ones in 'args'.
  // -- This creates the closure directly in-memory by using memcpy.
  template <typename ... Args>
  static std::unique_ptr<Closure> CreateClosureStaticVariables(ArtLambdaMethod* lambda_method,
                                                               Args&& ... args) {
    constexpr size_t header_size = sizeof(ArtLambdaMethod*);
    const size_t static_size = GetArgsSize(args ...) + header_size;
    EXPECT_GE(static_size, sizeof(Closure));

    // Can't just 'new' the Closure since we don't know the size up front.
    char* closure_as_char_array = new char[static_size];
    Closure* closure_ptr = new (closure_as_char_array) Closure;

    // Set up the data
    closure_ptr->lambda_info_ = lambda_method;
    CopyArgs(closure_ptr->captured_[0].static_variables_, args ...);

    // Make sure the entire thing is deleted once the unique_ptr goes out of scope.
    return std::unique_ptr<Closure>(closure_ptr);  // NOLINT [whitespace/braces] [5]
  }

  // Copy variadic arguments into the destination array with memcpy.
  template <typename T, typename ... Args>
  static void CopyArgs(uint8_t destination[], T&& arg, Args&& ... args) {
    memcpy(destination, &arg, sizeof(arg));
    CopyArgs(destination + sizeof(arg), args ...);
  }

  // Base case: Done.
  static void CopyArgs(uint8_t destination[]) {
    UNUSED(destination);
  }

  // Create a closure that captures the static variables from 'args' by-value.
  // The lambda method's captured variables types must match the ones in 'args'.
  // -- This uses ClosureBuilder interface to set up the closure indirectly.
  template <typename ... Args>
  static std::unique_ptr<Closure> CreateClosureStaticVariablesFromBuilder(
      ArtLambdaMethod* lambda_method,
      Args&& ... args) {
    // Acquire a fake lock since closure_builder needs it.
    ScopedFakeLock fake_lock(*Locks::mutator_lock_);

    ClosureBuilder closure_builder;
    CaptureVariableFromArgsList(/*out*/closure_builder, args ...);

    EXPECT_EQ(sizeof...(args), closure_builder.GetCaptureCount());

    constexpr size_t header_size = sizeof(ArtLambdaMethod*);
    const size_t static_size = GetArgsSize(args ...) + header_size;
    EXPECT_GE(static_size, sizeof(Closure));

    // For static variables, no nested closure, so size must match exactly.
    EXPECT_EQ(static_size, closure_builder.GetSize());

    // Can't just 'new' the Closure since we don't know the size up front.
    char* closure_as_char_array = new char[static_size];
    Closure* closure_ptr = new (closure_as_char_array) Closure;

    // The closure builder packs the captured variables into a Closure.
    closure_builder.CreateInPlace(closure_ptr, lambda_method);

    // Make sure the entire thing is deleted once the unique_ptr goes out of scope.
    return std::unique_ptr<Closure>(closure_ptr);  // NOLINT [whitespace/braces] [5]
  }

  // Call the correct ClosureBuilder::CaptureVariableXYZ function based on the type of args.
  // Invokes for each arg in args.
  template <typename ... Args>
  static void CaptureVariableFromArgsList(/*out*/ClosureBuilder& closure_builder, Args ... args) {
    int ignore[] = {
        (CaptureVariableFromArgs(/*out*/closure_builder, args),0)...  // NOLINT [whitespace/comma] [3]
    };
    UNUSED(ignore);
  }

  // ClosureBuilder::CaptureVariablePrimitive for types that are primitive only.
  template <typename T>
  typename std::enable_if<ShortyFieldTypeTraits::IsPrimitiveType<T>()>::type
  static CaptureVariableFromArgs(/*out*/ClosureBuilder& closure_builder, T value) {
    static_assert(ShortyFieldTypeTraits::IsPrimitiveType<T>(), "T must be a shorty primitive");
    closure_builder.CaptureVariablePrimitive<T, ShortyFieldTypeSelectEnum<T>::value>(value);
  }

  // ClosureBuilder::CaptureVariableObject for types that are objects only.
  template <typename T>
  typename std::enable_if<ShortyFieldTypeTraits::IsObjectType<T>()>::type
  static CaptureVariableFromArgs(/*out*/ClosureBuilder& closure_builder, const T* object) {
    ScopedFakeLock fake_lock(*Locks::mutator_lock_);
    closure_builder.CaptureVariableObject(object);
  }

  // Sum of sizeof(Args...).
  template <typename T, typename ... Args>
  static constexpr size_t GetArgsSize(T&& arg, Args&& ... args) {
    return sizeof(arg) + GetArgsSize(args ...);
  }

  // Base case: Done.
  static constexpr size_t GetArgsSize() {
    return 0;
  }

  // Take "U" and memcpy it into a "T". T starts out as (T)0.
  template <typename T, typename U>
  static T ExpandingBitCast(const U& val) {
    static_assert(sizeof(T) >= sizeof(U), "U too large");
    T new_val = static_cast<T>(0);
    memcpy(&new_val, &val, sizeof(U));
    return new_val;
  }

  // Templatized extraction from closures by checking their type with enable_if.
  template <typename T>
  static typename std::enable_if<ShortyFieldTypeTraits::IsPrimitiveNarrowType<T>()>::type
  ExpectCapturedVariable(const Closure* closure, size_t index, T value) {
    EXPECT_EQ(ExpandingBitCast<uint32_t>(value), closure->GetCapturedPrimitiveNarrow(index))
        << " with index " << index;
  }

  template <typename T>
  static typename std::enable_if<ShortyFieldTypeTraits::IsPrimitiveWideType<T>()>::type
  ExpectCapturedVariable(const Closure* closure, size_t index, T value) {
    EXPECT_EQ(ExpandingBitCast<uint64_t>(value), closure->GetCapturedPrimitiveWide(index))
        << " with index " << index;
  }

  // Templatized SFINAE for Objects so we can get better error messages.
  template <typename T>
  static typename std::enable_if<ShortyFieldTypeTraits::IsObjectType<T>()>::type
  ExpectCapturedVariable(const Closure* closure, size_t index, const T* object) {
    EXPECT_EQ(object, closure->GetCapturedObject(index))
        << " with index " << index;
  }

  template <typename ... Args>
  void TestPrimitive(const char *descriptor, Args ... args) {
    const char* shorty = descriptor;

    SCOPED_TRACE(descriptor);

    ASSERT_EQ(strlen(shorty), sizeof...(args))
        << "test error: descriptor must have same # of types as the # of captured variables";

    // Important: This fake lambda method needs to out-live any Closures we create with it.
    ArtLambdaMethod lambda_method{fake_method_,                    // NOLINT [whitespace/braces] [5]
                                  descriptor,                      // NOLINT [whitespace/blank_line] [2]
                                  shorty,
                                 };

    std::unique_ptr<Closure> closure_a;
    std::unique_ptr<Closure> closure_b;

    // Test the closure twice when it's constructed in different ways.
    {
      // Create the closure in a "raw" manner, that is directly with memcpy
      // since we know the underlying data format.
      // This simulates how the compiler would lay out the data directly.
      SCOPED_TRACE("raw closure");
      std::unique_ptr<Closure> closure_raw = CreateClosureStaticVariables(&lambda_method, args ...);

      if (kDebuggingClosureTest) {
        std::cerr << "closure raw address: " << closure_raw.get() << std::endl;
      }
      TestPrimitiveWithClosure(closure_raw.get(), descriptor, shorty, args ...);
      closure_a = std::move(closure_raw);
    }

    {
      // Create the closure with the ClosureBuilder, which is done indirectly.
      // This simulates how the interpreter would create the closure dynamically at runtime.
      SCOPED_TRACE("closure from builder");
      std::unique_ptr<Closure> closure_built =
          CreateClosureStaticVariablesFromBuilder(&lambda_method, args ...);
      if (kDebuggingClosureTest) {
        std::cerr << "closure built address: " << closure_built.get() << std::endl;
      }
      TestPrimitiveWithClosure(closure_built.get(), descriptor, shorty, args ...);
      closure_b = std::move(closure_built);
    }

    // The closures should be identical memory-wise as well.
    EXPECT_EQ(closure_a->GetSize(), closure_b->GetSize());
    EXPECT_TRUE(memcmp(closure_a.get(),
                       closure_b.get(),
                       std::min(closure_a->GetSize(), closure_b->GetSize())) == 0);
  }

  template <typename ... Args>
  static void TestPrimitiveWithClosure(Closure* closure,
                                       const char* descriptor,
                                       const char* shorty,
                                       Args ... args) {
    EXPECT_EQ(sizeof(ArtLambdaMethod*) + GetArgsSize(args...), closure->GetSize());
    EXPECT_EQ(sizeof...(args), closure->GetNumberOfCapturedVariables());
    EXPECT_STREQ(descriptor, closure->GetCapturedVariablesTypeDescriptor());
    TestPrimitiveExpects(closure, shorty, /*index*/0, args ...);
  }

  // Call EXPECT_EQ for each argument in the closure's #GetCapturedX.
  template <typename T, typename ... Args>
  static void TestPrimitiveExpects(
      const Closure* closure, const char* shorty, size_t index, T arg, Args ... args) {
    ASSERT_EQ(ShortyFieldType(shorty[index]).GetStaticSize(), sizeof(T))
        << "Test error: Type mismatch at index " << index;
    ExpectCapturedVariable(closure, index, arg);
    EXPECT_EQ(ShortyFieldType(shorty[index]), closure->GetCapturedShortyType(index));
    TestPrimitiveExpects(closure, shorty, index + 1, args ...);
  }

  // Base case for EXPECT_EQ.
  static void TestPrimitiveExpects(const Closure* closure, const char* shorty, size_t index) {
    UNUSED(closure, shorty, index);
  }

  ArtMethod* fake_method_;
};

TEST_F(ClosureTest, TestTrivial) {
  ArtLambdaMethod lambda_method{fake_method_,                    // NOLINT [whitespace/braces] [5]
                                "",  // No captured variables    // NOLINT [whitespace/blank_line] [2]
                                "",  // No captured variables
                               };

  std::unique_ptr<Closure> closure = CreateClosureStaticVariables(&lambda_method);

  EXPECT_EQ(sizeof(ArtLambdaMethod*), closure->GetSize());
  EXPECT_EQ(0u, closure->GetNumberOfCapturedVariables());
}  // TEST_F

TEST_F(ClosureTest, TestPrimitiveSingle) {
  TestPrimitive("Z", true);
  TestPrimitive("B", int8_t(0xde));
  TestPrimitive("C", uint16_t(0xbeef));
  TestPrimitive("S", int16_t(0xdead));
  TestPrimitive("I", int32_t(0xdeadbeef));
  TestPrimitive("F", 0.123f);
  TestPrimitive("J", int64_t(0xdeadbeef00c0ffee));
  TestPrimitive("D", 123.456);
}  // TEST_F

TEST_F(ClosureTest, TestPrimitiveMany) {
  TestPrimitive("ZZ", true, false);
  TestPrimitive("ZZZ", true, false, true);
  TestPrimitive("BBBB", int8_t(0xde), int8_t(0xa0), int8_t(0xff), int8_t(0xcc));
  TestPrimitive("CC", uint16_t(0xbeef), uint16_t(0xdead));
  TestPrimitive("SSSS", int16_t(0xdead), int16_t(0xc0ff), int16_t(0xf000), int16_t(0xbaba));
  TestPrimitive("III", int32_t(0xdeadbeef), int32_t(0xc0ffee), int32_t(0xbeefdead));
  TestPrimitive("FF", 0.123f, 555.666f);
  TestPrimitive("JJJ", int64_t(0xdeadbeef00c0ffee), int64_t(0x123), int64_t(0xc0ffee));
  TestPrimitive("DD", 123.456, 777.888);
}  // TEST_F

TEST_F(ClosureTest, TestPrimitiveMixed) {
  TestPrimitive("ZZBBCCSSIIFFJJDD",
                true, false,
                int8_t(0xde), int8_t(0xa0),
                uint16_t(0xbeef), uint16_t(0xdead),
                int16_t(0xdead), int16_t(0xc0ff),
                int32_t(0xdeadbeef), int32_t(0xc0ffee),
                0.123f, 555.666f,
                int64_t(0xdeadbeef00c0ffee), int64_t(0x123),
                123.456, 777.888);
}  // TEST_F

}  // namespace lambda
}  // namespace art
