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

#include "variant_map.h"
#include "gtest/gtest.h"

#define EXPECT_NULL(expected) EXPECT_EQ(reinterpret_cast<const void*>(expected), \
                                        static_cast<void*>(nullptr));

namespace art {

namespace {
  template <typename TValue>
  struct FruitMapKey : VariantMapKey<TValue> {
    FruitMapKey() {}
  };

  struct FruitMap : VariantMap<FruitMap, FruitMapKey> {
    // This 'using' line is necessary to inherit the variadic constructor.
    using VariantMap<FruitMap, FruitMapKey>::VariantMap;

    // Make the next '4' usages of Key slightly shorter to type.
    template <typename TValue>
    using Key = FruitMapKey<TValue>;

    static const Key<int> Apple;
    static const Key<double> Orange;
    static const Key<std::string> Label;
  };

  const FruitMap::Key<int> FruitMap::Apple;
  const FruitMap::Key<double> FruitMap::Orange;
  const FruitMap::Key<std::string> FruitMap::Label;
}  // namespace

TEST(VariantMaps, BasicReadWrite) {
  FruitMap fm;

  EXPECT_NULL(fm.Get(FruitMap::Apple));
  EXPECT_FALSE(fm.Exists(FruitMap::Apple));
  EXPECT_NULL(fm.Get(FruitMap::Orange));
  EXPECT_FALSE(fm.Exists(FruitMap::Orange));

  fm.Set(FruitMap::Apple, 1);
  EXPECT_NULL(fm.Get(FruitMap::Orange));
  EXPECT_EQ(1, *fm.Get(FruitMap::Apple));
  EXPECT_TRUE(fm.Exists(FruitMap::Apple));

  fm.Set(FruitMap::Apple, 5);
  EXPECT_NULL(fm.Get(FruitMap::Orange));
  EXPECT_EQ(5, *fm.Get(FruitMap::Apple));
  EXPECT_TRUE(fm.Exists(FruitMap::Apple));

  fm.Set(FruitMap::Orange, 555.0);
  EXPECT_EQ(5, *fm.Get(FruitMap::Apple));
  EXPECT_DOUBLE_EQ(555.0, *fm.Get(FruitMap::Orange));
  EXPECT_EQ(size_t(2), fm.Size());

  // Simple remove
  fm.Remove(FruitMap::Apple);
  EXPECT_FALSE(fm.Exists(FruitMap::Apple));

  fm.Clear();
  EXPECT_EQ(size_t(0), fm.Size());
  EXPECT_FALSE(fm.Exists(FruitMap::Orange));
}

TEST(VariantMaps, SetPreviousValue) {
  FruitMap fm;

  // Indirect remove by setting yourself again
  fm.Set(FruitMap::Label, std::string("hello_world"));
  auto* ptr = fm.Get(FruitMap::Label);
  ASSERT_TRUE(ptr != nullptr);
  *ptr = "foobar";

  // Set the value to the same exact pointer which we got out of the map.
  // This should cleanly 'just work' and not try to delete the value too early.
  fm.Set(FruitMap::Label, *ptr);

  auto* new_ptr = fm.Get(FruitMap::Label);
  ASSERT_TRUE(ptr != nullptr);
  EXPECT_EQ(std::string("foobar"), *new_ptr);
}

TEST(VariantMaps, RuleOfFive) {
  // Test empty constructor
  FruitMap fmEmpty;
  EXPECT_EQ(size_t(0), fmEmpty.Size());

  // Test empty constructor
  FruitMap fmFilled;
  fmFilled.Set(FruitMap::Apple, 1);
  fmFilled.Set(FruitMap::Orange, 555.0);
  EXPECT_EQ(size_t(2), fmFilled.Size());

  // Test copy constructor
  FruitMap fmEmptyCopy(fmEmpty);
  EXPECT_EQ(size_t(0), fmEmptyCopy.Size());

  // Test copy constructor
  FruitMap fmFilledCopy(fmFilled);
  EXPECT_EQ(size_t(2), fmFilledCopy.Size());
  EXPECT_EQ(*fmFilled.Get(FruitMap::Apple), *fmFilledCopy.Get(FruitMap::Apple));
  EXPECT_DOUBLE_EQ(*fmFilled.Get(FruitMap::Orange), *fmFilledCopy.Get(FruitMap::Orange));

  // Test operator=
  FruitMap fmFilledCopy2;
  fmFilledCopy2 = fmFilled;
  EXPECT_EQ(size_t(2), fmFilledCopy2.Size());
  EXPECT_EQ(*fmFilled.Get(FruitMap::Apple), *fmFilledCopy2.Get(FruitMap::Apple));
  EXPECT_DOUBLE_EQ(*fmFilled.Get(FruitMap::Orange), *fmFilledCopy2.Get(FruitMap::Orange));

  // Test move constructor
  FruitMap fmMoved(std::move(fmFilledCopy));
  EXPECT_EQ(size_t(0), fmFilledCopy.Size());
  EXPECT_EQ(size_t(2), fmMoved.Size());
  EXPECT_EQ(*fmFilled.Get(FruitMap::Apple), *fmMoved.Get(FruitMap::Apple));
  EXPECT_DOUBLE_EQ(*fmFilled.Get(FruitMap::Orange), *fmMoved.Get(FruitMap::Orange));

  // Test operator= move
  FruitMap fmMoved2;
  fmMoved2.Set(FruitMap::Apple, 12345);  // This value will be clobbered after the move

  fmMoved2 = std::move(fmFilledCopy2);
  EXPECT_EQ(size_t(0), fmFilledCopy2.Size());
  EXPECT_EQ(size_t(2), fmMoved2.Size());
  EXPECT_EQ(*fmFilled.Get(FruitMap::Apple), *fmMoved2.Get(FruitMap::Apple));
  EXPECT_DOUBLE_EQ(*fmFilled.Get(FruitMap::Orange), *fmMoved2.Get(FruitMap::Orange));
}

TEST(VariantMaps, VariadicConstructors) {
  // Variadic constructor, 1 kv/pair
  FruitMap fmApple(FruitMap::Apple, 12345);
  EXPECT_EQ(size_t(1), fmApple.Size());
  EXPECT_EQ(12345, *fmApple.Get(FruitMap::Apple));

  // Variadic constructor, 2 kv/pair
  FruitMap fmAppleAndOrange(FruitMap::Apple,   12345,
                            FruitMap::Orange,  100.0);
  EXPECT_EQ(size_t(2), fmAppleAndOrange.Size());
  EXPECT_EQ(12345, *fmAppleAndOrange.Get(FruitMap::Apple));
  EXPECT_DOUBLE_EQ(100.0, *fmAppleAndOrange.Get(FruitMap::Orange));
}

TEST(VariantMaps, ReleaseOrDefault) {
  FruitMap fmAppleAndOrange(FruitMap::Apple,   12345,
                            FruitMap::Orange,  100.0);

  int apple = fmAppleAndOrange.ReleaseOrDefault(FruitMap::Apple);
  EXPECT_EQ(12345, apple);

  // Releasing will also remove the Apple key.
  EXPECT_EQ(size_t(1), fmAppleAndOrange.Size());

  // Releasing again yields a default value.
  int apple2 = fmAppleAndOrange.ReleaseOrDefault(FruitMap::Apple);
  EXPECT_EQ(0, apple2);
}

TEST(VariantMaps, GetOrDefault) {
  FruitMap fm(FruitMap::Apple,   12345);

  // Apple gives the expected value we set.
  int apple = fm.GetOrDefault(FruitMap::Apple);
  EXPECT_EQ(12345, apple);

  // Map is still 1.
  EXPECT_EQ(size_t(1), fm.Size());

  // Orange gives back a default value, since it's not in the map.
  double orange = fm.GetOrDefault(FruitMap::Orange);
  EXPECT_DOUBLE_EQ(0.0, orange);
}

}  // namespace art
