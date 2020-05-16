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

#include "space_test.h"

#include "dlmalloc_space.h"
#include "rosalloc_space.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace gc {
namespace space {

enum MallocSpaceType {
  kMallocSpaceDlMalloc,
  kMallocSpaceRosAlloc,
};

class SpaceCreateTest : public SpaceTest<CommonRuntimeTestWithParam<MallocSpaceType>> {
 public:
  MallocSpace* CreateSpace(const std::string& name,
                           size_t initial_size,
                           size_t growth_limit,
                           size_t capacity,
                           uint8_t* requested_begin) {
    const MallocSpaceType type = GetParam();
    if (type == kMallocSpaceDlMalloc) {
      return DlMallocSpace::Create(name,
                                   initial_size,
                                   growth_limit,
                                   capacity,
                                   requested_begin,
                                   false);
    }
    DCHECK_EQ(static_cast<uint32_t>(type), static_cast<uint32_t>(kMallocSpaceRosAlloc));
    return RosAllocSpace::Create(name,
                                 initial_size,
                                 growth_limit,
                                 capacity,
                                 requested_begin,
                                 Runtime::Current()->GetHeap()->IsLowMemoryMode(),
                                 false);
  }
};

TEST_P(SpaceCreateTest, InitTestBody) {
  // This will lead to error messages in the log.
  ScopedLogSeverity sls(LogSeverity::FATAL);

  {
    // Init < max == growth
    std::unique_ptr<Space> space(CreateSpace("test", 16 * MB, 32 * MB, 32 * MB, nullptr));
    EXPECT_TRUE(space != nullptr);
    // Init == max == growth
    space.reset(CreateSpace("test", 16 * MB, 16 * MB, 16 * MB, nullptr));
    EXPECT_TRUE(space != nullptr);
    // Init > max == growth
    space.reset(CreateSpace("test", 32 * MB, 16 * MB, 16 * MB, nullptr));
    EXPECT_TRUE(space == nullptr);
    // Growth == init < max
    space.reset(CreateSpace("test", 16 * MB, 16 * MB, 32 * MB, nullptr));
    EXPECT_TRUE(space != nullptr);
    // Growth < init < max
    space.reset(CreateSpace("test", 16 * MB, 8 * MB, 32 * MB, nullptr));
    EXPECT_TRUE(space == nullptr);
    // Init < growth < max
    space.reset(CreateSpace("test", 8 * MB, 16 * MB, 32 * MB, nullptr));
    EXPECT_TRUE(space != nullptr);
    // Init < max < growth
    space.reset(CreateSpace("test", 8 * MB, 32 * MB, 16 * MB, nullptr));
    EXPECT_TRUE(space == nullptr);
  }
}

// TODO: This test is not very good, we should improve it.
// The test should do more allocations before the creation of the ZygoteSpace, and then do
// allocations after the ZygoteSpace is created. The test should also do some GCs to ensure that
// the GC works with the ZygoteSpace.
TEST_P(SpaceCreateTest, ZygoteSpaceTestBody) {
  size_t dummy;
  MallocSpace* space(CreateSpace("test", 4 * MB, 16 * MB, 16 * MB, nullptr));
  ASSERT_TRUE(space != nullptr);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  // Succeeds, fits without adjusting the footprint limit.
  size_t ptr1_bytes_allocated, ptr1_usable_size, ptr1_bytes_tl_bulk_allocated;
  StackHandleScope<3> hs(soa.Self());
  MutableHandle<mirror::Object> ptr1(hs.NewHandle(Alloc(space,
                                                        self,
                                                        1 * MB,
                                                        &ptr1_bytes_allocated,
                                                        &ptr1_usable_size,
                                                        &ptr1_bytes_tl_bulk_allocated)));
  EXPECT_TRUE(ptr1.Get() != nullptr);
  EXPECT_LE(1U * MB, ptr1_bytes_allocated);
  EXPECT_LE(1U * MB, ptr1_usable_size);
  EXPECT_LE(ptr1_usable_size, ptr1_bytes_allocated);
  EXPECT_EQ(ptr1_bytes_tl_bulk_allocated, ptr1_bytes_allocated);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr2 = Alloc(space, self, 8 * MB, &dummy, nullptr, &dummy);
  EXPECT_TRUE(ptr2 == nullptr);

  // Succeeds, adjusts the footprint.
  size_t ptr3_bytes_allocated, ptr3_usable_size, ptr3_bytes_tl_bulk_allocated;
  MutableHandle<mirror::Object> ptr3(hs.NewHandle(AllocWithGrowth(space,
                                                                  self,
                                                                  8 * MB,
                                                                  &ptr3_bytes_allocated,
                                                                  &ptr3_usable_size,
                                                                  &ptr3_bytes_tl_bulk_allocated)));
  EXPECT_TRUE(ptr3.Get() != nullptr);
  EXPECT_LE(8U * MB, ptr3_bytes_allocated);
  EXPECT_LE(8U * MB, ptr3_usable_size);
  EXPECT_LE(ptr3_usable_size, ptr3_bytes_allocated);
  EXPECT_EQ(ptr3_bytes_tl_bulk_allocated, ptr3_bytes_allocated);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr4 = space->Alloc(self, 8 * MB, &dummy, nullptr, &dummy);
  EXPECT_TRUE(ptr4 == nullptr);

  // Also fails, requires a higher allowed footprint.
  mirror::Object* ptr5 = space->AllocWithGrowth(self, 8 * MB, &dummy, nullptr, &dummy);
  EXPECT_TRUE(ptr5 == nullptr);

  // Release some memory.
  size_t free3 = space->AllocationSize(ptr3.Get(), nullptr);
  EXPECT_EQ(free3, ptr3_bytes_allocated);
  EXPECT_EQ(free3, space->Free(self, ptr3.Assign(nullptr)));
  EXPECT_LE(8U * MB, free3);

  // Succeeds, now that memory has been freed.
  size_t ptr6_bytes_allocated, ptr6_usable_size, ptr6_bytes_tl_bulk_allocated;
  Handle<mirror::Object> ptr6(hs.NewHandle(AllocWithGrowth(space,
                                                           self,
                                                           9 * MB,
                                                           &ptr6_bytes_allocated,
                                                           &ptr6_usable_size,
                                                           &ptr6_bytes_tl_bulk_allocated)));
  EXPECT_TRUE(ptr6.Get() != nullptr);
  EXPECT_LE(9U * MB, ptr6_bytes_allocated);
  EXPECT_LE(9U * MB, ptr6_usable_size);
  EXPECT_LE(ptr6_usable_size, ptr6_bytes_allocated);
  EXPECT_EQ(ptr6_bytes_tl_bulk_allocated, ptr6_bytes_allocated);

  // Final clean up.
  size_t free1 = space->AllocationSize(ptr1.Get(), nullptr);
  space->Free(self, ptr1.Assign(nullptr));
  EXPECT_LE(1U * MB, free1);

  // Make sure that the zygote space isn't directly at the start of the space.
  EXPECT_TRUE(space->Alloc(self, 1U * MB, &dummy, nullptr, &dummy) != nullptr);

  gc::Heap* heap = Runtime::Current()->GetHeap();
  space::Space* old_space = space;
  {
    ScopedThreadSuspension sts(self, kSuspended);
    ScopedSuspendAll ssa("Add image space");
    heap->RemoveSpace(old_space);
  }
  heap->RevokeAllThreadLocalBuffers();
  space::ZygoteSpace* zygote_space = space->CreateZygoteSpace("alloc space",
                                                              heap->IsLowMemoryMode(),
                                                              &space);
  delete old_space;
  // Add the zygote space.
  AddSpace(zygote_space, false);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space, false);

  // Succeeds, fits without adjusting the footprint limit.
  ptr1.Assign(Alloc(space,
                    self,
                    1 * MB,
                    &ptr1_bytes_allocated,
                    &ptr1_usable_size,
                    &ptr1_bytes_tl_bulk_allocated));
  EXPECT_TRUE(ptr1.Get() != nullptr);
  EXPECT_LE(1U * MB, ptr1_bytes_allocated);
  EXPECT_LE(1U * MB, ptr1_usable_size);
  EXPECT_LE(ptr1_usable_size, ptr1_bytes_allocated);
  EXPECT_EQ(ptr1_bytes_tl_bulk_allocated, ptr1_bytes_allocated);

  // Fails, requires a higher footprint limit.
  ptr2 = Alloc(space, self, 8 * MB, &dummy, nullptr, &dummy);
  EXPECT_TRUE(ptr2 == nullptr);

  // Succeeds, adjusts the footprint.
  ptr3.Assign(AllocWithGrowth(space,
                              self,
                              2 * MB,
                              &ptr3_bytes_allocated,
                              &ptr3_usable_size,
                              &ptr3_bytes_tl_bulk_allocated));
  EXPECT_TRUE(ptr3.Get() != nullptr);
  EXPECT_LE(2U * MB, ptr3_bytes_allocated);
  EXPECT_LE(2U * MB, ptr3_usable_size);
  EXPECT_LE(ptr3_usable_size, ptr3_bytes_allocated);
  EXPECT_EQ(ptr3_bytes_tl_bulk_allocated, ptr3_bytes_allocated);
  space->Free(self, ptr3.Assign(nullptr));

  // Final clean up.
  free1 = space->AllocationSize(ptr1.Get(), nullptr);
  space->Free(self, ptr1.Assign(nullptr));
  EXPECT_LE(1U * MB, free1);
}

TEST_P(SpaceCreateTest, AllocAndFreeTestBody) {
  size_t dummy = 0;
  MallocSpace* space(CreateSpace("test", 4 * MB, 16 * MB, 16 * MB, nullptr));
  ASSERT_TRUE(space != nullptr);
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);

  // Succeeds, fits without adjusting the footprint limit.
  size_t ptr1_bytes_allocated, ptr1_usable_size, ptr1_bytes_tl_bulk_allocated;
  StackHandleScope<3> hs(soa.Self());
  MutableHandle<mirror::Object> ptr1(hs.NewHandle(Alloc(space,
                                                        self,
                                                        1 * MB,
                                                        &ptr1_bytes_allocated,
                                                        &ptr1_usable_size,
                                                        &ptr1_bytes_tl_bulk_allocated)));
  EXPECT_TRUE(ptr1.Get() != nullptr);
  EXPECT_LE(1U * MB, ptr1_bytes_allocated);
  EXPECT_LE(1U * MB, ptr1_usable_size);
  EXPECT_LE(ptr1_usable_size, ptr1_bytes_allocated);
  EXPECT_EQ(ptr1_bytes_tl_bulk_allocated, ptr1_bytes_allocated);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr2 = Alloc(space, self, 8 * MB, &dummy, nullptr, &dummy);
  EXPECT_TRUE(ptr2 == nullptr);

  // Succeeds, adjusts the footprint.
  size_t ptr3_bytes_allocated, ptr3_usable_size, ptr3_bytes_tl_bulk_allocated;
  MutableHandle<mirror::Object> ptr3(hs.NewHandle(AllocWithGrowth(space,
                                                                  self,
                                                                  8 * MB,
                                                                  &ptr3_bytes_allocated,
                                                                  &ptr3_usable_size,
                                                                  &ptr3_bytes_tl_bulk_allocated)));
  EXPECT_TRUE(ptr3.Get() != nullptr);
  EXPECT_LE(8U * MB, ptr3_bytes_allocated);
  EXPECT_LE(8U * MB, ptr3_usable_size);
  EXPECT_LE(ptr3_usable_size, ptr3_bytes_allocated);
  EXPECT_EQ(ptr3_bytes_tl_bulk_allocated, ptr3_bytes_allocated);

  // Fails, requires a higher footprint limit.
  mirror::Object* ptr4 = Alloc(space, self, 8 * MB, &dummy, nullptr, &dummy);
  EXPECT_TRUE(ptr4 == nullptr);

  // Also fails, requires a higher allowed footprint.
  mirror::Object* ptr5 = AllocWithGrowth(space, self, 8 * MB, &dummy, nullptr, &dummy);
  EXPECT_TRUE(ptr5 == nullptr);

  // Release some memory.
  size_t free3 = space->AllocationSize(ptr3.Get(), nullptr);
  EXPECT_EQ(free3, ptr3_bytes_allocated);
  space->Free(self, ptr3.Assign(nullptr));
  EXPECT_LE(8U * MB, free3);

  // Succeeds, now that memory has been freed.
  size_t ptr6_bytes_allocated, ptr6_usable_size, ptr6_bytes_tl_bulk_allocated;
  Handle<mirror::Object> ptr6(hs.NewHandle(AllocWithGrowth(space,
                                                           self,
                                                           9 * MB,
                                                           &ptr6_bytes_allocated,
                                                           &ptr6_usable_size,
                                                           &ptr6_bytes_tl_bulk_allocated)));
  EXPECT_TRUE(ptr6.Get() != nullptr);
  EXPECT_LE(9U * MB, ptr6_bytes_allocated);
  EXPECT_LE(9U * MB, ptr6_usable_size);
  EXPECT_LE(ptr6_usable_size, ptr6_bytes_allocated);
  EXPECT_EQ(ptr6_bytes_tl_bulk_allocated, ptr6_bytes_allocated);

  // Final clean up.
  size_t free1 = space->AllocationSize(ptr1.Get(), nullptr);
  space->Free(self, ptr1.Assign(nullptr));
  EXPECT_LE(1U * MB, free1);
}

TEST_P(SpaceCreateTest, AllocAndFreeListTestBody) {
  MallocSpace* space(CreateSpace("test", 4 * MB, 16 * MB, 16 * MB, nullptr));
  ASSERT_TRUE(space != nullptr);

  // Make space findable to the heap, will also delete space when runtime is cleaned up
  AddSpace(space);
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);

  // Succeeds, fits without adjusting the max allowed footprint.
  mirror::Object* lots_of_objects[1024];
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    size_t allocation_size, usable_size, bytes_tl_bulk_allocated;
    size_t size_of_zero_length_byte_array = SizeOfZeroLengthByteArray();
    lots_of_objects[i] = Alloc(space,
                               self,
                               size_of_zero_length_byte_array,
                               &allocation_size,
                               &usable_size,
                               &bytes_tl_bulk_allocated);
    EXPECT_TRUE(lots_of_objects[i] != nullptr);
    size_t computed_usable_size;
    EXPECT_EQ(allocation_size, space->AllocationSize(lots_of_objects[i], &computed_usable_size));
    EXPECT_EQ(usable_size, computed_usable_size);
    EXPECT_TRUE(bytes_tl_bulk_allocated == 0 ||
                bytes_tl_bulk_allocated >= allocation_size);
  }

  // Release memory.
  space->FreeList(self, arraysize(lots_of_objects), lots_of_objects);

  // Succeeds, fits by adjusting the max allowed footprint.
  for (size_t i = 0; i < arraysize(lots_of_objects); i++) {
    size_t allocation_size, usable_size, bytes_tl_bulk_allocated;
    lots_of_objects[i] = AllocWithGrowth(space,
                                         self,
                                         1024,
                                         &allocation_size,
                                         &usable_size,
                                         &bytes_tl_bulk_allocated);
    EXPECT_TRUE(lots_of_objects[i] != nullptr);
    size_t computed_usable_size;
    EXPECT_EQ(allocation_size, space->AllocationSize(lots_of_objects[i], &computed_usable_size));
    EXPECT_EQ(usable_size, computed_usable_size);
    EXPECT_TRUE(bytes_tl_bulk_allocated == 0 ||
                bytes_tl_bulk_allocated >= allocation_size);
  }

  // Release memory.
  space->FreeList(self, arraysize(lots_of_objects), lots_of_objects);
}

INSTANTIATE_TEST_CASE_P(CreateRosAllocSpace,
                        SpaceCreateTest,
                        testing::Values(kMallocSpaceRosAlloc));
INSTANTIATE_TEST_CASE_P(CreateDlMallocSpace,
                        SpaceCreateTest,
                        testing::Values(kMallocSpaceDlMalloc));

}  // namespace space
}  // namespace gc
}  // namespace art
