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

#include "mem_map.h"

#include "base/memory_tool.h"
#include <backtrace/BacktraceMap.h>
#include <inttypes.h>
#include <stdlib.h>

#include <memory>
#include <sstream>

#include "base/stringprintf.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "ScopedFd.h"
#pragma GCC diagnostic pop

#include "thread-inl.h"
#include "utils.h"

#include <cutils/ashmem.h>

#ifndef ANDROID_OS
#include <sys/resource.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

namespace art {

static std::ostream& operator<<(
    std::ostream& os,
    std::pair<BacktraceMap::const_iterator, BacktraceMap::const_iterator> iters) {
  for (BacktraceMap::const_iterator it = iters.first; it != iters.second; ++it) {
    os << StringPrintf("0x%08x-0x%08x %c%c%c %s\n",
                       static_cast<uint32_t>(it->start),
                       static_cast<uint32_t>(it->end),
                       (it->flags & PROT_READ) ? 'r' : '-',
                       (it->flags & PROT_WRITE) ? 'w' : '-',
                       (it->flags & PROT_EXEC) ? 'x' : '-', it->name.c_str());
  }
  return os;
}

std::ostream& operator<<(std::ostream& os, const MemMap::Maps& mem_maps) {
  os << "MemMap:" << std::endl;
  for (auto it = mem_maps.begin(); it != mem_maps.end(); ++it) {
    void* base = it->first;
    MemMap* map = it->second;
    CHECK_EQ(base, map->BaseBegin());
    os << *map << std::endl;
  }
  return os;
}

MemMap::Maps* MemMap::maps_ = nullptr;

#if USE_ART_LOW_4G_ALLOCATOR
// Handling mem_map in 32b address range for 64b architectures that do not support MAP_32BIT.

// The regular start of memory allocations. The first 64KB is protected by SELinux.
static constexpr uintptr_t LOW_MEM_START = 64 * KB;

// Generate random starting position.
// To not interfere with image position, take the image's address and only place it below. Current
// formula (sketch):
//
// ART_BASE_ADDR      = 0001XXXXXXXXXXXXXXX
// ----------------------------------------
//                    = 0000111111111111111
// & ~(kPageSize - 1) =~0000000000000001111
// ----------------------------------------
// mask               = 0000111111111110000
// & random data      = YYYYYYYYYYYYYYYYYYY
// -----------------------------------
// tmp                = 0000YYYYYYYYYYY0000
// + LOW_MEM_START    = 0000000000001000000
// --------------------------------------
// start
//
// arc4random as an entropy source is exposed in Bionic, but not in glibc. When we
// do not have Bionic, simply start with LOW_MEM_START.

// Function is standalone so it can be tested somewhat in mem_map_test.cc.
#ifdef __BIONIC__
uintptr_t CreateStartPos(uint64_t input) {
  CHECK_NE(0, ART_BASE_ADDRESS);

  // Start with all bits below highest bit in ART_BASE_ADDRESS.
  constexpr size_t leading_zeros = CLZ(static_cast<uint32_t>(ART_BASE_ADDRESS));
  constexpr uintptr_t mask_ones = (1 << (31 - leading_zeros)) - 1;

  // Lowest (usually 12) bits are not used, as aligned by page size.
  constexpr uintptr_t mask = mask_ones & ~(kPageSize - 1);

  // Mask input data.
  return (input & mask) + LOW_MEM_START;
}
#endif

static uintptr_t GenerateNextMemPos() {
#ifdef __BIONIC__
  uint64_t random_data;
  arc4random_buf(&random_data, sizeof(random_data));
  return CreateStartPos(random_data);
#else
  // No arc4random on host, see above.
  return LOW_MEM_START;
#endif
}

// Initialize linear scan to random position.
uintptr_t MemMap::next_mem_pos_ = GenerateNextMemPos();
#endif

// Return true if the address range is contained in a single memory map by either reading
// the maps_ variable or the /proc/self/map entry.
bool MemMap::ContainedWithinExistingMap(uint8_t* ptr, size_t size, std::string* error_msg) {
  uintptr_t begin = reinterpret_cast<uintptr_t>(ptr);
  uintptr_t end = begin + size;

  // There is a suspicion that BacktraceMap::Create is occasionally missing maps. TODO: Investigate
  // further.
  {
    MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
    for (auto& pair : *maps_) {
      MemMap* const map = pair.second;
      if (begin >= reinterpret_cast<uintptr_t>(map->Begin()) &&
          end <= reinterpret_cast<uintptr_t>(map->End())) {
        return true;
      }
    }
  }

  std::unique_ptr<BacktraceMap> map(BacktraceMap::Create(getpid(), true));
  if (map == nullptr) {
    if (error_msg != nullptr) {
      *error_msg = StringPrintf("Failed to build process map");
    }
    return false;
  }

  ScopedBacktraceMapIteratorLock lock(map.get());
  for (BacktraceMap::const_iterator it = map->begin(); it != map->end(); ++it) {
    if ((begin >= it->start && begin < it->end)  // start of new within old
        && (end > it->start && end <= it->end)) {  // end of new within old
      return true;
    }
  }
  if (error_msg != nullptr) {
    PrintFileToLog("/proc/self/maps", LogSeverity::ERROR);
    *error_msg = StringPrintf("Requested region 0x%08" PRIxPTR "-0x%08" PRIxPTR " does not overlap "
                              "any existing map. See process maps in the log.", begin, end);
  }
  return false;
}

// Return true if the address range does not conflict with any /proc/self/maps entry.
static bool CheckNonOverlapping(uintptr_t begin,
                                uintptr_t end,
                                std::string* error_msg) {
  std::unique_ptr<BacktraceMap> map(BacktraceMap::Create(getpid(), true));
  if (map.get() == nullptr) {
    *error_msg = StringPrintf("Failed to build process map");
    return false;
  }
  ScopedBacktraceMapIteratorLock(map.get());
  for (BacktraceMap::const_iterator it = map->begin(); it != map->end(); ++it) {
    if ((begin >= it->start && begin < it->end)      // start of new within old
        || (end > it->start && end < it->end)        // end of new within old
        || (begin <= it->start && end > it->end)) {  // start/end of new includes all of old
      std::ostringstream map_info;
      map_info << std::make_pair(it, map->end());
      *error_msg = StringPrintf("Requested region 0x%08" PRIxPTR "-0x%08" PRIxPTR " overlaps with "
                                "existing map 0x%08" PRIxPTR "-0x%08" PRIxPTR " (%s)\n%s",
                                begin, end,
                                static_cast<uintptr_t>(it->start), static_cast<uintptr_t>(it->end),
                                it->name.c_str(),
                                map_info.str().c_str());
      return false;
    }
  }
  return true;
}

// CheckMapRequest to validate a non-MAP_FAILED mmap result based on
// the expected value, calling munmap if validation fails, giving the
// reason in error_msg.
//
// If the expected_ptr is null, nothing is checked beyond the fact
// that the actual_ptr is not MAP_FAILED. However, if expected_ptr is
// non-null, we check that pointer is the actual_ptr == expected_ptr,
// and if not, report in error_msg what the conflict mapping was if
// found, or a generic error in other cases.
static bool CheckMapRequest(uint8_t* expected_ptr, void* actual_ptr, size_t byte_count,
                            std::string* error_msg) {
  // Handled first by caller for more specific error messages.
  CHECK(actual_ptr != MAP_FAILED);

  if (expected_ptr == nullptr) {
    return true;
  }

  uintptr_t actual = reinterpret_cast<uintptr_t>(actual_ptr);
  uintptr_t expected = reinterpret_cast<uintptr_t>(expected_ptr);
  uintptr_t limit = expected + byte_count;

  if (expected_ptr == actual_ptr) {
    return true;
  }

  // We asked for an address but didn't get what we wanted, all paths below here should fail.
  int result = munmap(actual_ptr, byte_count);
  if (result == -1) {
    PLOG(WARNING) << StringPrintf("munmap(%p, %zd) failed", actual_ptr, byte_count);
  }

  if (error_msg != nullptr) {
    // We call this here so that we can try and generate a full error
    // message with the overlapping mapping. There's no guarantee that
    // that there will be an overlap though, since
    // - The kernel is not *required* to honor expected_ptr unless MAP_FIXED is
    //   true, even if there is no overlap
    // - There might have been an overlap at the point of mmap, but the
    //   overlapping region has since been unmapped.
    std::string error_detail;
    CheckNonOverlapping(expected, limit, &error_detail);
    std::ostringstream os;
    os <<  StringPrintf("Failed to mmap at expected address, mapped at "
                        "0x%08" PRIxPTR " instead of 0x%08" PRIxPTR,
                        actual, expected);
    if (!error_detail.empty()) {
      os << " : " << error_detail;
    }
    *error_msg = os.str();
  }
  return false;
}

#if USE_ART_LOW_4G_ALLOCATOR
static inline void* TryMemMapLow4GB(void* ptr,
                                    size_t page_aligned_byte_count,
                                    int prot,
                                    int flags,
                                    int fd,
                                    off_t offset) {
  void* actual = mmap(ptr, page_aligned_byte_count, prot, flags, fd, offset);
  if (actual != MAP_FAILED) {
    // Since we didn't use MAP_FIXED the kernel may have mapped it somewhere not in the low
    // 4GB. If this is the case, unmap and retry.
    if (reinterpret_cast<uintptr_t>(actual) + page_aligned_byte_count >= 4 * GB) {
      munmap(actual, page_aligned_byte_count);
      actual = MAP_FAILED;
    }
  }
  return actual;
}
#endif

MemMap* MemMap::MapAnonymous(const char* name,
                             uint8_t* expected_ptr,
                             size_t byte_count,
                             int prot,
                             bool low_4gb,
                             bool reuse,
                             std::string* error_msg,
                             bool use_ashmem) {
#ifndef __LP64__
  UNUSED(low_4gb);
#endif
  if (byte_count == 0) {
    return new MemMap(name, nullptr, 0, nullptr, 0, prot, false);
  }
  size_t page_aligned_byte_count = RoundUp(byte_count, kPageSize);

  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (reuse) {
    // reuse means it is okay that it overlaps an existing page mapping.
    // Only use this if you actually made the page reservation yourself.
    CHECK(expected_ptr != nullptr);

    DCHECK(ContainedWithinExistingMap(expected_ptr, byte_count, error_msg)) << *error_msg;
    flags |= MAP_FIXED;
  }

  ScopedFd fd(-1);

  if (use_ashmem) {
    if (!kIsTargetBuild) {
      // When not on Android ashmem is faked using files in /tmp. Ensure that such files won't
      // fail due to ulimit restrictions. If they will then use a regular mmap.
      struct rlimit rlimit_fsize;
      CHECK_EQ(getrlimit(RLIMIT_FSIZE, &rlimit_fsize), 0);
      use_ashmem = (rlimit_fsize.rlim_cur == RLIM_INFINITY) ||
        (page_aligned_byte_count < rlimit_fsize.rlim_cur);
    }
  }

  if (use_ashmem) {
    // android_os_Debug.cpp read_mapinfo assumes all ashmem regions associated with the VM are
    // prefixed "dalvik-".
    std::string debug_friendly_name("dalvik-");
    debug_friendly_name += name;
    fd.reset(ashmem_create_region(debug_friendly_name.c_str(), page_aligned_byte_count));
    if (fd.get() == -1) {
      *error_msg = StringPrintf("ashmem_create_region failed for '%s': %s", name, strerror(errno));
      return nullptr;
    }
    flags &= ~MAP_ANONYMOUS;
  }

  // We need to store and potentially set an error number for pretty printing of errors
  int saved_errno = 0;

  void* actual = MapInternal(expected_ptr,
                             page_aligned_byte_count,
                             prot,
                             flags,
                             fd.get(),
                             0,
                             low_4gb);
  saved_errno = errno;

  if (actual == MAP_FAILED) {
    if (error_msg != nullptr) {
      PrintFileToLog("/proc/self/maps", LogSeverity::WARNING);

      *error_msg = StringPrintf("Failed anonymous mmap(%p, %zd, 0x%x, 0x%x, %d, 0): %s. "
                                    "See process maps in the log.",
                                expected_ptr,
                                page_aligned_byte_count,
                                prot,
                                flags,
                                fd.get(),
                                strerror(saved_errno));
    }
    return nullptr;
  }
  std::ostringstream check_map_request_error_msg;
  if (!CheckMapRequest(expected_ptr, actual, page_aligned_byte_count, error_msg)) {
    return nullptr;
  }
  return new MemMap(name, reinterpret_cast<uint8_t*>(actual), byte_count, actual,
                    page_aligned_byte_count, prot, reuse);
}

MemMap* MemMap::MapDummy(const char* name, uint8_t* addr, size_t byte_count) {
  if (byte_count == 0) {
    return new MemMap(name, nullptr, 0, nullptr, 0, 0, false);
  }
  const size_t page_aligned_byte_count = RoundUp(byte_count, kPageSize);
  return new MemMap(name, addr, byte_count, addr, page_aligned_byte_count, 0, true /* reuse */);
}

MemMap* MemMap::MapFileAtAddress(uint8_t* expected_ptr,
                                 size_t byte_count,
                                 int prot,
                                 int flags,
                                 int fd,
                                 off_t start,
                                 bool low_4gb,
                                 bool reuse,
                                 const char* filename,
                                 std::string* error_msg) {
  CHECK_NE(0, prot);
  CHECK_NE(0, flags & (MAP_SHARED | MAP_PRIVATE));

  // Note that we do not allow MAP_FIXED unless reuse == true, i.e we
  // expect his mapping to be contained within an existing map.
  if (reuse) {
    // reuse means it is okay that it overlaps an existing page mapping.
    // Only use this if you actually made the page reservation yourself.
    CHECK(expected_ptr != nullptr);

    DCHECK(ContainedWithinExistingMap(expected_ptr, byte_count, error_msg))
        << ((error_msg != nullptr) ? *error_msg : std::string());
    flags |= MAP_FIXED;
  } else {
    CHECK_EQ(0, flags & MAP_FIXED);
    // Don't bother checking for an overlapping region here. We'll
    // check this if required after the fact inside CheckMapRequest.
  }

  if (byte_count == 0) {
    return new MemMap(filename, nullptr, 0, nullptr, 0, prot, false);
  }
  // Adjust 'offset' to be page-aligned as required by mmap.
  int page_offset = start % kPageSize;
  off_t page_aligned_offset = start - page_offset;
  // Adjust 'byte_count' to be page-aligned as we will map this anyway.
  size_t page_aligned_byte_count = RoundUp(byte_count + page_offset, kPageSize);
  // The 'expected_ptr' is modified (if specified, ie non-null) to be page aligned to the file but
  // not necessarily to virtual memory. mmap will page align 'expected' for us.
  uint8_t* page_aligned_expected =
      (expected_ptr == nullptr) ? nullptr : (expected_ptr - page_offset);

  size_t redzone_size = 0;
  if (RUNNING_ON_MEMORY_TOOL && kMemoryToolAddsRedzones && expected_ptr == nullptr) {
    redzone_size = kPageSize;
    page_aligned_byte_count += redzone_size;
  }

  uint8_t* actual = reinterpret_cast<uint8_t*>(MapInternal(page_aligned_expected,
                                                           page_aligned_byte_count,
                                                           prot,
                                                           flags,
                                                           fd,
                                                           page_aligned_offset,
                                                           low_4gb));
  if (actual == MAP_FAILED) {
    if (error_msg != nullptr) {
      auto saved_errno = errno;

      if (kIsDebugBuild || VLOG_IS_ON(oat)) {
        PrintFileToLog("/proc/self/maps", LogSeverity::WARNING);
      }

      *error_msg = StringPrintf("mmap(%p, %zd, 0x%x, 0x%x, %d, %" PRId64
                                ") of file '%s' failed: %s. See process maps in the log.",
                                page_aligned_expected, page_aligned_byte_count, prot, flags, fd,
                                static_cast<int64_t>(page_aligned_offset), filename,
                                strerror(saved_errno));
    }
    return nullptr;
  }
  std::ostringstream check_map_request_error_msg;
  if (!CheckMapRequest(expected_ptr, actual, page_aligned_byte_count, error_msg)) {
    return nullptr;
  }
  if (redzone_size != 0) {
    const uint8_t *real_start = actual + page_offset;
    const uint8_t *real_end = actual + page_offset + byte_count;
    const uint8_t *mapping_end = actual + page_aligned_byte_count;

    MEMORY_TOOL_MAKE_NOACCESS(actual, real_start - actual);
    MEMORY_TOOL_MAKE_NOACCESS(real_end, mapping_end - real_end);
    page_aligned_byte_count -= redzone_size;
  }

  return new MemMap(filename, actual + page_offset, byte_count, actual, page_aligned_byte_count,
                    prot, reuse, redzone_size);
}

MemMap::~MemMap() {
  if (base_begin_ == nullptr && base_size_ == 0) {
    return;
  }

  // Unlike Valgrind, AddressSanitizer requires that all manually poisoned memory is unpoisoned
  // before it is returned to the system.
  if (redzone_size_ != 0) {
    MEMORY_TOOL_MAKE_UNDEFINED(
        reinterpret_cast<char*>(base_begin_) + base_size_ - redzone_size_,
        redzone_size_);
  }

  if (!reuse_) {
    MEMORY_TOOL_MAKE_UNDEFINED(base_begin_, base_size_);
    int result = munmap(base_begin_, base_size_);
    if (result == -1) {
      PLOG(FATAL) << "munmap failed";
    }
  }

  // Remove it from maps_.
  MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
  bool found = false;
  DCHECK(maps_ != nullptr);
  for (auto it = maps_->lower_bound(base_begin_), end = maps_->end();
       it != end && it->first == base_begin_; ++it) {
    if (it->second == this) {
      found = true;
      maps_->erase(it);
      break;
    }
  }
  CHECK(found) << "MemMap not found";
}

MemMap::MemMap(const std::string& name, uint8_t* begin, size_t size, void* base_begin,
               size_t base_size, int prot, bool reuse, size_t redzone_size)
    : name_(name), begin_(begin), size_(size), base_begin_(base_begin), base_size_(base_size),
      prot_(prot), reuse_(reuse), redzone_size_(redzone_size) {
  if (size_ == 0) {
    CHECK(begin_ == nullptr);
    CHECK(base_begin_ == nullptr);
    CHECK_EQ(base_size_, 0U);
  } else {
    CHECK(begin_ != nullptr);
    CHECK(base_begin_ != nullptr);
    CHECK_NE(base_size_, 0U);

    // Add it to maps_.
    MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
    DCHECK(maps_ != nullptr);
    maps_->insert(std::make_pair(base_begin_, this));
  }
}

MemMap* MemMap::RemapAtEnd(uint8_t* new_end, const char* tail_name, int tail_prot,
                           std::string* error_msg, bool use_ashmem) {
  DCHECK_GE(new_end, Begin());
  DCHECK_LE(new_end, End());
  DCHECK_LE(begin_ + size_, reinterpret_cast<uint8_t*>(base_begin_) + base_size_);
  DCHECK_ALIGNED(begin_, kPageSize);
  DCHECK_ALIGNED(base_begin_, kPageSize);
  DCHECK_ALIGNED(reinterpret_cast<uint8_t*>(base_begin_) + base_size_, kPageSize);
  DCHECK_ALIGNED(new_end, kPageSize);
  uint8_t* old_end = begin_ + size_;
  uint8_t* old_base_end = reinterpret_cast<uint8_t*>(base_begin_) + base_size_;
  uint8_t* new_base_end = new_end;
  DCHECK_LE(new_base_end, old_base_end);
  if (new_base_end == old_base_end) {
    return new MemMap(tail_name, nullptr, 0, nullptr, 0, tail_prot, false);
  }
  size_ = new_end - reinterpret_cast<uint8_t*>(begin_);
  base_size_ = new_base_end - reinterpret_cast<uint8_t*>(base_begin_);
  DCHECK_LE(begin_ + size_, reinterpret_cast<uint8_t*>(base_begin_) + base_size_);
  size_t tail_size = old_end - new_end;
  uint8_t* tail_base_begin = new_base_end;
  size_t tail_base_size = old_base_end - new_base_end;
  DCHECK_EQ(tail_base_begin + tail_base_size, old_base_end);
  DCHECK_ALIGNED(tail_base_size, kPageSize);

  int int_fd = -1;
  int flags = MAP_PRIVATE | MAP_ANONYMOUS;
  if (use_ashmem) {
    // android_os_Debug.cpp read_mapinfo assumes all ashmem regions associated with the VM are
    // prefixed "dalvik-".
    std::string debug_friendly_name("dalvik-");
    debug_friendly_name += tail_name;
    int_fd = ashmem_create_region(debug_friendly_name.c_str(), tail_base_size);
    flags = MAP_PRIVATE | MAP_FIXED;
    if (int_fd == -1) {
      *error_msg = StringPrintf("ashmem_create_region failed for '%s': %s",
                                tail_name, strerror(errno));
      return nullptr;
    }
  }
  ScopedFd fd(int_fd);

  MEMORY_TOOL_MAKE_UNDEFINED(tail_base_begin, tail_base_size);
  // Unmap/map the tail region.
  int result = munmap(tail_base_begin, tail_base_size);
  if (result == -1) {
    PrintFileToLog("/proc/self/maps", LogSeverity::WARNING);
    *error_msg = StringPrintf("munmap(%p, %zd) failed for '%s'. See process maps in the log.",
                              tail_base_begin, tail_base_size, name_.c_str());
    return nullptr;
  }
  // Don't cause memory allocation between the munmap and the mmap
  // calls. Otherwise, libc (or something else) might take this memory
  // region. Note this isn't perfect as there's no way to prevent
  // other threads to try to take this memory region here.
  uint8_t* actual = reinterpret_cast<uint8_t*>(mmap(tail_base_begin, tail_base_size, tail_prot,
                                              flags, fd.get(), 0));
  if (actual == MAP_FAILED) {
    PrintFileToLog("/proc/self/maps", LogSeverity::WARNING);
    *error_msg = StringPrintf("anonymous mmap(%p, %zd, 0x%x, 0x%x, %d, 0) failed. See process "
                              "maps in the log.", tail_base_begin, tail_base_size, tail_prot, flags,
                              fd.get());
    return nullptr;
  }
  return new MemMap(tail_name, actual, tail_size, actual, tail_base_size, tail_prot, false);
}

void MemMap::MadviseDontNeedAndZero() {
  if (base_begin_ != nullptr || base_size_ != 0) {
    if (!kMadviseZeroes) {
      memset(base_begin_, 0, base_size_);
    }
    int result = madvise(base_begin_, base_size_, MADV_DONTNEED);
    if (result == -1) {
      PLOG(WARNING) << "madvise failed";
    }
  }
}

bool MemMap::Sync() {
  bool result;
  if (redzone_size_ != 0) {
    // To avoid valgrind errors, temporarily lift the lower-end noaccess protection before passing
    // it to msync() as it only accepts page-aligned base address, and exclude the higher-end
    // noaccess protection from the msync range. b/27552451.
    uint8_t* base_begin = reinterpret_cast<uint8_t*>(base_begin_);
    MEMORY_TOOL_MAKE_DEFINED(base_begin, begin_ - base_begin);
    result = msync(BaseBegin(), End() - base_begin, MS_SYNC) == 0;
    MEMORY_TOOL_MAKE_NOACCESS(base_begin, begin_ - base_begin);
  } else {
    result = msync(BaseBegin(), BaseSize(), MS_SYNC) == 0;
  }
  return result;
}

bool MemMap::Protect(int prot) {
  if (base_begin_ == nullptr && base_size_ == 0) {
    prot_ = prot;
    return true;
  }

  if (mprotect(base_begin_, base_size_, prot) == 0) {
    prot_ = prot;
    return true;
  }

  PLOG(ERROR) << "mprotect(" << reinterpret_cast<void*>(base_begin_) << ", " << base_size_ << ", "
              << prot << ") failed";
  return false;
}

bool MemMap::CheckNoGaps(MemMap* begin_map, MemMap* end_map) {
  MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
  CHECK(begin_map != nullptr);
  CHECK(end_map != nullptr);
  CHECK(HasMemMap(begin_map));
  CHECK(HasMemMap(end_map));
  CHECK_LE(begin_map->BaseBegin(), end_map->BaseBegin());
  MemMap* map = begin_map;
  while (map->BaseBegin() != end_map->BaseBegin()) {
    MemMap* next_map = GetLargestMemMapAt(map->BaseEnd());
    if (next_map == nullptr) {
      // Found a gap.
      return false;
    }
    map = next_map;
  }
  return true;
}

void MemMap::DumpMaps(std::ostream& os, bool terse) {
  MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
  DumpMapsLocked(os, terse);
}

void MemMap::DumpMapsLocked(std::ostream& os, bool terse) {
  const auto& mem_maps = *maps_;
  if (!terse) {
    os << mem_maps;
    return;
  }

  // Terse output example:
  //   [MemMap: 0x409be000+0x20P~0x11dP+0x20P~0x61cP+0x20P prot=0x3 LinearAlloc]
  //   [MemMap: 0x451d6000+0x6bP(3) prot=0x3 large object space allocation]
  // The details:
  //   "+0x20P" means 0x20 pages taken by a single mapping,
  //   "~0x11dP" means a gap of 0x11d pages,
  //   "+0x6bP(3)" means 3 mappings one after another, together taking 0x6b pages.
  os << "MemMap:" << std::endl;
  for (auto it = mem_maps.begin(), maps_end = mem_maps.end(); it != maps_end;) {
    MemMap* map = it->second;
    void* base = it->first;
    CHECK_EQ(base, map->BaseBegin());
    os << "[MemMap: " << base;
    ++it;
    // Merge consecutive maps with the same protect flags and name.
    constexpr size_t kMaxGaps = 9;
    size_t num_gaps = 0;
    size_t num = 1u;
    size_t size = map->BaseSize();
    CHECK_ALIGNED(size, kPageSize);
    void* end = map->BaseEnd();
    while (it != maps_end &&
        it->second->GetProtect() == map->GetProtect() &&
        it->second->GetName() == map->GetName() &&
        (it->second->BaseBegin() == end || num_gaps < kMaxGaps)) {
      if (it->second->BaseBegin() != end) {
        ++num_gaps;
        os << "+0x" << std::hex << (size / kPageSize) << "P";
        if (num != 1u) {
          os << "(" << std::dec << num << ")";
        }
        size_t gap =
            reinterpret_cast<uintptr_t>(it->second->BaseBegin()) - reinterpret_cast<uintptr_t>(end);
        CHECK_ALIGNED(gap, kPageSize);
        os << "~0x" << std::hex << (gap / kPageSize) << "P";
        num = 0u;
        size = 0u;
      }
      CHECK_ALIGNED(it->second->BaseSize(), kPageSize);
      ++num;
      size += it->second->BaseSize();
      end = it->second->BaseEnd();
      ++it;
    }
    os << "+0x" << std::hex << (size / kPageSize) << "P";
    if (num != 1u) {
      os << "(" << std::dec << num << ")";
    }
    os << " prot=0x" << std::hex << map->GetProtect() << " " << map->GetName() << "]" << std::endl;
  }
}

bool MemMap::HasMemMap(MemMap* map) {
  void* base_begin = map->BaseBegin();
  for (auto it = maps_->lower_bound(base_begin), end = maps_->end();
       it != end && it->first == base_begin; ++it) {
    if (it->second == map) {
      return true;
    }
  }
  return false;
}

MemMap* MemMap::GetLargestMemMapAt(void* address) {
  size_t largest_size = 0;
  MemMap* largest_map = nullptr;
  DCHECK(maps_ != nullptr);
  for (auto it = maps_->lower_bound(address), end = maps_->end();
       it != end && it->first == address; ++it) {
    MemMap* map = it->second;
    CHECK(map != nullptr);
    if (largest_size < map->BaseSize()) {
      largest_size = map->BaseSize();
      largest_map = map;
    }
  }
  return largest_map;
}

void MemMap::Init() {
  MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
  if (maps_ == nullptr) {
    // dex2oat calls MemMap::Init twice since its needed before the runtime is created.
    maps_ = new Maps;
  }
}

void MemMap::Shutdown() {
  MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
  delete maps_;
  maps_ = nullptr;
}

void MemMap::SetSize(size_t new_size) {
  if (new_size == base_size_) {
    return;
  }
  CHECK_ALIGNED(new_size, kPageSize);
  CHECK_EQ(base_size_, size_) << "Unsupported";
  CHECK_LE(new_size, base_size_);
  MEMORY_TOOL_MAKE_UNDEFINED(
      reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(BaseBegin()) +
                              new_size),
      base_size_ - new_size);
  CHECK_EQ(munmap(reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(BaseBegin()) + new_size),
                  base_size_ - new_size), 0) << new_size << " " << base_size_;
  base_size_ = new_size;
  size_ = new_size;
}

void* MemMap::MapInternal(void* addr,
                          size_t length,
                          int prot,
                          int flags,
                          int fd,
                          off_t offset,
                          bool low_4gb) {
#ifdef __LP64__
  // When requesting low_4g memory and having an expectation, the requested range should fit into
  // 4GB.
  if (low_4gb && (
      // Start out of bounds.
      (reinterpret_cast<uintptr_t>(addr) >> 32) != 0 ||
      // End out of bounds. For simplicity, this will fail for the last page of memory.
      ((reinterpret_cast<uintptr_t>(addr) + length) >> 32) != 0)) {
    LOG(ERROR) << "The requested address space (" << addr << ", "
               << reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(addr) + length)
               << ") cannot fit in low_4gb";
    return MAP_FAILED;
  }
#else
  UNUSED(low_4gb);
#endif
  DCHECK_ALIGNED(length, kPageSize);
  if (low_4gb) {
    DCHECK_EQ(flags & MAP_FIXED, 0);
  }
  // TODO:
  // A page allocator would be a useful abstraction here, as
  // 1) It is doubtful that MAP_32BIT on x86_64 is doing the right job for us
  void* actual = MAP_FAILED;
#if USE_ART_LOW_4G_ALLOCATOR
  // MAP_32BIT only available on x86_64.
  if (low_4gb && addr == nullptr) {
    bool first_run = true;

    MutexLock mu(Thread::Current(), *Locks::mem_maps_lock_);
    for (uintptr_t ptr = next_mem_pos_; ptr < 4 * GB; ptr += kPageSize) {
      // Use maps_ as an optimization to skip over large maps.
      // Find the first map which is address > ptr.
      auto it = maps_->upper_bound(reinterpret_cast<void*>(ptr));
      if (it != maps_->begin()) {
        auto before_it = it;
        --before_it;
        // Start at the end of the map before the upper bound.
        ptr = std::max(ptr, reinterpret_cast<uintptr_t>(before_it->second->BaseEnd()));
        CHECK_ALIGNED(ptr, kPageSize);
      }
      while (it != maps_->end()) {
        // How much space do we have until the next map?
        size_t delta = reinterpret_cast<uintptr_t>(it->first) - ptr;
        // If the space may be sufficient, break out of the loop.
        if (delta >= length) {
          break;
        }
        // Otherwise, skip to the end of the map.
        ptr = reinterpret_cast<uintptr_t>(it->second->BaseEnd());
        CHECK_ALIGNED(ptr, kPageSize);
        ++it;
      }

      // Try to see if we get lucky with this address since none of the ART maps overlap.
      actual = TryMemMapLow4GB(reinterpret_cast<void*>(ptr), length, prot, flags, fd, offset);
      if (actual != MAP_FAILED) {
        next_mem_pos_ = reinterpret_cast<uintptr_t>(actual) + length;
        return actual;
      }

      if (4U * GB - ptr < length) {
        // Not enough memory until 4GB.
        if (first_run) {
          // Try another time from the bottom;
          ptr = LOW_MEM_START - kPageSize;
          first_run = false;
          continue;
        } else {
          // Second try failed.
          break;
        }
      }

      uintptr_t tail_ptr;

      // Check pages are free.
      bool safe = true;
      for (tail_ptr = ptr; tail_ptr < ptr + length; tail_ptr += kPageSize) {
        if (msync(reinterpret_cast<void*>(tail_ptr), kPageSize, 0) == 0) {
          safe = false;
          break;
        } else {
          DCHECK_EQ(errno, ENOMEM);
        }
      }

      next_mem_pos_ = tail_ptr;  // update early, as we break out when we found and mapped a region

      if (safe == true) {
        actual = TryMemMapLow4GB(reinterpret_cast<void*>(ptr), length, prot, flags, fd, offset);
        if (actual != MAP_FAILED) {
          return actual;
        }
      } else {
        // Skip over last page.
        ptr = tail_ptr;
      }
    }

    if (actual == MAP_FAILED) {
      LOG(ERROR) << "Could not find contiguous low-memory space.";
      errno = ENOMEM;
    }
  } else {
    actual = mmap(addr, length, prot, flags, fd, offset);
  }

#else
#if defined(__LP64__)
  if (low_4gb && addr == nullptr) {
    flags |= MAP_32BIT;
  }
#endif
  actual = mmap(addr, length, prot, flags, fd, offset);
#endif
  return actual;
}

std::ostream& operator<<(std::ostream& os, const MemMap& mem_map) {
  os << StringPrintf("[MemMap: %p-%p prot=0x%x %s]",
                     mem_map.BaseBegin(), mem_map.BaseEnd(), mem_map.GetProtect(),
                     mem_map.GetName().c_str());
  return os;
}

void MemMap::TryReadable() {
  if (base_begin_ == nullptr && base_size_ == 0) {
    return;
  }
  CHECK_NE(prot_ & PROT_READ, 0);
  volatile uint8_t* begin = reinterpret_cast<volatile uint8_t*>(base_begin_);
  volatile uint8_t* end = begin + base_size_;
  DCHECK(IsAligned<kPageSize>(begin));
  DCHECK(IsAligned<kPageSize>(end));
  // Read the first byte of each page. Use volatile to prevent the compiler from optimizing away the
  // reads.
  for (volatile uint8_t* ptr = begin; ptr < end; ptr += kPageSize) {
    // This read could fault if protection wasn't set correctly.
    uint8_t value = *ptr;
    UNUSED(value);
  }
}

}  // namespace art
