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

#include "common_runtime_test.h"
#include "reference_queue.h"
#include "handle_scope-inl.h"
#include "mirror/class-inl.h"
#include "scoped_thread_state_change.h"

namespace art {
namespace gc {

class ReferenceQueueTest : public CommonRuntimeTest {};

TEST_F(ReferenceQueueTest, EnqueueDequeue) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  StackHandleScope<20> hs(self);
  Mutex lock("Reference queue lock");
  ReferenceQueue queue(&lock);
  ASSERT_TRUE(queue.IsEmpty());
  ASSERT_EQ(queue.GetLength(), 0U);
  auto ref_class = hs.NewHandle(
      Runtime::Current()->GetClassLinker()->FindClass(self, "Ljava/lang/ref/WeakReference;",
                                                      ScopedNullHandle<mirror::ClassLoader>()));
  ASSERT_TRUE(ref_class.Get() != nullptr);
  auto ref1(hs.NewHandle(ref_class->AllocObject(self)->AsReference()));
  ASSERT_TRUE(ref1.Get() != nullptr);
  auto ref2(hs.NewHandle(ref_class->AllocObject(self)->AsReference()));
  ASSERT_TRUE(ref2.Get() != nullptr);
  queue.EnqueueReference(ref1.Get());
  ASSERT_TRUE(!queue.IsEmpty());
  ASSERT_EQ(queue.GetLength(), 1U);
  queue.EnqueueReference(ref2.Get());
  ASSERT_TRUE(!queue.IsEmpty());
  ASSERT_EQ(queue.GetLength(), 2U);

  std::set<mirror::Reference*> refs = {ref1.Get(), ref2.Get()};
  std::set<mirror::Reference*> dequeued;
  dequeued.insert(queue.DequeuePendingReference());
  ASSERT_TRUE(!queue.IsEmpty());
  ASSERT_EQ(queue.GetLength(), 1U);
  dequeued.insert(queue.DequeuePendingReference());
  ASSERT_EQ(queue.GetLength(), 0U);
  ASSERT_TRUE(queue.IsEmpty());
  ASSERT_EQ(refs, dequeued);
}

TEST_F(ReferenceQueueTest, Dump) {
  Thread* self = Thread::Current();
  ScopedObjectAccess soa(self);
  StackHandleScope<20> hs(self);
  Mutex lock("Reference queue lock");
  ReferenceQueue queue(&lock);
  queue.Dump(LOG(INFO));
  auto weak_ref_class = hs.NewHandle(
      Runtime::Current()->GetClassLinker()->FindClass(self, "Ljava/lang/ref/WeakReference;",
                                                      ScopedNullHandle<mirror::ClassLoader>()));
  ASSERT_TRUE(weak_ref_class.Get() != nullptr);
  auto finalizer_ref_class = hs.NewHandle(
      Runtime::Current()->GetClassLinker()->FindClass(self, "Ljava/lang/ref/FinalizerReference;",
                                                      ScopedNullHandle<mirror::ClassLoader>()));
  ASSERT_TRUE(finalizer_ref_class.Get() != nullptr);
  auto ref1(hs.NewHandle(weak_ref_class->AllocObject(self)->AsReference()));
  ASSERT_TRUE(ref1.Get() != nullptr);
  auto ref2(hs.NewHandle(finalizer_ref_class->AllocObject(self)->AsReference()));
  ASSERT_TRUE(ref2.Get() != nullptr);
  queue.EnqueueReference(ref1.Get());
  queue.Dump(LOG(INFO));
  queue.EnqueueReference(ref2.Get());
  queue.Dump(LOG(INFO));
}

}  // namespace gc
}  // namespace art
