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

#include "monitor_pool.h"

#include "base/logging.h"
#include "base/mutex-inl.h"
#include "thread-inl.h"
#include "monitor.h"

namespace art {

namespace mirror {
  class Object;
}  // namespace mirror

MonitorPool::MonitorPool()
    : current_chunk_list_index_(0), num_chunks_(0), current_chunk_list_capacity_(0),
    first_free_(nullptr) {
  for (size_t i = 0; i < kMaxChunkLists; ++i) {
    monitor_chunks_[i] = nullptr;  // Not absolutely required, but ...
  }
  AllocateChunk();  // Get our first chunk.
}

// Assumes locks are held appropriately when necessary.
// We do not need a lock in the constructor, but we need one when in CreateMonitorInPool.
void MonitorPool::AllocateChunk() {
  DCHECK(first_free_ == nullptr);

  // Do we need to allocate another chunk list?
  if (num_chunks_ == current_chunk_list_capacity_) {
    if (current_chunk_list_capacity_ != 0U) {
      ++current_chunk_list_index_;
      CHECK_LT(current_chunk_list_index_, kMaxChunkLists) << "Out of space for inflated monitors";
      VLOG(monitor) << "Expanding to capacity "
          << 2 * ChunkListCapacity(current_chunk_list_index_) - kInitialChunkStorage;
    }  // else we're initializing
    current_chunk_list_capacity_ = ChunkListCapacity(current_chunk_list_index_);
    uintptr_t* new_list = new uintptr_t[current_chunk_list_capacity_]();
    DCHECK(monitor_chunks_[current_chunk_list_index_] == nullptr);
    monitor_chunks_[current_chunk_list_index_] = new_list;
    num_chunks_ = 0;
  }

  // Allocate the chunk.
  void* chunk = allocator_.allocate(kChunkSize);
  // Check we allocated memory.
  CHECK_NE(reinterpret_cast<uintptr_t>(nullptr), reinterpret_cast<uintptr_t>(chunk));
  // Check it is aligned as we need it.
  CHECK_EQ(0U, reinterpret_cast<uintptr_t>(chunk) % kMonitorAlignment);

  // Add the chunk.
  monitor_chunks_[current_chunk_list_index_][num_chunks_] = reinterpret_cast<uintptr_t>(chunk);
  num_chunks_++;

  // Set up the free list
  Monitor* last = reinterpret_cast<Monitor*>(reinterpret_cast<uintptr_t>(chunk) +
                                             (kChunkCapacity - 1) * kAlignedMonitorSize);
  last->next_free_ = nullptr;
  // Eagerly compute id.
  last->monitor_id_ = OffsetToMonitorId(current_chunk_list_index_* (kMaxListSize * kChunkSize)
      + (num_chunks_ - 1) * kChunkSize + (kChunkCapacity - 1) * kAlignedMonitorSize);
  for (size_t i = 0; i < kChunkCapacity - 1; ++i) {
    Monitor* before = reinterpret_cast<Monitor*>(reinterpret_cast<uintptr_t>(last) -
                                                 kAlignedMonitorSize);
    before->next_free_ = last;
    // Derive monitor_id from last.
    before->monitor_id_ = OffsetToMonitorId(MonitorIdToOffset(last->monitor_id_) -
                                            kAlignedMonitorSize);

    last = before;
  }
  DCHECK(last == reinterpret_cast<Monitor*>(chunk));
  first_free_ = last;
}

void MonitorPool::FreeInternal() {
  // This is on shutdown with NO_THREAD_SAFETY_ANALYSIS, can't/don't need to lock.
  DCHECK_NE(current_chunk_list_capacity_, 0UL);
  for (size_t i = 0; i <= current_chunk_list_index_; ++i) {
    DCHECK_NE(monitor_chunks_[i], static_cast<uintptr_t*>(nullptr));
    for (size_t j = 0; j < ChunkListCapacity(i); ++j) {
      if (i < current_chunk_list_index_ || j < num_chunks_) {
        DCHECK_NE(monitor_chunks_[i][j], 0U);
        allocator_.deallocate(reinterpret_cast<uint8_t*>(monitor_chunks_[i][j]), kChunkSize);
      } else {
        DCHECK_EQ(monitor_chunks_[i][j], 0U);
      }
    }
    delete[] monitor_chunks_[i];
  }
}

Monitor* MonitorPool::CreateMonitorInPool(Thread* self, Thread* owner, mirror::Object* obj,
                                          int32_t hash_code)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  // We are gonna allocate, so acquire the writer lock.
  MutexLock mu(self, *Locks::allocated_monitor_ids_lock_);

  // Enough space, or need to resize?
  if (first_free_ == nullptr) {
    VLOG(monitor) << "Allocating a new chunk.";
    AllocateChunk();
  }

  Monitor* mon_uninitialized = first_free_;
  first_free_ = first_free_->next_free_;

  // Pull out the id which was preinitialized.
  MonitorId id = mon_uninitialized->monitor_id_;

  // Initialize it.
  Monitor* monitor = new(mon_uninitialized) Monitor(self, owner, obj, hash_code, id);

  return monitor;
}

void MonitorPool::ReleaseMonitorToPool(Thread* self, Monitor* monitor) {
  // Might be racy with allocation, so acquire lock.
  MutexLock mu(self, *Locks::allocated_monitor_ids_lock_);

  // Keep the monitor id. Don't trust it's not cleared.
  MonitorId id = monitor->monitor_id_;

  // Call the destructor.
  // TODO: Exception safety?
  monitor->~Monitor();

  // Add to the head of the free list.
  monitor->next_free_ = first_free_;
  first_free_ = monitor;

  // Rewrite monitor id.
  monitor->monitor_id_ = id;
}

void MonitorPool::ReleaseMonitorsToPool(Thread* self, MonitorList::Monitors* monitors) {
  for (Monitor* mon : *monitors) {
    ReleaseMonitorToPool(self, mon);
  }
}

}  // namespace art
