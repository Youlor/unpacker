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

#include "lambda/shorty_field_type.h"
#include "mirror/object_reference.h"

#include "utils.h"
#include <numeric>
#include <stdint.h>
#include "gtest/gtest.h"

#define EXPECT_NULL(expected) EXPECT_EQ(reinterpret_cast<const void*>(expected), \
                                        reinterpret_cast<void*>(nullptr));

namespace art {
namespace lambda {

class ShortyFieldTypeTest : public ::testing::Test {
 public:
  ShortyFieldTypeTest() = default;
  ~ShortyFieldTypeTest() = default;

 protected:
  static void SetUpTestCase() {
  }

  virtual void SetUp() {
  }

  static ::testing::AssertionResult IsResultSuccessful(bool result) {
    if (result) {
      return ::testing::AssertionSuccess();
    } else {
      return ::testing::AssertionFailure();
    }
  }

  template <typename T>
  static std::string ListToString(const T& list) {
    std::stringstream stream;

    stream << "[";
    for (auto&& val : list) {
      stream << val << ", ";
    }
    stream << "]";

    return stream.str();
  }

  // Compare two vector-like types for equality.
  template <typename T>
  static ::testing::AssertionResult AreListsEqual(const T& expected, const T& actual) {
    bool success = true;
    std::stringstream stream;

    if (expected.size() != actual.size()) {
      success = false;
      stream << "Expected list size: " << expected.size()
             << ", but got list size: " << actual.size();
      stream << std::endl;
    }

    for (size_t j = 0; j < std::min(expected.size(), actual.size()); ++j) {
      if (expected[j] != actual[j]) {
        success = false;
        stream << "Expected element '" << j << "' to be '" << expected[j] << "', but got actual: '"
               << actual[j] << "'.";
        stream << std::endl;
      }
    }

    if (success) {
      return ::testing::AssertionSuccess();
    }

    stream << "Expected list was: " << ListToString(expected)
           << ", actual list was: " << ListToString(actual);

    return ::testing::AssertionFailure() << stream.str();
  }

  static std::vector<ShortyFieldType> ParseLongTypeDescriptorsToList(const char* type_descriptor) {
    std::vector<ShortyFieldType> lst;

    ShortyFieldType shorty;

    const char* parsed = type_descriptor;
    while ((parsed = ShortyFieldType::ParseFromFieldTypeDescriptor(parsed, &shorty)) != nullptr) {
      lst.push_back(shorty);
    }

    return lst;
  }

 protected:
  // Shorthands for the ShortyFieldType constants.
  // The letters are the same as JNI letters, with kS_ being a lambda since \ is not available.
  static constexpr ShortyFieldType kSZ = ShortyFieldType::kBoolean;
  static constexpr ShortyFieldType kSB = ShortyFieldType::kByte;
  static constexpr ShortyFieldType kSC = ShortyFieldType::kChar;
  static constexpr ShortyFieldType kSS = ShortyFieldType::kShort;
  static constexpr ShortyFieldType kSI = ShortyFieldType::kInt;
  static constexpr ShortyFieldType kSF = ShortyFieldType::kFloat;
  static constexpr ShortyFieldType kSJ = ShortyFieldType::kLong;
  static constexpr ShortyFieldType kSD = ShortyFieldType::kDouble;
  static constexpr ShortyFieldType kSL = ShortyFieldType::kObject;
  static constexpr ShortyFieldType kS_ = ShortyFieldType::kLambda;
};

TEST_F(ShortyFieldTypeTest, TestMaybeCreate) {
  ShortyFieldType shorty;

  std::vector<char> shorties = {'Z', 'B', 'C', 'S', 'I', 'F', 'J', 'D', 'L', '\\'};

  // All valid 'shorty' characters are created successfully.
  for (const char c : shorties) {
    EXPECT_TRUE(ShortyFieldType::MaybeCreate(c, &shorty)) << c;
    EXPECT_EQ(c, static_cast<char>(c));
  }

  // All other characters can never be created.
  for (unsigned char c = 0; c < std::numeric_limits<unsigned char>::max(); ++c) {
    // Skip the valid characters.
    if (std::find(shorties.begin(), shorties.end(), c) != shorties.end()) { continue; }
    // All invalid characters should fail.
    EXPECT_FALSE(ShortyFieldType::MaybeCreate(static_cast<char>(c), &shorty)) << c;
  }
}  // TEST_F

TEST_F(ShortyFieldTypeTest, TestCreateFromFieldTypeDescriptor) {
  // Sample input.
  std::vector<const char*> lengthies = {
      "Z", "B", "C", "S", "I", "F", "J", "D", "LObject;", "\\Closure;",
      "[Z", "[[B", "[[LObject;"
  };

  // Expected output.
  std::vector<ShortyFieldType> expected = {
      ShortyFieldType::kBoolean,
      ShortyFieldType::kByte,
      ShortyFieldType::kChar,
      ShortyFieldType::kShort,
      ShortyFieldType::kInt,
      ShortyFieldType::kFloat,
      ShortyFieldType::kLong,
      ShortyFieldType::kDouble,
      ShortyFieldType::kObject,
      ShortyFieldType::kLambda,
      // Arrays are always treated as objects.
      ShortyFieldType::kObject,
      ShortyFieldType::kObject,
      ShortyFieldType::kObject,
  };

  // All valid lengthy types are correctly turned into the expected shorty type.
  for (size_t i = 0; i < lengthies.size(); ++i) {
    EXPECT_EQ(expected[i], ShortyFieldType::CreateFromFieldTypeDescriptor(lengthies[i]));
  }
}  // TEST_F

TEST_F(ShortyFieldTypeTest, TestParseFromFieldTypeDescriptor) {
  // Sample input.
  std::vector<const char*> lengthies = {
      // Empty list
      "",
      // Primitives
      "Z", "B", "C", "S", "I", "F", "J", "D",
      // Non-primitives
      "LObject;", "\\Closure;",
      // Arrays. The biggest PITA.
      "[Z", "[[B", "[[LObject;", "[[[[\\Closure;",
      // Multiple things at once:
      "ZBCSIFJD",
      "LObject;LObject;SSI",
      "[[ZDDZ",
      "[[LObject;[[Z[F\\Closure;LObject;",
  };

  // Expected output.
  std::vector<std::vector<ShortyFieldType>> expected = {
      // Empty list
      {},
      // Primitives
      {kSZ}, {kSB}, {kSC}, {kSS}, {kSI}, {kSF}, {kSJ}, {kSD},
      // Non-primitives.
      { ShortyFieldType::kObject }, { ShortyFieldType::kLambda },
      // Arrays are always treated as objects.
      { kSL }, { kSL }, { kSL }, { kSL },
      // Multiple things at once:
      { kSZ, kSB, kSC, kSS, kSI, kSF, kSJ, kSD },
      { kSL, kSL, kSS, kSS, kSI },
      { kSL, kSD, kSD, kSZ },
      { kSL, kSL, kSL, kS_, kSL },
  };

  // Sanity check that the expected/actual lists are the same size.. when adding new entries.
  ASSERT_EQ(expected.size(), lengthies.size());

  // All valid lengthy types are correctly turned into the expected shorty type.
  for (size_t i = 0; i < expected.size(); ++i) {
    const std::vector<ShortyFieldType>& expected_list = expected[i];
    std::vector<ShortyFieldType> actual_list = ParseLongTypeDescriptorsToList(lengthies[i]);
    EXPECT_TRUE(AreListsEqual(expected_list, actual_list));
  }
}  // TEST_F

// Helper class to probe a shorty's characteristics by minimizing copy-and-paste tests.
template <typename T, decltype(ShortyFieldType::kByte) kShortyEnum>
struct ShortyTypeCharacteristics {
  bool is_primitive_ = false;
  bool is_primitive_narrow_ = false;
  bool is_primitive_wide_ = false;
  bool is_object_ = false;
  bool is_lambda_ = false;
  size_t size_ = sizeof(T);
  bool is_dynamic_sized_ = false;

  void CheckExpects() {
    ShortyFieldType shorty = kShortyEnum;

    // Test the main non-parsing-related ShortyFieldType characteristics.
    EXPECT_EQ(is_primitive_, shorty.IsPrimitive());
    EXPECT_EQ(is_primitive_narrow_, shorty.IsPrimitiveNarrow());
    EXPECT_EQ(is_primitive_wide_, shorty.IsPrimitiveWide());
    EXPECT_EQ(is_object_, shorty.IsObject());
    EXPECT_EQ(is_lambda_, shorty.IsLambda());
    EXPECT_EQ(size_, shorty.GetStaticSize());
    EXPECT_EQ(is_dynamic_sized_, !shorty.IsStaticSize());

    // Test compile-time ShortyFieldTypeTraits.
    EXPECT_TRUE(ShortyFieldTypeTraits::IsType<T>());
    EXPECT_EQ(is_primitive_, ShortyFieldTypeTraits::IsPrimitiveType<T>());
    EXPECT_EQ(is_primitive_narrow_, ShortyFieldTypeTraits::IsPrimitiveNarrowType<T>());
    EXPECT_EQ(is_primitive_wide_, ShortyFieldTypeTraits::IsPrimitiveWideType<T>());
    EXPECT_EQ(is_object_, ShortyFieldTypeTraits::IsObjectType<T>());
    EXPECT_EQ(is_lambda_, ShortyFieldTypeTraits::IsLambdaType<T>());

    // Test compile-time ShortyFieldType selectors
    static_assert(std::is_same<T, typename ShortyFieldTypeSelectType<kShortyEnum>::type>::value,
                  "ShortyFieldType Enum->Type incorrect mapping");
    auto kActualEnum = ShortyFieldTypeSelectEnum<T>::value;  // Do not ODR-use, avoid linker error.
    EXPECT_EQ(kShortyEnum, kActualEnum);
  }
};

TEST_F(ShortyFieldTypeTest, TestCharacteristicsAndTraits) {
  // Boolean test
  {
    SCOPED_TRACE("boolean");
    ShortyTypeCharacteristics<bool, ShortyFieldType::kBoolean> chars;
    chars.is_primitive_ = true;
    chars.is_primitive_narrow_ = true;
    chars.CheckExpects();
  }

  // Byte test
  {
    SCOPED_TRACE("byte");
    ShortyTypeCharacteristics<int8_t, ShortyFieldType::kByte> chars;
    chars.is_primitive_ = true;
    chars.is_primitive_narrow_ = true;
    chars.CheckExpects();
  }

  // Char test
  {
    SCOPED_TRACE("char");
    ShortyTypeCharacteristics<uint16_t, ShortyFieldType::kChar> chars;  // Char is unsigned.
    chars.is_primitive_ = true;
    chars.is_primitive_narrow_ = true;
    chars.CheckExpects();
  }

  // Short test
  {
    SCOPED_TRACE("short");
    ShortyTypeCharacteristics<int16_t, ShortyFieldType::kShort> chars;
    chars.is_primitive_ = true;
    chars.is_primitive_narrow_ = true;
    chars.CheckExpects();
  }

  // Int test
  {
    SCOPED_TRACE("int");
    ShortyTypeCharacteristics<int32_t, ShortyFieldType::kInt> chars;
    chars.is_primitive_ = true;
    chars.is_primitive_narrow_ = true;
    chars.CheckExpects();
  }

  // Long test
  {
    SCOPED_TRACE("long");
    ShortyTypeCharacteristics<int64_t, ShortyFieldType::kLong> chars;
    chars.is_primitive_ = true;
    chars.is_primitive_wide_ = true;
    chars.CheckExpects();
  }

  // Float test
  {
    SCOPED_TRACE("float");
    ShortyTypeCharacteristics<float, ShortyFieldType::kFloat> chars;
    chars.is_primitive_ = true;
    chars.is_primitive_narrow_ = true;
    chars.CheckExpects();
  }

  // Double test
  {
    SCOPED_TRACE("double");
    ShortyTypeCharacteristics<double, ShortyFieldType::kDouble> chars;
    chars.is_primitive_ = true;
    chars.is_primitive_wide_ = true;
    chars.CheckExpects();
  }

  // Object test
  {
    SCOPED_TRACE("object");
    ShortyTypeCharacteristics<mirror::Object*, ShortyFieldType::kObject> chars;
    chars.is_object_ = true;
    chars.size_ = kObjectReferenceSize;
    chars.CheckExpects();
    EXPECT_EQ(kObjectReferenceSize, sizeof(mirror::CompressedReference<mirror::Object>));
  }

  // Lambda test
  {
    SCOPED_TRACE("lambda");
    ShortyTypeCharacteristics<Closure*, ShortyFieldType::kLambda> chars;
    chars.is_lambda_ = true;
    chars.is_dynamic_sized_ = true;
    chars.CheckExpects();
  }
}

}  // namespace lambda
}  // namespace art
