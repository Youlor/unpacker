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

#include <algorithm>
#include <iomanip>
#include <numeric>

#include "arena_allocator.h"
#include "logging.h"
#include "mem_map.h"
#include "mutex.h"
#include "thread-inl.h"
#include "systrace.h"

namespace art {

static constexpr size_t kMemoryToolRedZoneBytes = 8;
constexpr size_t Arena::kDefaultSize;

template <bool kCount>
const char* const ArenaAllocatorStatsImpl<kCount>::kAllocNames[] = {
  "Misc         ",
  "SwitchTbl    ",
  "SlowPaths    ",
  "GrowBitMap   ",
  "STL          ",
  "GraphBuilder ",
  "Graph        ",
  "BasicBlock   ",
  "BlockList    ",
  "RevPostOrder ",
  "LinearOrder  ",
  "ConstantsMap ",
  "Predecessors ",
  "Successors   ",
  "Dominated    ",
  "Instruction  ",
  "InvokeInputs ",
  "PhiInputs    ",
  "LoopInfo     ",
  "LIBackEdges  ",
  "TryCatchInf  ",
  "UseListNode  ",
  "Environment  ",
  "EnvVRegs     ",
  "EnvLocations ",
  "LocSummary   ",
  "SsaBuilder   ",
  "MoveOperands ",
  "CodeBuffer   ",
  "StackMaps    ",
  "Optimization ",
  "GVN          ",
  "InductionVar ",
  "BCE          ",
  "DCE          ",
  "LSE          ",
  "LICM         ",
  "SsaLiveness  ",
  "SsaPhiElim   ",
  "RefTypeProp  ",
  "SideEffects  ",
  "RegAllocator ",
  "RegAllocVldt ",
  "StackMapStm  ",
  "CodeGen      ",
  "Assembler    ",
  "ParallelMove ",
  "GraphChecker ",
  "Verifier     ",
  "CallingConv  ",
};

template <bool kCount>
ArenaAllocatorStatsImpl<kCount>::ArenaAllocatorStatsImpl()
    : num_allocations_(0u) {
  std::fill_n(alloc_stats_, arraysize(alloc_stats_), 0u);
}

template <bool kCount>
void ArenaAllocatorStatsImpl<kCount>::Copy(const ArenaAllocatorStatsImpl& other) {
  num_allocations_ = other.num_allocations_;
  std::copy(other.alloc_stats_, other.alloc_stats_ + arraysize(alloc_stats_), alloc_stats_);
}

template <bool kCount>
void ArenaAllocatorStatsImpl<kCount>::RecordAlloc(size_t bytes, ArenaAllocKind kind) {
  alloc_stats_[kind] += bytes;
  ++num_allocations_;
}

template <bool kCount>
size_t ArenaAllocatorStatsImpl<kCount>::NumAllocations() const {
  return num_allocations_;
}

template <bool kCount>
size_t ArenaAllocatorStatsImpl<kCount>::BytesAllocated() const {
  const size_t init = 0u;  // Initial value of the correct type.
  return std::accumulate(alloc_stats_, alloc_stats_ + arraysize(alloc_stats_), init);
}

template <bool kCount>
void ArenaAllocatorStatsImpl<kCount>::Dump(std::ostream& os, const Arena* first,
                                           ssize_t lost_bytes_adjustment) const {
  size_t malloc_bytes = 0u;
  size_t lost_bytes = 0u;
  size_t num_arenas = 0u;
  for (const Arena* arena = first; arena != nullptr; arena = arena->next_) {
    malloc_bytes += arena->Size();
    lost_bytes += arena->RemainingSpace();
    ++num_arenas;
  }
  // The lost_bytes_adjustment is used to make up for the fact that the current arena
  // may not have the bytes_allocated_ updated correctly.
  lost_bytes += lost_bytes_adjustment;
  const size_t bytes_allocated = BytesAllocated();
  os << " MEM: used: " << bytes_allocated << ", allocated: " << malloc_bytes
     << ", lost: " << lost_bytes << "\n";
  size_t num_allocations = NumAllocations();
  if (num_allocations != 0) {
    os << "Number of arenas allocated: " << num_arenas << ", Number of allocations: "
       << num_allocations << ", avg size: " << bytes_allocated / num_allocations << "\n";
  }
  os << "===== Allocation by kind\n";
  static_assert(arraysize(kAllocNames) == kNumArenaAllocKinds, "arraysize of kAllocNames");
  for (int i = 0; i < kNumArenaAllocKinds; i++) {
      os << kAllocNames[i] << std::setw(10) << alloc_stats_[i] << "\n";
  }
}

// Explicitly instantiate the used implementation.
template class ArenaAllocatorStatsImpl<kArenaAllocatorCountAllocations>;

void ArenaAllocatorMemoryTool::DoMakeDefined(void* ptr, size_t size) {
  MEMORY_TOOL_MAKE_DEFINED(ptr, size);
}

void ArenaAllocatorMemoryTool::DoMakeUndefined(void* ptr, size_t size) {
  MEMORY_TOOL_MAKE_UNDEFINED(ptr, size);
}

void ArenaAllocatorMemoryTool::DoMakeInaccessible(void* ptr, size_t size) {
  MEMORY_TOOL_MAKE_NOACCESS(ptr, size);
}

Arena::Arena() : bytes_allocated_(0), next_(nullptr) {
}

MallocArena::MallocArena(size_t size) {
  memory_ = reinterpret_cast<uint8_t*>(calloc(1, size));
  CHECK(memory_ != nullptr);  // Abort on OOM.
  size_ = size;
}

MallocArena::~MallocArena() {
  free(reinterpret_cast<void*>(memory_));
}

MemMapArena::MemMapArena(size_t size, bool low_4gb, const char* name) {
  std::string error_msg;
  map_.reset(MemMap::MapAnonymous(
      name, nullptr, size, PROT_READ | PROT_WRITE, low_4gb, false, &error_msg));
  CHECK(map_.get() != nullptr) << error_msg;
  memory_ = map_->Begin();
  size_ = map_->Size();
}

MemMapArena::~MemMapArena() {
  // Destroys MemMap via std::unique_ptr<>.
}

void MemMapArena::Release() {
  if (bytes_allocated_ > 0) {
    map_->MadviseDontNeedAndZero();
    bytes_allocated_ = 0;
  }
}

void Arena::Reset() {
  if (bytes_allocated_ > 0) {
    memset(Begin(), 0, bytes_allocated_);
    bytes_allocated_ = 0;
  }
}

ArenaPool::ArenaPool(bool use_malloc, bool low_4gb, const char* name)
    : use_malloc_(use_malloc),
      lock_("Arena pool lock", kArenaPoolLock),
      free_arenas_(nullptr),
      low_4gb_(low_4gb),
      name_(name) {
  if (low_4gb) {
    CHECK(!use_malloc) << "low4gb must use map implementation";
  }
  if (!use_malloc) {
    MemMap::Init();
  }
}

ArenaPool::~ArenaPool() {
  ReclaimMemory();
}

void ArenaPool::ReclaimMemory() {
  while (free_arenas_ != nullptr) {
    auto* arena = free_arenas_;
    free_arenas_ = free_arenas_->next_;
    delete arena;
  }
}

void ArenaPool::LockReclaimMemory() {
  MutexLock lock(Thread::Current(), lock_);
  ReclaimMemory();
}

Arena* ArenaPool::AllocArena(size_t size) {
  Thread* self = Thread::Current();
  Arena* ret = nullptr;
  {
    MutexLock lock(self, lock_);
    if (free_arenas_ != nullptr && LIKELY(free_arenas_->Size() >= size)) {
      ret = free_arenas_;
      free_arenas_ = free_arenas_->next_;
    }
  }
  if (ret == nullptr) {
    ret = use_malloc_ ? static_cast<Arena*>(new MallocArena(size)) :
        new MemMapArena(size, low_4gb_, name_);
  }
  ret->Reset();
  return ret;
}

void ArenaPool::TrimMaps() {
  if (!use_malloc_) {
    ScopedTrace trace(__PRETTY_FUNCTION__);
    // Doesn't work for malloc.
    MutexLock lock(Thread::Current(), lock_);
    for (auto* arena = free_arenas_; arena != nullptr; arena = arena->next_) {
      arena->Release();
    }
  }
}

size_t ArenaPool::GetBytesAllocated() const {
  size_t total = 0;
  MutexLock lock(Thread::Current(), lock_);
  for (Arena* arena = free_arenas_; arena != nullptr; arena = arena->next_) {
    total += arena->GetBytesAllocated();
  }
  return total;
}

void ArenaPool::FreeArenaChain(Arena* first) {
  if (UNLIKELY(RUNNING_ON_MEMORY_TOOL > 0)) {
    for (Arena* arena = first; arena != nullptr; arena = arena->next_) {
      MEMORY_TOOL_MAKE_UNDEFINED(arena->memory_, arena->bytes_allocated_);
    }
  }
  if (first != nullptr) {
    Arena* last = first;
    while (last->next_ != nullptr) {
      last = last->next_;
    }
    Thread* self = Thread::Current();
    MutexLock lock(self, lock_);
    last->next_ = free_arenas_;
    free_arenas_ = first;
  }
}

size_t ArenaAllocator::BytesAllocated() const {
  return ArenaAllocatorStats::BytesAllocated();
}

size_t ArenaAllocator::BytesUsed() const {
  size_t total = ptr_ - begin_;
  if (arena_head_ != nullptr) {
    for (Arena* cur_arena = arena_head_->next_; cur_arena != nullptr;
         cur_arena = cur_arena->next_) {
     total += cur_arena->GetBytesAllocated();
    }
  }
  return total;
}

ArenaAllocator::ArenaAllocator(ArenaPool* pool)
  : pool_(pool),
    begin_(nullptr),
    end_(nullptr),
    ptr_(nullptr),
    arena_head_(nullptr) {
}

void ArenaAllocator::UpdateBytesAllocated() {
  if (arena_head_ != nullptr) {
    // Update how many bytes we have allocated into the arena so that the arena pool knows how
    // much memory to zero out.
    arena_head_->bytes_allocated_ = ptr_ - begin_;
  }
}

void* ArenaAllocator::AllocWithMemoryTool(size_t bytes, ArenaAllocKind kind) {
  // We mark all memory for a newly retrieved arena as inaccessible and then
  // mark only the actually allocated memory as defined. That leaves red zones
  // and padding between allocations marked as inaccessible.
  size_t rounded_bytes = RoundUp(bytes + kMemoryToolRedZoneBytes, 8);
  ArenaAllocatorStats::RecordAlloc(rounded_bytes, kind);
  uint8_t* ret;
  if (UNLIKELY(rounded_bytes > static_cast<size_t>(end_ - ptr_))) {
    ret = AllocFromNewArena(rounded_bytes);
    uint8_t* noaccess_begin = ret + bytes;
    uint8_t* noaccess_end;
    if (ret == arena_head_->Begin()) {
      DCHECK(ptr_ - rounded_bytes == ret);
      noaccess_end = end_;
    } else {
      // We're still using the old arena but `ret` comes from a new one just after it.
      DCHECK(arena_head_->next_ != nullptr);
      DCHECK(ret == arena_head_->next_->Begin());
      DCHECK_EQ(rounded_bytes, arena_head_->next_->GetBytesAllocated());
      noaccess_end = arena_head_->next_->End();
    }
    MEMORY_TOOL_MAKE_NOACCESS(noaccess_begin, noaccess_end - noaccess_begin);
  } else {
    ret = ptr_;
    ptr_ += rounded_bytes;
  }
  MEMORY_TOOL_MAKE_DEFINED(ret, bytes);
  // Check that the memory is already zeroed out.
  DCHECK(std::all_of(ret, ret + bytes, [](uint8_t val) { return val == 0u; }));
  return ret;
}

ArenaAllocator::~ArenaAllocator() {
  // Reclaim all the arenas by giving them back to the thread pool.
  UpdateBytesAllocated();
  pool_->FreeArenaChain(arena_head_);
}

uint8_t* ArenaAllocator::AllocFromNewArena(size_t bytes) {
  Arena* new_arena = pool_->AllocArena(std::max(Arena::kDefaultSize, bytes));
  DCHECK(new_arena != nullptr);
  DCHECK_LE(bytes, new_arena->Size());
  if (static_cast<size_t>(end_ - ptr_) > new_arena->Size() - bytes) {
    // The old arena has more space remaining than the new one, so keep using it.
    // This can happen when the requested size is over half of the default size.
    DCHECK(arena_head_ != nullptr);
    new_arena->bytes_allocated_ = bytes;  // UpdateBytesAllocated() on the new_arena.
    new_arena->next_ = arena_head_->next_;
    arena_head_->next_ = new_arena;
  } else {
    UpdateBytesAllocated();
    new_arena->next_ = arena_head_;
    arena_head_ = new_arena;
    // Update our internal data structures.
    begin_ = new_arena->Begin();
    ptr_ = begin_ + bytes;
    end_ = new_arena->End();
  }
  return new_arena->Begin();
}

bool ArenaAllocator::Contains(const void* ptr) const {
  if (ptr >= begin_ && ptr < end_) {
    return true;
  }
  for (const Arena* cur_arena = arena_head_; cur_arena != nullptr; cur_arena = cur_arena->next_) {
    if (cur_arena->Contains(ptr)) {
      return true;
    }
  }
  return false;
}

MemStats::MemStats(const char* name, const ArenaAllocatorStats* stats, const Arena* first_arena,
                   ssize_t lost_bytes_adjustment)
    : name_(name),
      stats_(stats),
      first_arena_(first_arena),
      lost_bytes_adjustment_(lost_bytes_adjustment) {
}

void MemStats::Dump(std::ostream& os) const {
  os << name_ << " stats:\n";
  stats_->Dump(os, first_arena_, lost_bytes_adjustment_);
}

// Dump memory usage stats.
MemStats ArenaAllocator::GetMemStats() const {
  ssize_t lost_bytes_adjustment =
      (arena_head_ == nullptr) ? 0 : (end_ - ptr_) - arena_head_->RemainingSpace();
  return MemStats("ArenaAllocator", this, arena_head_, lost_bytes_adjustment);
}

}  // namespace art
