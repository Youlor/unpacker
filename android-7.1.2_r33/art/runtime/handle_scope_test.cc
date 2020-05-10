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

#include "gtest/gtest.h"
#include "handle_scope-inl.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

// Handle scope with a fixed size which is allocated on the stack.
template<size_t kNumReferences>
class NoThreadStackHandleScope : public HandleScope {
 public:
  explicit NoThreadStackHandleScope(HandleScope* link) : HandleScope(link, kNumReferences) {
  }
  ~NoThreadStackHandleScope() {
  }

 private:
  // references_storage_ needs to be first so that it matches the address of references_
  StackReference<mirror::Object> references_storage_[kNumReferences];
};

// Test the offsets computed for members of HandleScope. Because of cross-compiling
// it is impossible the use OFFSETOF_MEMBER, so we do some reasonable computations ourselves. This
// test checks whether we do the right thing.
TEST(HandleScopeTest, Offsets) NO_THREAD_SAFETY_ANALYSIS {
  // As the members of HandleScope are private, we cannot use OFFSETOF_MEMBER
  // here. So do the inverse: set some data, and access it through pointers created from the offsets.
  NoThreadStackHandleScope<0x9ABC> test_table(reinterpret_cast<HandleScope*>(0x5678));
  test_table.SetReference(0, reinterpret_cast<mirror::Object*>(0x1234));

  uint8_t* table_base_ptr = reinterpret_cast<uint8_t*>(&test_table);

  {
    uintptr_t* link_ptr = reinterpret_cast<uintptr_t*>(table_base_ptr +
        HandleScope::LinkOffset(sizeof(void*)));
    EXPECT_EQ(*link_ptr, static_cast<size_t>(0x5678));
  }

  {
    uint32_t* num_ptr = reinterpret_cast<uint32_t*>(table_base_ptr +
        HandleScope::NumberOfReferencesOffset(sizeof(void*)));
    EXPECT_EQ(*num_ptr, static_cast<size_t>(0x9ABC));
  }

  {
    // Assume sizeof(StackReference<mirror::Object>) == sizeof(uint32_t)
    // TODO: How can we make this assumption-less but still access directly and fully?
    EXPECT_EQ(sizeof(StackReference<mirror::Object>), sizeof(uint32_t));

    uint32_t* ref_ptr = reinterpret_cast<uint32_t*>(table_base_ptr +
        HandleScope::ReferencesOffset(sizeof(void*)));
    EXPECT_EQ(*ref_ptr, static_cast<uint32_t>(0x1234));
  }
}

}  // namespace art
