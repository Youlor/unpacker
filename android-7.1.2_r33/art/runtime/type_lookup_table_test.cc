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


#include <memory>

#include "common_runtime_test.h"
#include "dex_file-inl.h"
#include "scoped_thread_state_change.h"
#include "type_lookup_table.h"
#include "utf-inl.h"

namespace art {

static const size_t kDexNoIndex = DexFile::kDexNoIndex;  // Make copy to prevent linking errors.

using DescriptorClassDefIdxPair = std::pair<const char*, uint32_t>;
class TypeLookupTableTest : public CommonRuntimeTestWithParam<DescriptorClassDefIdxPair> {};

TEST_F(TypeLookupTableTest, CreateLookupTable) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<const DexFile> dex_file(OpenTestDexFile("Lookup"));
  std::unique_ptr<TypeLookupTable> table(TypeLookupTable::Create(*dex_file));
  ASSERT_NE(nullptr, table.get());
  ASSERT_NE(nullptr, table->RawData());
  ASSERT_EQ(32U, table->RawDataLength());
}

TEST_P(TypeLookupTableTest, Find) {
  ScopedObjectAccess soa(Thread::Current());
  std::unique_ptr<const DexFile> dex_file(OpenTestDexFile("Lookup"));
  std::unique_ptr<TypeLookupTable> table(TypeLookupTable::Create(*dex_file));
  ASSERT_NE(nullptr, table.get());
  auto pair = GetParam();
  const char* descriptor = pair.first;
  size_t hash = ComputeModifiedUtf8Hash(descriptor);
  uint32_t class_def_idx = table->Lookup(descriptor, hash);
  ASSERT_EQ(pair.second, class_def_idx);
}

INSTANTIATE_TEST_CASE_P(FindNonExistingClassWithoutCollisions,
                        TypeLookupTableTest,
                        testing::Values(DescriptorClassDefIdxPair("LAB;", 1U)));
INSTANTIATE_TEST_CASE_P(FindNonExistingClassWithCollisions,
                        TypeLookupTableTest,
                        testing::Values(DescriptorClassDefIdxPair("LDA;", kDexNoIndex)));
INSTANTIATE_TEST_CASE_P(FindClassNoCollisions,
                        TypeLookupTableTest,
                        testing::Values(DescriptorClassDefIdxPair("LC;", 2U)));
INSTANTIATE_TEST_CASE_P(FindClassWithCollisions,
                        TypeLookupTableTest,
                        testing::Values(DescriptorClassDefIdxPair("LAB;", 1U)));
}  // namespace art
