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

#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "gc/accounting/card_table-inl.h"
#include "gc/accounting/space_bitmap-inl.h"
#include "handle_scope-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace gc {

class HeapTest : public CommonRuntimeTest {};

TEST_F(HeapTest, ClearGrowthLimit) {
  Heap* heap = Runtime::Current()->GetHeap();
  int64_t max_memory_before = heap->GetMaxMemory();
  int64_t total_memory_before = heap->GetTotalMemory();
  heap->ClearGrowthLimit();
  int64_t max_memory_after = heap->GetMaxMemory();
  int64_t total_memory_after = heap->GetTotalMemory();
  EXPECT_GE(max_memory_after, max_memory_before);
  EXPECT_GE(total_memory_after, total_memory_before);
}

TEST_F(HeapTest, GarbageCollectClassLinkerInit) {
  {
    ScopedObjectAccess soa(Thread::Current());
    // garbage is created during ClassLinker::Init

    StackHandleScope<1> hs(soa.Self());
    Handle<mirror::Class> c(
        hs.NewHandle(class_linker_->FindSystemClass(soa.Self(), "[Ljava/lang/Object;")));
    for (size_t i = 0; i < 1024; ++i) {
      StackHandleScope<1> hs2(soa.Self());
      Handle<mirror::ObjectArray<mirror::Object>> array(hs2.NewHandle(
          mirror::ObjectArray<mirror::Object>::Alloc(soa.Self(), c.Get(), 2048)));
      for (size_t j = 0; j < 2048; ++j) {
        mirror::String* string = mirror::String::AllocFromModifiedUtf8(soa.Self(), "hello, world!");
        // handle scope operator -> deferences the handle scope before running the method.
        array->Set<false>(j, string);
      }
    }
  }
  Runtime::Current()->GetHeap()->CollectGarbage(false);
}

TEST_F(HeapTest, HeapBitmapCapacityTest) {
  uint8_t* heap_begin = reinterpret_cast<uint8_t*>(0x1000);
  const size_t heap_capacity = kObjectAlignment * (sizeof(intptr_t) * 8 + 1);
  std::unique_ptr<accounting::ContinuousSpaceBitmap> bitmap(
      accounting::ContinuousSpaceBitmap::Create("test bitmap", heap_begin, heap_capacity));
  mirror::Object* fake_end_of_heap_object =
      reinterpret_cast<mirror::Object*>(&heap_begin[heap_capacity - kObjectAlignment]);
  bitmap->Set(fake_end_of_heap_object);
}

class ZygoteHeapTest : public CommonRuntimeTest {
  void SetUpRuntimeOptions(RuntimeOptions* options) {
    CommonRuntimeTest::SetUpRuntimeOptions(options);
    options->push_back(std::make_pair("-Xzygote", nullptr));
  }
};

TEST_F(ZygoteHeapTest, PreZygoteFork) {
  // Exercise Heap::PreZygoteFork() to check it does not crash.
  Runtime::Current()->GetHeap()->PreZygoteFork();
}

}  // namespace gc
}  // namespace art
