/*
 * Copyright (C) 2008 The Android Open Source Project
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

#include "space_bitmap-inl.h"

#include "art_field-inl.h"
#include "base/stringprintf.h"
#include "dex_file-inl.h"
#include "mem_map.h"
#include "mirror/object-inl.h"
#include "mirror/class-inl.h"
#include "mirror/object_array.h"

namespace art {
namespace gc {
namespace accounting {

template<size_t kAlignment>
size_t SpaceBitmap<kAlignment>::ComputeBitmapSize(uint64_t capacity) {
  const uint64_t kBytesCoveredPerWord = kAlignment * kBitsPerIntPtrT;
  return (RoundUp(capacity, kBytesCoveredPerWord) / kBytesCoveredPerWord) * sizeof(intptr_t);
}

template<size_t kAlignment>
size_t SpaceBitmap<kAlignment>::ComputeHeapSize(uint64_t bitmap_bytes) {
  return bitmap_bytes * kBitsPerByte * kAlignment;
}

template<size_t kAlignment>
SpaceBitmap<kAlignment>* SpaceBitmap<kAlignment>::CreateFromMemMap(
    const std::string& name, MemMap* mem_map, uint8_t* heap_begin, size_t heap_capacity) {
  CHECK(mem_map != nullptr);
  uintptr_t* bitmap_begin = reinterpret_cast<uintptr_t*>(mem_map->Begin());
  const size_t bitmap_size = ComputeBitmapSize(heap_capacity);
  return new SpaceBitmap(name, mem_map, bitmap_begin, bitmap_size, heap_begin);
}

template<size_t kAlignment>
SpaceBitmap<kAlignment>::SpaceBitmap(const std::string& name, MemMap* mem_map, uintptr_t* bitmap_begin,
                                     size_t bitmap_size, const void* heap_begin)
    : mem_map_(mem_map), bitmap_begin_(bitmap_begin), bitmap_size_(bitmap_size),
      heap_begin_(reinterpret_cast<uintptr_t>(heap_begin)),
      name_(name) {
  CHECK(bitmap_begin_ != nullptr);
  CHECK_NE(bitmap_size, 0U);
}

template<size_t kAlignment>
SpaceBitmap<kAlignment>::~SpaceBitmap() {}

template<size_t kAlignment>
SpaceBitmap<kAlignment>* SpaceBitmap<kAlignment>::Create(
    const std::string& name, uint8_t* heap_begin, size_t heap_capacity) {
  // Round up since heap_capacity is not necessarily a multiple of kAlignment * kBitsPerWord.
  const size_t bitmap_size = ComputeBitmapSize(heap_capacity);
  std::string error_msg;
  std::unique_ptr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), nullptr, bitmap_size,
                                                       PROT_READ | PROT_WRITE, false, false,
                                                       &error_msg));
  if (UNLIKELY(mem_map.get() == nullptr)) {
    LOG(ERROR) << "Failed to allocate bitmap " << name << ": " << error_msg;
    return nullptr;
  }
  return CreateFromMemMap(name, mem_map.release(), heap_begin, heap_capacity);
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::SetHeapLimit(uintptr_t new_end) {
  DCHECK_ALIGNED(new_end, kBitsPerIntPtrT * kAlignment);
  size_t new_size = OffsetToIndex(new_end - heap_begin_) * sizeof(intptr_t);
  if (new_size < bitmap_size_) {
    bitmap_size_ = new_size;
  }
  // Not sure if doing this trim is necessary, since nothing past the end of the heap capacity
  // should be marked.
}

template<size_t kAlignment>
std::string SpaceBitmap<kAlignment>::Dump() const {
  return StringPrintf("%s: %p-%p", name_.c_str(), reinterpret_cast<void*>(HeapBegin()),
                      reinterpret_cast<void*>(HeapLimit()));
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::Clear() {
  if (bitmap_begin_ != nullptr) {
    mem_map_->MadviseDontNeedAndZero();
  }
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::CopyFrom(SpaceBitmap* source_bitmap) {
  DCHECK_EQ(Size(), source_bitmap->Size());
  std::copy(source_bitmap->Begin(), source_bitmap->Begin() + source_bitmap->Size() / sizeof(intptr_t), Begin());
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::Walk(ObjectCallback* callback, void* arg) {
  CHECK(bitmap_begin_ != nullptr);
  CHECK(callback != nullptr);

  uintptr_t end = OffsetToIndex(HeapLimit() - heap_begin_ - 1);
  uintptr_t* bitmap_begin = bitmap_begin_;
  for (uintptr_t i = 0; i <= end; ++i) {
    uintptr_t w = bitmap_begin[i];
    if (w != 0) {
      uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
      do {
        const size_t shift = CTZ(w);
        mirror::Object* obj = reinterpret_cast<mirror::Object*>(ptr_base + shift * kAlignment);
        (*callback)(obj, arg);
        w ^= (static_cast<uintptr_t>(1)) << shift;
      } while (w != 0);
    }
  }
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::SweepWalk(const SpaceBitmap<kAlignment>& live_bitmap,
                                        const SpaceBitmap<kAlignment>& mark_bitmap,
                                        uintptr_t sweep_begin, uintptr_t sweep_end,
                                        SpaceBitmap::SweepCallback* callback, void* arg) {
  CHECK(live_bitmap.bitmap_begin_ != nullptr);
  CHECK(mark_bitmap.bitmap_begin_ != nullptr);
  CHECK_EQ(live_bitmap.heap_begin_, mark_bitmap.heap_begin_);
  CHECK_EQ(live_bitmap.bitmap_size_, mark_bitmap.bitmap_size_);
  CHECK(callback != nullptr);
  CHECK_LE(sweep_begin, sweep_end);
  CHECK_GE(sweep_begin, live_bitmap.heap_begin_);

  if (sweep_end <= sweep_begin) {
    return;
  }

  // TODO: rewrite the callbacks to accept a std::vector<mirror::Object*> rather than a mirror::Object**?
  constexpr size_t buffer_size = sizeof(intptr_t) * kBitsPerIntPtrT;
#ifdef __LP64__
  // Heap-allocate for smaller stack frame.
  std::unique_ptr<mirror::Object*[]> pointer_buf_ptr(new mirror::Object*[buffer_size]);
  mirror::Object** pointer_buf = pointer_buf_ptr.get();
#else
  // Stack-allocate buffer as it's small enough.
  mirror::Object* pointer_buf[buffer_size];
#endif
  mirror::Object** pb = &pointer_buf[0];

  size_t start = OffsetToIndex(sweep_begin - live_bitmap.heap_begin_);
  size_t end = OffsetToIndex(sweep_end - live_bitmap.heap_begin_ - 1);
  CHECK_LT(end, live_bitmap.Size() / sizeof(intptr_t));
  uintptr_t* live = live_bitmap.bitmap_begin_;
  uintptr_t* mark = mark_bitmap.bitmap_begin_;
  for (size_t i = start; i <= end; i++) {
    uintptr_t garbage = live[i] & ~mark[i];
    if (UNLIKELY(garbage != 0)) {
      uintptr_t ptr_base = IndexToOffset(i) + live_bitmap.heap_begin_;
      do {
        const size_t shift = CTZ(garbage);
        garbage ^= (static_cast<uintptr_t>(1)) << shift;
        *pb++ = reinterpret_cast<mirror::Object*>(ptr_base + shift * kAlignment);
      } while (garbage != 0);
      // Make sure that there are always enough slots available for an
      // entire word of one bits.
      if (pb >= &pointer_buf[buffer_size - kBitsPerIntPtrT]) {
        (*callback)(pb - &pointer_buf[0], &pointer_buf[0], arg);
        pb = &pointer_buf[0];
      }
    }
  }
  if (pb > &pointer_buf[0]) {
    (*callback)(pb - &pointer_buf[0], &pointer_buf[0], arg);
  }
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::WalkInstanceFields(SpaceBitmap<kAlignment>* visited,
                                                 ObjectCallback* callback, mirror::Object* obj,
                                                 mirror::Class* klass, void* arg)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // Visit fields of parent classes first.
  mirror::Class* super = klass->GetSuperClass();
  if (super != nullptr) {
    WalkInstanceFields(visited, callback, obj, super, arg);
  }
  // Walk instance fields
  for (ArtField& field : klass->GetIFields()) {
    if (!field.IsPrimitiveType()) {
      mirror::Object* value = field.GetObj(obj);
      if (value != nullptr) {
        WalkFieldsInOrder(visited, callback, value, arg);
      }
    }
  }
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::WalkFieldsInOrder(SpaceBitmap<kAlignment>* visited,
                                                ObjectCallback* callback, mirror::Object* obj,
                                                void* arg) {
  if (visited->Test(obj)) {
    return;
  }
  // visit the object itself
  (*callback)(obj, arg);
  visited->Set(obj);
  // Walk instance fields of all objects
  mirror::Class* klass = obj->GetClass();
  WalkInstanceFields(visited, callback, obj, klass, arg);
  // Walk static fields of a Class
  if (obj->IsClass()) {
    for (ArtField& field : klass->GetSFields()) {
      if (!field.IsPrimitiveType()) {
        mirror::Object* value = field.GetObj(nullptr);
        if (value != nullptr) {
          WalkFieldsInOrder(visited, callback, value, arg);
        }
      }
    }
  } else if (obj->IsObjectArray()) {
    // Walk elements of an object array
    mirror::ObjectArray<mirror::Object>* obj_array = obj->AsObjectArray<mirror::Object>();
    int32_t length = obj_array->GetLength();
    for (int32_t i = 0; i < length; i++) {
      mirror::Object* value = obj_array->Get(i);
      if (value != nullptr) {
        WalkFieldsInOrder(visited, callback, value, arg);
      }
    }
  }
}

template<size_t kAlignment>
void SpaceBitmap<kAlignment>::InOrderWalk(ObjectCallback* callback, void* arg) {
  std::unique_ptr<SpaceBitmap<kAlignment>> visited(
      Create("bitmap for in-order walk", reinterpret_cast<uint8_t*>(heap_begin_),
             IndexToOffset(bitmap_size_ / sizeof(intptr_t))));
  CHECK(bitmap_begin_ != nullptr);
  CHECK(callback != nullptr);
  uintptr_t end = Size() / sizeof(intptr_t);
  for (uintptr_t i = 0; i < end; ++i) {
    // Need uint for unsigned shift.
    uintptr_t w = bitmap_begin_[i];
    if (UNLIKELY(w != 0)) {
      uintptr_t ptr_base = IndexToOffset(i) + heap_begin_;
      while (w != 0) {
        const size_t shift = CTZ(w);
        mirror::Object* obj = reinterpret_cast<mirror::Object*>(ptr_base + shift * kAlignment);
        WalkFieldsInOrder(visited.get(), callback, obj, arg);
        w ^= (static_cast<uintptr_t>(1)) << shift;
      }
    }
  }
}

template class SpaceBitmap<kObjectAlignment>;
template class SpaceBitmap<kPageSize>;

}  // namespace accounting
}  // namespace gc
}  // namespace art
