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

#include "array.h"

#include "class.h"
#include "class-inl.h"
#include "class_linker-inl.h"
#include "common_throws.h"
#include "dex_file-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "object-inl.h"
#include "object_array.h"
#include "object_array-inl.h"
#include "handle_scope-inl.h"
#include "thread.h"
#include "utils.h"

namespace art {
namespace mirror {

// Create a multi-dimensional array of Objects or primitive types.
//
// We have to generate the names for X[], X[][], X[][][], and so on.  The
// easiest way to deal with that is to create the full name once and then
// subtract pieces off.  Besides, we want to start with the outermost
// piece and work our way in.
// Recursively create an array with multiple dimensions.  Elements may be
// Objects or primitive types.
static Array* RecursiveCreateMultiArray(Thread* self,
                                        Handle<Class> array_class, int current_dimension,
                                        Handle<mirror::IntArray> dimensions)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  int32_t array_length = dimensions->Get(current_dimension);
  StackHandleScope<1> hs(self);
  Handle<Array> new_array(
      hs.NewHandle(
          Array::Alloc<true>(self, array_class.Get(), array_length,
                             array_class->GetComponentSizeShift(),
                             Runtime::Current()->GetHeap()->GetCurrentAllocator())));
  if (UNLIKELY(new_array.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());
    return nullptr;
  }
  if (current_dimension + 1 < dimensions->GetLength()) {
    // Create a new sub-array in every element of the array.
    for (int32_t i = 0; i < array_length; i++) {
      StackHandleScope<1> hs2(self);
      Handle<mirror::Class> h_component_type(hs2.NewHandle(array_class->GetComponentType()));
      Array* sub_array = RecursiveCreateMultiArray(self, h_component_type,
                                                   current_dimension + 1, dimensions);
      if (UNLIKELY(sub_array == nullptr)) {
        CHECK(self->IsExceptionPending());
        return nullptr;
      }
      // Use non-transactional mode without check.
      new_array->AsObjectArray<Array>()->Set<false, false>(i, sub_array);
    }
  }
  return new_array.Get();
}

Array* Array::CreateMultiArray(Thread* self, Handle<Class> element_class,
                               Handle<IntArray> dimensions) {
  // Verify dimensions.
  //
  // The caller is responsible for verifying that "dimArray" is non-null
  // and has a length > 0 and <= 255.
  int num_dimensions = dimensions->GetLength();
  DCHECK_GT(num_dimensions, 0);
  DCHECK_LE(num_dimensions, 255);

  for (int i = 0; i < num_dimensions; i++) {
    int dimension = dimensions->Get(i);
    if (UNLIKELY(dimension < 0)) {
      ThrowNegativeArraySizeException(StringPrintf("Dimension %d: %d", i, dimension).c_str());
      return nullptr;
    }
  }

  // Find/generate the array class.
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  mirror::Class* element_class_ptr = element_class.Get();
  StackHandleScope<1> hs(self);
  MutableHandle<mirror::Class> array_class(
      hs.NewHandle(class_linker->FindArrayClass(self, &element_class_ptr)));
  if (UNLIKELY(array_class.Get() == nullptr)) {
    CHECK(self->IsExceptionPending());
    return nullptr;
  }
  for (int32_t i = 1; i < dimensions->GetLength(); ++i) {
    mirror::Class* array_class_ptr = array_class.Get();
    array_class.Assign(class_linker->FindArrayClass(self, &array_class_ptr));
    if (UNLIKELY(array_class.Get() == nullptr)) {
      CHECK(self->IsExceptionPending());
      return nullptr;
    }
  }
  // Create the array.
  Array* new_array = RecursiveCreateMultiArray(self, array_class, 0, dimensions);
  if (UNLIKELY(new_array == nullptr)) {
    CHECK(self->IsExceptionPending());
  }
  return new_array;
}

void Array::ThrowArrayIndexOutOfBoundsException(int32_t index) {
  art::ThrowArrayIndexOutOfBoundsException(index, GetLength());
}

void Array::ThrowArrayStoreException(Object* object) {
  art::ThrowArrayStoreException(object->GetClass(), this->GetClass());
}

Array* Array::CopyOf(Thread* self, int32_t new_length) {
  CHECK(GetClass()->GetComponentType()->IsPrimitive()) << "Will miss write barriers";
  DCHECK_GE(new_length, 0);
  // We may get copied by a compacting GC.
  StackHandleScope<1> hs(self);
  auto h_this(hs.NewHandle(this));
  auto* heap = Runtime::Current()->GetHeap();
  gc::AllocatorType allocator_type = heap->IsMovableObject(this) ? heap->GetCurrentAllocator() :
      heap->GetCurrentNonMovingAllocator();
  const auto component_size = GetClass()->GetComponentSize();
  const auto component_shift = GetClass()->GetComponentSizeShift();
  Array* new_array = Alloc<true>(self, GetClass(), new_length, component_shift, allocator_type);
  if (LIKELY(new_array != nullptr)) {
    memcpy(new_array->GetRawData(component_size, 0), h_this->GetRawData(component_size, 0),
           std::min(h_this->GetLength(), new_length) << component_shift);
  }
  return new_array;
}


template <typename T> GcRoot<Class> PrimitiveArray<T>::array_class_;

// Explicitly instantiate all the primitive array types.
template class PrimitiveArray<uint8_t>;   // BooleanArray
template class PrimitiveArray<int8_t>;    // ByteArray
template class PrimitiveArray<uint16_t>;  // CharArray
template class PrimitiveArray<double>;    // DoubleArray
template class PrimitiveArray<float>;     // FloatArray
template class PrimitiveArray<int32_t>;   // IntArray
template class PrimitiveArray<int64_t>;   // LongArray
template class PrimitiveArray<int16_t>;   // ShortArray

}  // namespace mirror
}  // namespace art
