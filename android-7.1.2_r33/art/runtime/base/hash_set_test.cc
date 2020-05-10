/*
 * Copyright (C) 2014 The Android Open Source Project
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

#include "hash_set.h"

#include <map>
#include <forward_list>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

#include <gtest/gtest.h>
#include "hash_map.h"

namespace art {

struct IsEmptyFnString {
  void MakeEmpty(std::string& item) const {
    item.clear();
  }
  bool IsEmpty(const std::string& item) const {
    return item.empty();
  }
};

class HashSetTest : public testing::Test {
 public:
  HashSetTest() : seed_(97421), unique_number_(0) {
  }
  std::string RandomString(size_t len) {
    std::ostringstream oss;
    for (size_t i = 0; i < len; ++i) {
      oss << static_cast<char>('A' + PRand() % 64);
    }
    static_assert(' ' < 'A', "space must be less than a");
    oss << " " << unique_number_++;  // Relies on ' ' < 'A'
    return oss.str();
  }
  void SetSeed(size_t seed) {
    seed_ = seed;
  }
  size_t PRand() {  // Pseudo random.
    seed_ = seed_ * 1103515245 + 12345;
    return seed_;
  }

 private:
  size_t seed_;
  size_t unique_number_;
};

TEST_F(HashSetTest, TestSmoke) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  const std::string test_string = "hello world 1234";
  ASSERT_TRUE(hash_set.Empty());
  ASSERT_EQ(hash_set.Size(), 0U);
  hash_set.Insert(test_string);
  auto it = hash_set.Find(test_string);
  ASSERT_EQ(*it, test_string);
  auto after_it = hash_set.Erase(it);
  ASSERT_TRUE(after_it == hash_set.end());
  ASSERT_TRUE(hash_set.Empty());
  ASSERT_EQ(hash_set.Size(), 0U);
  it = hash_set.Find(test_string);
  ASSERT_TRUE(it == hash_set.end());
}

TEST_F(HashSetTest, TestInsertAndErase) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  static constexpr size_t count = 1000;
  std::vector<std::string> strings;
  for (size_t i = 0; i < count; ++i) {
    // Insert a bunch of elements and make sure we can find them.
    strings.push_back(RandomString(10));
    hash_set.Insert(strings[i]);
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
  }
  ASSERT_EQ(strings.size(), hash_set.Size());
  // Try to erase the odd strings.
  for (size_t i = 1; i < count; i += 2) {
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
    hash_set.Erase(it);
  }
  // Test removed.
  for (size_t i = 1; i < count; i += 2) {
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it == hash_set.end());
  }
  for (size_t i = 0; i < count; i += 2) {
    auto it = hash_set.Find(strings[i]);
    ASSERT_TRUE(it != hash_set.end());
    ASSERT_EQ(*it, strings[i]);
  }
}

TEST_F(HashSetTest, TestIterator) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  ASSERT_TRUE(hash_set.begin() == hash_set.end());
  static constexpr size_t count = 1000;
  std::vector<std::string> strings;
  for (size_t i = 0; i < count; ++i) {
    // Insert a bunch of elements and make sure we can find them.
    strings.push_back(RandomString(10));
    hash_set.Insert(strings[i]);
  }
  // Make sure we visit each string exactly once.
  std::map<std::string, size_t> found_count;
  for (const std::string& s : hash_set) {
    ++found_count[s];
  }
  for (size_t i = 0; i < count; ++i) {
    ASSERT_EQ(found_count[strings[i]], 1U);
  }
  found_count.clear();
  // Remove all the elements with iterator erase.
  for (auto it = hash_set.begin(); it != hash_set.end();) {
    ++found_count[*it];
    it = hash_set.Erase(it);
    ASSERT_EQ(hash_set.Verify(), 0U);
  }
  for (size_t i = 0; i < count; ++i) {
    ASSERT_EQ(found_count[strings[i]], 1U);
  }
}

TEST_F(HashSetTest, TestSwap) {
  HashSet<std::string, IsEmptyFnString> hash_seta, hash_setb;
  std::vector<std::string> strings;
  static constexpr size_t count = 1000;
  for (size_t i = 0; i < count; ++i) {
    strings.push_back(RandomString(10));
    hash_seta.Insert(strings[i]);
  }
  std::swap(hash_seta, hash_setb);
  hash_seta.Insert("TEST");
  hash_setb.Insert("TEST2");
  for (size_t i = 0; i < count; ++i) {
    strings.push_back(RandomString(10));
    hash_seta.Insert(strings[i]);
  }
}

TEST_F(HashSetTest, TestShrink) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  std::vector<std::string> strings = {"a", "b", "c", "d", "e", "f", "g"};
  for (size_t i = 0; i < strings.size(); ++i) {
    // Insert some strings into the beginning of our hash set to establish an initial size
    hash_set.Insert(strings[i]);
  }

  hash_set.ShrinkToMaximumLoad();
  const double initial_load = hash_set.CalculateLoadFactor();

  // Insert a bunch of random strings to guarantee that we grow the capacity.
  std::vector<std::string> random_strings;
  static constexpr size_t count = 1000;
  for (size_t i = 0; i < count; ++i) {
    random_strings.push_back(RandomString(10));
    hash_set.Insert(random_strings[i]);
  }

  // Erase all the extra strings which guarantees that our load factor will be really bad.
  for (size_t i = 0; i < count; ++i) {
    hash_set.Erase(hash_set.Find(random_strings[i]));
  }

  const double bad_load = hash_set.CalculateLoadFactor();
  EXPECT_GT(initial_load, bad_load);

  // Shrink again, the load factor should be good again.
  hash_set.ShrinkToMaximumLoad();
  EXPECT_DOUBLE_EQ(initial_load, hash_set.CalculateLoadFactor());

  // Make sure all the initial elements we had are still there
  for (const std::string& initial_string : strings) {
    EXPECT_NE(hash_set.end(), hash_set.Find(initial_string))
        << "expected to find " << initial_string;
  }
}

TEST_F(HashSetTest, TestLoadFactor) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  static constexpr size_t kStringCount = 1000;
  static constexpr double kEpsilon = 0.01;
  for (size_t i = 0; i < kStringCount; ++i) {
    hash_set.Insert(RandomString(i % 10 + 1));
  }
  // Check that changing the load factor resizes the table to be within the target range.
  EXPECT_GE(hash_set.CalculateLoadFactor() + kEpsilon, hash_set.GetMinLoadFactor());
  EXPECT_LE(hash_set.CalculateLoadFactor() - kEpsilon, hash_set.GetMaxLoadFactor());
  hash_set.SetLoadFactor(0.1, 0.3);
  EXPECT_DOUBLE_EQ(0.1, hash_set.GetMinLoadFactor());
  EXPECT_DOUBLE_EQ(0.3, hash_set.GetMaxLoadFactor());
  EXPECT_LE(hash_set.CalculateLoadFactor() - kEpsilon, hash_set.GetMaxLoadFactor());
  hash_set.SetLoadFactor(0.6, 0.8);
  EXPECT_LE(hash_set.CalculateLoadFactor() - kEpsilon, hash_set.GetMaxLoadFactor());
}

TEST_F(HashSetTest, TestStress) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  std::unordered_multiset<std::string> std_set;
  std::vector<std::string> strings;
  static constexpr size_t string_count = 2000;
  static constexpr size_t operations = 100000;
  static constexpr size_t target_size = 5000;
  for (size_t i = 0; i < string_count; ++i) {
    strings.push_back(RandomString(i % 10 + 1));
  }
  const size_t seed = time(nullptr);
  SetSeed(seed);
  LOG(INFO) << "Starting stress test with seed " << seed;
  for (size_t i = 0; i < operations; ++i) {
    ASSERT_EQ(hash_set.Size(), std_set.size());
    size_t delta = std::abs(static_cast<ssize_t>(target_size) -
                            static_cast<ssize_t>(hash_set.Size()));
    size_t n = PRand();
    if (n % target_size == 0) {
      hash_set.Clear();
      std_set.clear();
      ASSERT_TRUE(hash_set.Empty());
      ASSERT_TRUE(std_set.empty());
    } else  if (n % target_size < delta) {
      // Skew towards adding elements until we are at the desired size.
      const std::string& s = strings[PRand() % string_count];
      hash_set.Insert(s);
      std_set.insert(s);
      ASSERT_EQ(*hash_set.Find(s), *std_set.find(s));
    } else {
      const std::string& s = strings[PRand() % string_count];
      auto it1 = hash_set.Find(s);
      auto it2 = std_set.find(s);
      ASSERT_EQ(it1 == hash_set.end(), it2 == std_set.end());
      if (it1 != hash_set.end()) {
        ASSERT_EQ(*it1, *it2);
        hash_set.Erase(it1);
        std_set.erase(it2);
      }
    }
  }
}

struct IsEmptyStringPair {
  void MakeEmpty(std::pair<std::string, int>& pair) const {
    pair.first.clear();
  }
  bool IsEmpty(const std::pair<std::string, int>& pair) const {
    return pair.first.empty();
  }
};

TEST_F(HashSetTest, TestHashMap) {
  HashMap<std::string, int, IsEmptyStringPair> hash_map;
  hash_map.Insert(std::make_pair(std::string("abcd"), 123));
  hash_map.Insert(std::make_pair(std::string("abcd"), 124));
  hash_map.Insert(std::make_pair(std::string("bags"), 444));
  auto it = hash_map.Find(std::string("abcd"));
  ASSERT_EQ(it->second, 123);
  hash_map.Erase(it);
  it = hash_map.Find(std::string("abcd"));
  ASSERT_EQ(it->second, 124);
}

struct IsEmptyFnVectorInt {
  void MakeEmpty(std::vector<int>& item) const {
    item.clear();
  }
  bool IsEmpty(const std::vector<int>& item) const {
    return item.empty();
  }
};

template <typename T>
size_t HashIntSequence(T begin, T end) {
  size_t hash = 0;
  for (auto iter = begin; iter != end; ++iter) {
    hash = hash * 2 + *iter;
  }
  return hash;
};

struct VectorIntHashEquals {
  std::size_t operator()(const std::vector<int>& item) const {
    return HashIntSequence(item.begin(), item.end());
  }

  std::size_t operator()(const std::forward_list<int>& item) const {
    return HashIntSequence(item.begin(), item.end());
  }

  bool operator()(const std::vector<int>& a, const std::vector<int>& b) const {
    return a == b;
  }

  bool operator()(const std::vector<int>& a, const std::forward_list<int>& b) const {
    auto aiter = a.begin();
    auto biter = b.begin();
    while (aiter != a.end() && biter != b.end()) {
      if (*aiter != *biter) {
        return false;
      }
      aiter++;
      biter++;
    }
    return (aiter == a.end() && biter == b.end());
  }
};

TEST_F(HashSetTest, TestLookupByAlternateKeyType) {
  HashSet<std::vector<int>, IsEmptyFnVectorInt, VectorIntHashEquals, VectorIntHashEquals> hash_set;
  hash_set.Insert(std::vector<int>({1, 2, 3, 4}));
  hash_set.Insert(std::vector<int>({4, 2}));
  ASSERT_EQ(hash_set.end(), hash_set.Find(std::vector<int>({1, 1, 1, 1})));
  ASSERT_NE(hash_set.end(), hash_set.Find(std::vector<int>({1, 2, 3, 4})));
  ASSERT_EQ(hash_set.end(), hash_set.Find(std::forward_list<int>({1, 1, 1, 1})));
  ASSERT_NE(hash_set.end(), hash_set.Find(std::forward_list<int>({1, 2, 3, 4})));
}

TEST_F(HashSetTest, TestReserve) {
  HashSet<std::string, IsEmptyFnString> hash_set;
  std::vector<size_t> sizes = {1, 10, 25, 55, 128, 1024, 4096};
  for (size_t size : sizes) {
    hash_set.Reserve(size);
    const size_t buckets_before = hash_set.NumBuckets();
    // Check that we expanded enough.
    CHECK_GE(hash_set.ElementsUntilExpand(), size);
    // Try inserting elements until we are at our reserve size and ensure the hash set did not
    // expand.
    while (hash_set.Size() < size) {
      hash_set.Insert(std::to_string(hash_set.Size()));
    }
    CHECK_EQ(hash_set.NumBuckets(), buckets_before);
  }
  // Check the behaviour for shrinking, it does not necessarily resize down.
  constexpr size_t size = 100;
  hash_set.Reserve(size);
  CHECK_GE(hash_set.ElementsUntilExpand(), size);
}

}  // namespace art
