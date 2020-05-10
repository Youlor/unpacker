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

#include "scoped_arena_allocator.h"

#include "arena_allocator.h"
#include "base/memory_tool.h"

namespace art {

static constexpr size_t kMemoryToolRedZoneBytes = 8;

ArenaStack::ArenaStack(ArenaPool* arena_pool)
  : DebugStackRefCounter(),
    stats_and_pool_(arena_pool),
    bottom_arena_(nullptr),
    top_arena_(nullptr),
    top_ptr_(nullptr),
    top_end_(nullptr) {
}

ArenaStack::~ArenaStack() {
  DebugStackRefCounter::CheckNoRefs();
  stats_and_pool_.pool->FreeArenaChain(bottom_arena_);
}

void ArenaStack::Reset() {
  DebugStackRefCounter::CheckNoRefs();
  stats_and_pool_.pool->FreeArenaChain(bottom_arena_);
  bottom_arena_ = nullptr;
  top_arena_  = nullptr;
  top_ptr_ = nullptr;
  top_end_ = nullptr;
}

MemStats ArenaStack::GetPeakStats() const {
  DebugStackRefCounter::CheckNoRefs();
  return MemStats("ArenaStack peak", static_cast<const TaggedStats<Peak>*>(&stats_and_pool_),
                  bottom_arena_);
}

uint8_t* ArenaStack::AllocateFromNextArena(size_t rounded_bytes) {
  UpdateBytesAllocated();
  size_t allocation_size = std::max(Arena::kDefaultSize, rounded_bytes);
  if (UNLIKELY(top_arena_ == nullptr)) {
    top_arena_ = bottom_arena_ = stats_and_pool_.pool->AllocArena(allocation_size);
    top_arena_->next_ = nullptr;
  } else if (top_arena_->next_ != nullptr && top_arena_->next_->Size() >= allocation_size) {
    top_arena_ = top_arena_->next_;
  } else {
    Arena* tail = top_arena_->next_;
    top_arena_->next_ = stats_and_pool_.pool->AllocArena(allocation_size);
    top_arena_ = top_arena_->next_;
    top_arena_->next_ = tail;
  }
  top_end_ = top_arena_->End();
  // top_ptr_ shall be updated by ScopedArenaAllocator.
  return top_arena_->Begin();
}

void ArenaStack::UpdatePeakStatsAndRestore(const ArenaAllocatorStats& restore_stats) {
  if (PeakStats()->BytesAllocated() < CurrentStats()->BytesAllocated()) {
    PeakStats()->Copy(*CurrentStats());
  }
  CurrentStats()->Copy(restore_stats);
}

void ArenaStack::UpdateBytesAllocated() {
  if (top_arena_ != nullptr) {
    // Update how many bytes we have allocated into the arena so that the arena pool knows how
    // much memory to zero out. Though ScopedArenaAllocator doesn't guarantee the memory is
    // zero-initialized, the Arena may be reused by ArenaAllocator which does guarantee this.
    size_t allocated = static_cast<size_t>(top_ptr_ - top_arena_->Begin());
    if (top_arena_->bytes_allocated_ < allocated) {
      top_arena_->bytes_allocated_ = allocated;
    }
  }
}

void* ArenaStack::AllocWithMemoryTool(size_t bytes, ArenaAllocKind kind) {
  // We mark all memory for a newly retrieved arena as inaccessible and then
  // mark only the actually allocated memory as defined. That leaves red zones
  // and padding between allocations marked as inaccessible.
  size_t rounded_bytes = RoundUp(bytes + kMemoryToolRedZoneBytes, 8);
  uint8_t* ptr = top_ptr_;
  if (UNLIKELY(static_cast<size_t>(top_end_ - ptr) < rounded_bytes)) {
    ptr = AllocateFromNextArena(rounded_bytes);
    CHECK(ptr != nullptr) << "Failed to allocate memory";
    MEMORY_TOOL_MAKE_NOACCESS(ptr, top_end_ - ptr);
  }
  CurrentStats()->RecordAlloc(bytes, kind);
  top_ptr_ = ptr + rounded_bytes;
  MEMORY_TOOL_MAKE_UNDEFINED(ptr, bytes);
  return ptr;
}

ScopedArenaAllocator::ScopedArenaAllocator(ArenaStack* arena_stack)
  : DebugStackReference(arena_stack),
    DebugStackRefCounter(),
    ArenaAllocatorStats(*arena_stack->CurrentStats()),
    arena_stack_(arena_stack),
    mark_arena_(arena_stack->top_arena_),
    mark_ptr_(arena_stack->top_ptr_),
    mark_end_(arena_stack->top_end_) {
}

ScopedArenaAllocator::~ScopedArenaAllocator() {
  DoReset();
}

void ScopedArenaAllocator::Reset() {
  DoReset();
  // If this allocator was Create()d, we need to move the arena_stack_->top_ptr_ past *this.
  if (mark_ptr_ == reinterpret_cast<uint8_t*>(this)) {
    arena_stack_->top_ptr_ = mark_ptr_ + RoundUp(sizeof(ScopedArenaAllocator), 8);
  }
}

void ScopedArenaAllocator::DoReset() {
  DebugStackReference::CheckTop();
  DebugStackRefCounter::CheckNoRefs();
  arena_stack_->UpdatePeakStatsAndRestore(*this);
  arena_stack_->UpdateBytesAllocated();
  if (LIKELY(mark_arena_ != nullptr)) {
    arena_stack_->top_arena_ = mark_arena_;
    arena_stack_->top_ptr_ = mark_ptr_;
    arena_stack_->top_end_ = mark_end_;
  } else if (arena_stack_->bottom_arena_ != nullptr) {
    mark_arena_ = arena_stack_->top_arena_ = arena_stack_->bottom_arena_;
    mark_ptr_ = arena_stack_->top_ptr_ = mark_arena_->Begin();
    mark_end_ = arena_stack_->top_end_ = mark_arena_->End();
  }
}

}  // namespace art
