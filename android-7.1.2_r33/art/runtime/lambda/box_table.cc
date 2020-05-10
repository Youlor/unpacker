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
#include "lambda/box_table.h"

#include "base/mutex.h"
#include "common_throws.h"
#include "gc_root-inl.h"
#include "lambda/closure.h"
#include "lambda/leaking_allocator.h"
#include "mirror/method.h"
#include "mirror/object-inl.h"
#include "thread.h"

#include <vector>

namespace art {
namespace lambda {
// Temporarily represent the lambda Closure as its raw bytes in an array.
// TODO: Generate a proxy class for the closure when boxing the first time.
using BoxedClosurePointerType = mirror::ByteArray*;

static mirror::Class* GetBoxedClosureClass() SHARED_REQUIRES(Locks::mutator_lock_) {
  return mirror::ByteArray::GetArrayClass();
}

namespace {
  // Convenience functions to allocating/deleting box table copies of the closures.
  struct ClosureAllocator {
    // Deletes a Closure that was allocated through ::Allocate.
    static void Delete(Closure* ptr) {
      delete[] reinterpret_cast<char*>(ptr);
    }

    // Returns a well-aligned pointer to a newly allocated Closure on the 'new' heap.
    static Closure* Allocate(size_t size) {
      DCHECK_GE(size, sizeof(Closure));

      // TODO: Maybe point to the interior of the boxed closure object after we add proxy support?
      Closure* closure = reinterpret_cast<Closure*>(new char[size]);
      DCHECK_ALIGNED(closure, alignof(Closure));
      return closure;
    }
  };
}  // namespace

BoxTable::BoxTable()
  : allow_new_weaks_(true),
    new_weaks_condition_("lambda box table allowed weaks", *Locks::lambda_table_lock_) {}

BoxTable::~BoxTable() {
  // Free all the copies of our closures.
  for (auto map_iterator = map_.begin(); map_iterator != map_.end(); ) {
    std::pair<UnorderedMapKeyType, ValueType>& key_value_pair = *map_iterator;

    Closure* closure = key_value_pair.first;

    // Remove from the map first, so that it doesn't try to access dangling pointer.
    map_iterator = map_.Erase(map_iterator);

    // Safe to delete, no dangling pointers.
    ClosureAllocator::Delete(closure);
  }
}

mirror::Object* BoxTable::BoxLambda(const ClosureType& closure) {
  Thread* self = Thread::Current();

  {
    // TODO: Switch to ReaderMutexLock if ConditionVariable ever supports RW Mutexes
    /*Reader*/MutexLock mu(self, *Locks::lambda_table_lock_);
    BlockUntilWeaksAllowed();

    // Attempt to look up this object, it's possible it was already boxed previously.
    // If this is the case we *must* return the same object as before to maintain
    // referential equality.
    //
    // In managed code:
    //   Functional f = () -> 5;  // vF = create-lambda
    //   Object a = f;            // vA = box-lambda vA
    //   Object b = f;            // vB = box-lambda vB
    //   assert(a == f)
    ValueType value = FindBoxedLambda(closure);
    if (!value.IsNull()) {
      return value.Read();
    }

    // Otherwise we need to box ourselves and insert it into the hash map
  }

  // Release the lambda table lock here, so that thread suspension is allowed.

  // Convert the Closure into a managed byte[] which will serve
  // as the temporary 'boxed' version of the lambda. This is good enough
  // to check all the basic object identities that a boxed lambda must retain.
  // It's also good enough to contain all the captured primitive variables.

  // TODO: Boxing an innate lambda (i.e. made with create-lambda) should make a proxy class
  // TODO: Boxing a learned lambda (i.e. made with unbox-lambda) should return the original object
  BoxedClosurePointerType closure_as_array_object =
      mirror::ByteArray::Alloc(self, closure->GetSize());

  // There are no thread suspension points after this, so we don't need to put it into a handle.

  if (UNLIKELY(closure_as_array_object == nullptr)) {
    // Most likely an OOM has occurred.
    CHECK(self->IsExceptionPending());
    return nullptr;
  }

  // Write the raw closure data into the byte[].
  closure->CopyTo(closure_as_array_object->GetRawData(sizeof(uint8_t),  // component size
                                                      0 /*index*/),     // index
                  closure_as_array_object->GetLength());

  // The method has been successfully boxed into an object, now insert it into the hash map.
  {
    MutexLock mu(self, *Locks::lambda_table_lock_);
    BlockUntilWeaksAllowed();

    // Lookup the object again, it's possible another thread already boxed it while
    // we were allocating the object before.
    ValueType value = FindBoxedLambda(closure);
    if (UNLIKELY(!value.IsNull())) {
      // Let the GC clean up method_as_object at a later time.
      return value.Read();
    }

    // Otherwise we need to insert it into the hash map in this thread.

    // Make a copy for the box table to keep, in case the closure gets collected from the stack.
    // TODO: GC may need to sweep for roots in the box table's copy of the closure.
    Closure* closure_table_copy = ClosureAllocator::Allocate(closure->GetSize());
    closure->CopyTo(closure_table_copy, closure->GetSize());

    // The closure_table_copy needs to be deleted by us manually when we erase it from the map.

    // Actually insert into the table.
    map_.Insert({closure_table_copy, ValueType(closure_as_array_object)});
  }

  return closure_as_array_object;
}

bool BoxTable::UnboxLambda(mirror::Object* object, ClosureType* out_closure) {
  DCHECK(object != nullptr);
  *out_closure = nullptr;

  Thread* self = Thread::Current();

  // Note that we do not need to access lambda_table_lock_ here
  // since we don't need to look at the map.

  mirror::Object* boxed_closure_object = object;

  // Raise ClassCastException if object is not instanceof byte[]
  if (UNLIKELY(!boxed_closure_object->InstanceOf(GetBoxedClosureClass()))) {
    ThrowClassCastException(GetBoxedClosureClass(), boxed_closure_object->GetClass());
    return false;
  }

  // TODO(iam): We must check that the closure object extends/implements the type
  // specified in [type id]. This is not currently implemented since it's always a byte[].

  // If we got this far, the inputs are valid.
  // Shuffle the byte[] back into a raw closure, then allocate it, copy, and return it.
  BoxedClosurePointerType boxed_closure_as_array =
      down_cast<BoxedClosurePointerType>(boxed_closure_object);

  const int8_t* unaligned_interior_closure = boxed_closure_as_array->GetData();

  // Allocate a copy that can "escape" and copy the closure data into that.
  Closure* unboxed_closure =
      LeakingAllocator::MakeFlexibleInstance<Closure>(self, boxed_closure_as_array->GetLength());
  // TODO: don't just memcpy the closure, it's unsafe when we add references to the mix.
  memcpy(unboxed_closure, unaligned_interior_closure, boxed_closure_as_array->GetLength());

  DCHECK_EQ(unboxed_closure->GetSize(), static_cast<size_t>(boxed_closure_as_array->GetLength()));

  *out_closure = unboxed_closure;
  return true;
}

BoxTable::ValueType BoxTable::FindBoxedLambda(const ClosureType& closure) const {
  auto map_iterator = map_.Find(closure);
  if (map_iterator != map_.end()) {
    const std::pair<UnorderedMapKeyType, ValueType>& key_value_pair = *map_iterator;
    const ValueType& value = key_value_pair.second;

    DCHECK(!value.IsNull());  // Never store null boxes.
    return value;
  }

  return ValueType(nullptr);
}

void BoxTable::BlockUntilWeaksAllowed() {
  Thread* self = Thread::Current();
  while (UNLIKELY((!kUseReadBarrier && !allow_new_weaks_) ||
                  (kUseReadBarrier && !self->GetWeakRefAccessEnabled()))) {
    new_weaks_condition_.WaitHoldingLocks(self);  // wait while holding mutator lock
  }
}

void BoxTable::SweepWeakBoxedLambdas(IsMarkedVisitor* visitor) {
  DCHECK(visitor != nullptr);

  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  /*
   * Visit every weak root in our lambda box table.
   * Remove unmarked objects, update marked objects to new address.
   */
  std::vector<ClosureType> remove_list;
  for (auto map_iterator = map_.begin(); map_iterator != map_.end(); ) {
    std::pair<UnorderedMapKeyType, ValueType>& key_value_pair = *map_iterator;

    const ValueType& old_value = key_value_pair.second;

    // This does not need a read barrier because this is called by GC.
    mirror::Object* old_value_raw = old_value.Read<kWithoutReadBarrier>();
    mirror::Object* new_value = visitor->IsMarked(old_value_raw);

    if (new_value == nullptr) {
      // The object has been swept away.
      const ClosureType& closure = key_value_pair.first;

      // Delete the entry from the map.
      map_iterator = map_.Erase(map_iterator);

      // Clean up the memory by deleting the closure.
      ClosureAllocator::Delete(closure);

    } else {
      // The object has been moved.
      // Update the map.
      key_value_pair.second = ValueType(new_value);
      ++map_iterator;
    }
  }

  // Occasionally shrink the map to avoid growing very large.
  if (map_.CalculateLoadFactor() < kMinimumLoadFactor) {
    map_.ShrinkToMaximumLoad();
  }
}

void BoxTable::DisallowNewWeakBoxedLambdas() {
  CHECK(!kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  allow_new_weaks_ = false;
}

void BoxTable::AllowNewWeakBoxedLambdas() {
  CHECK(!kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);

  allow_new_weaks_ = true;
  new_weaks_condition_.Broadcast(self);
}

void BoxTable::BroadcastForNewWeakBoxedLambdas() {
  CHECK(kUseReadBarrier);
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::lambda_table_lock_);
  new_weaks_condition_.Broadcast(self);
}

void BoxTable::EmptyFn::MakeEmpty(std::pair<UnorderedMapKeyType, ValueType>& item) const {
  item.first = nullptr;

  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  item.second = ValueType();  // Also clear the GC root.
}

bool BoxTable::EmptyFn::IsEmpty(const std::pair<UnorderedMapKeyType, ValueType>& item) const {
  return item.first == nullptr;
}

bool BoxTable::EqualsFn::operator()(const UnorderedMapKeyType& lhs,
                                    const UnorderedMapKeyType& rhs) const {
  // Nothing needs this right now, but leave this assertion for later when
  // we need to look at the references inside of the closure.
  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());

  return lhs->ReferenceEquals(rhs);
}

size_t BoxTable::HashFn::operator()(const UnorderedMapKeyType& key) const {
  const lambda::Closure* closure = key;
  DCHECK_ALIGNED(closure, alignof(lambda::Closure));

  // Need to hold mutator_lock_ before calling into Closure::GetHashCode.
  Locks::mutator_lock_->AssertSharedHeld(Thread::Current());
  return closure->GetHashCode();
}

}  // namespace lambda
}  // namespace art
