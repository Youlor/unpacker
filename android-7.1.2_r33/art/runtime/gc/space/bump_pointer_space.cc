/*
 * Copyright (C) 2013 The Android Open Source Project
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

#include "bump_pointer_space.h"
#include "bump_pointer_space-inl.h"
#include "mirror/object-inl.h"
#include "mirror/class-inl.h"
#include "thread_list.h"

namespace art {
namespace gc {
namespace space {

BumpPointerSpace* BumpPointerSpace::Create(const std::string& name, size_t capacity,
                                           uint8_t* requested_begin) {
  capacity = RoundUp(capacity, kPageSize);
  std::string error_msg;
  std::unique_ptr<MemMap> mem_map(MemMap::MapAnonymous(name.c_str(), requested_begin, capacity,
                                                       PROT_READ | PROT_WRITE, true, false,
                                                       &error_msg));
  if (mem_map.get() == nullptr) {
    LOG(ERROR) << "Failed to allocate pages for alloc space (" << name << ") of size "
        << PrettySize(capacity) << " with message " << error_msg;
    return nullptr;
  }
  return new BumpPointerSpace(name, mem_map.release());
}

BumpPointerSpace* BumpPointerSpace::CreateFromMemMap(const std::string& name, MemMap* mem_map) {
  return new BumpPointerSpace(name, mem_map);
}

BumpPointerSpace::BumpPointerSpace(const std::string& name, uint8_t* begin, uint8_t* limit)
    : ContinuousMemMapAllocSpace(name, nullptr, begin, begin, limit,
                                 kGcRetentionPolicyAlwaysCollect),
      growth_end_(limit),
      objects_allocated_(0), bytes_allocated_(0),
      block_lock_("Block lock"),
      main_block_size_(0),
      num_blocks_(0) {
}

BumpPointerSpace::BumpPointerSpace(const std::string& name, MemMap* mem_map)
    : ContinuousMemMapAllocSpace(name, mem_map, mem_map->Begin(), mem_map->Begin(), mem_map->End(),
                                 kGcRetentionPolicyAlwaysCollect),
      growth_end_(mem_map->End()),
      objects_allocated_(0), bytes_allocated_(0),
      block_lock_("Block lock", kBumpPointerSpaceBlockLock),
      main_block_size_(0),
      num_blocks_(0) {
}

void BumpPointerSpace::Clear() {
  // Release the pages back to the operating system.
  if (!kMadviseZeroes) {
    memset(Begin(), 0, Limit() - Begin());
  }
  CHECK_NE(madvise(Begin(), Limit() - Begin(), MADV_DONTNEED), -1) << "madvise failed";
  // Reset the end of the space back to the beginning, we move the end forward as we allocate
  // objects.
  SetEnd(Begin());
  objects_allocated_.StoreRelaxed(0);
  bytes_allocated_.StoreRelaxed(0);
  growth_end_ = Limit();
  {
    MutexLock mu(Thread::Current(), block_lock_);
    num_blocks_ = 0;
    main_block_size_ = 0;
  }
}

void BumpPointerSpace::Dump(std::ostream& os) const {
  os << GetName() << " "
      << reinterpret_cast<void*>(Begin()) << "-" << reinterpret_cast<void*>(End()) << " - "
      << reinterpret_cast<void*>(Limit());
}

mirror::Object* BumpPointerSpace::GetNextObject(mirror::Object* obj) {
  const uintptr_t position = reinterpret_cast<uintptr_t>(obj) + obj->SizeOf();
  return reinterpret_cast<mirror::Object*>(RoundUp(position, kAlignment));
}

size_t BumpPointerSpace::RevokeThreadLocalBuffers(Thread* thread) {
  MutexLock mu(Thread::Current(), block_lock_);
  RevokeThreadLocalBuffersLocked(thread);
  return 0U;
}

size_t BumpPointerSpace::RevokeAllThreadLocalBuffers() {
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  // TODO: Not do a copy of the thread list?
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  for (Thread* thread : thread_list) {
    RevokeThreadLocalBuffers(thread);
  }
  return 0U;
}

void BumpPointerSpace::AssertThreadLocalBuffersAreRevoked(Thread* thread) {
  if (kIsDebugBuild) {
    MutexLock mu(Thread::Current(), block_lock_);
    DCHECK(!thread->HasTlab());
  }
}

void BumpPointerSpace::AssertAllThreadLocalBuffersAreRevoked() {
  if (kIsDebugBuild) {
    Thread* self = Thread::Current();
    MutexLock mu(self, *Locks::runtime_shutdown_lock_);
    MutexLock mu2(self, *Locks::thread_list_lock_);
    // TODO: Not do a copy of the thread list?
    std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
    for (Thread* thread : thread_list) {
      AssertThreadLocalBuffersAreRevoked(thread);
    }
  }
}

void BumpPointerSpace::UpdateMainBlock() {
  DCHECK_EQ(num_blocks_, 0U);
  main_block_size_ = Size();
}

// Returns the start of the storage.
uint8_t* BumpPointerSpace::AllocBlock(size_t bytes) {
  bytes = RoundUp(bytes, kAlignment);
  if (!num_blocks_) {
    UpdateMainBlock();
  }
  uint8_t* storage = reinterpret_cast<uint8_t*>(
      AllocNonvirtualWithoutAccounting(bytes + sizeof(BlockHeader)));
  if (LIKELY(storage != nullptr)) {
    BlockHeader* header = reinterpret_cast<BlockHeader*>(storage);
    header->size_ = bytes;  // Write out the block header.
    storage += sizeof(BlockHeader);
    ++num_blocks_;
  }
  return storage;
}

void BumpPointerSpace::Walk(ObjectCallback* callback, void* arg) {
  uint8_t* pos = Begin();
  uint8_t* end = End();
  uint8_t* main_end = pos;
  {
    MutexLock mu(Thread::Current(), block_lock_);
    // If we have 0 blocks then we need to update the main header since we have bump pointer style
    // allocation into an unbounded region (actually bounded by Capacity()).
    if (num_blocks_ == 0) {
      UpdateMainBlock();
    }
    main_end = Begin() + main_block_size_;
    if (num_blocks_ == 0) {
      // We don't have any other blocks, this means someone else may be allocating into the main
      // block. In this case, we don't want to try and visit the other blocks after the main block
      // since these could actually be part of the main block.
      end = main_end;
    }
  }
  // Walk all of the objects in the main block first.
  while (pos < main_end) {
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(pos);
    // No read barrier because obj may not be a valid object.
    if (obj->GetClass<kDefaultVerifyFlags, kWithoutReadBarrier>() == nullptr) {
      // There is a race condition where a thread has just allocated an object but not set the
      // class. We can't know the size of this object, so we don't visit it and exit the function
      // since there is guaranteed to be not other blocks.
      return;
    } else {
      callback(obj, arg);
      pos = reinterpret_cast<uint8_t*>(GetNextObject(obj));
    }
  }
  // Walk the other blocks (currently only TLABs).
  while (pos < end) {
    BlockHeader* header = reinterpret_cast<BlockHeader*>(pos);
    size_t block_size = header->size_;
    pos += sizeof(BlockHeader);  // Skip the header so that we know where the objects
    mirror::Object* obj = reinterpret_cast<mirror::Object*>(pos);
    const mirror::Object* end_obj = reinterpret_cast<const mirror::Object*>(pos + block_size);
    CHECK_LE(reinterpret_cast<const uint8_t*>(end_obj), End());
    // We don't know how many objects are allocated in the current block. When we hit a null class
    // assume its the end. TODO: Have a thread update the header when it flushes the block?
    // No read barrier because obj may not be a valid object.
    while (obj < end_obj && obj->GetClass<kDefaultVerifyFlags, kWithoutReadBarrier>() != nullptr) {
      callback(obj, arg);
      obj = GetNextObject(obj);
    }
    pos += block_size;
  }
}

accounting::ContinuousSpaceBitmap::SweepCallback* BumpPointerSpace::GetSweepCallback() {
  UNIMPLEMENTED(FATAL);
  UNREACHABLE();
}

uint64_t BumpPointerSpace::GetBytesAllocated() {
  // Start out pre-determined amount (blocks which are not being allocated into).
  uint64_t total = static_cast<uint64_t>(bytes_allocated_.LoadRelaxed());
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  MutexLock mu3(Thread::Current(), block_lock_);
  // If we don't have any blocks, we don't have any thread local buffers. This check is required
  // since there can exist multiple bump pointer spaces which exist at the same time.
  if (num_blocks_ > 0) {
    for (Thread* thread : thread_list) {
      total += thread->GetThreadLocalBytesAllocated();
    }
  }
  return total;
}

uint64_t BumpPointerSpace::GetObjectsAllocated() {
  // Start out pre-determined amount (blocks which are not being allocated into).
  uint64_t total = static_cast<uint64_t>(objects_allocated_.LoadRelaxed());
  Thread* self = Thread::Current();
  MutexLock mu(self, *Locks::runtime_shutdown_lock_);
  MutexLock mu2(self, *Locks::thread_list_lock_);
  std::list<Thread*> thread_list = Runtime::Current()->GetThreadList()->GetList();
  MutexLock mu3(Thread::Current(), block_lock_);
  // If we don't have any blocks, we don't have any thread local buffers. This check is required
  // since there can exist multiple bump pointer spaces which exist at the same time.
  if (num_blocks_ > 0) {
    for (Thread* thread : thread_list) {
      total += thread->GetThreadLocalObjectsAllocated();
    }
  }
  return total;
}

void BumpPointerSpace::RevokeThreadLocalBuffersLocked(Thread* thread) {
  objects_allocated_.FetchAndAddSequentiallyConsistent(thread->GetThreadLocalObjectsAllocated());
  bytes_allocated_.FetchAndAddSequentiallyConsistent(thread->GetThreadLocalBytesAllocated());
  thread->SetTlab(nullptr, nullptr);
}

bool BumpPointerSpace::AllocNewTlab(Thread* self, size_t bytes) {
  MutexLock mu(Thread::Current(), block_lock_);
  RevokeThreadLocalBuffersLocked(self);
  uint8_t* start = AllocBlock(bytes);
  if (start == nullptr) {
    return false;
  }
  self->SetTlab(start, start + bytes);
  return true;
}

void BumpPointerSpace::LogFragmentationAllocFailure(std::ostream& os,
                                                    size_t /* failed_alloc_bytes */) {
  size_t max_contiguous_allocation = Limit() - End();
  os << "; failed due to fragmentation (largest possible contiguous allocation "
     <<  max_contiguous_allocation << " bytes)";
  // Caller's job to print failed_alloc_bytes.
}

}  // namespace space
}  // namespace gc
}  // namespace art
