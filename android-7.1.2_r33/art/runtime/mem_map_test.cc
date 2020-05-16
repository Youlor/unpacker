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

#include "mem_map.h"

#include <memory>

#include "common_runtime_test.h"
#include "base/memory_tool.h"
#include "base/unix_file/fd_file.h"

namespace art {

class MemMapTest : public CommonRuntimeTest {
 public:
  static uint8_t* BaseBegin(MemMap* mem_map) {
    return reinterpret_cast<uint8_t*>(mem_map->base_begin_);
  }

  static size_t BaseSize(MemMap* mem_map) {
    return mem_map->base_size_;
  }

  static uint8_t* GetValidMapAddress(size_t size, bool low_4gb) {
    // Find a valid map address and unmap it before returning.
    std::string error_msg;
    std::unique_ptr<MemMap> map(MemMap::MapAnonymous("temp",
                                                     nullptr,
                                                     size,
                                                     PROT_READ,
                                                     low_4gb,
                                                     false,
                                                     &error_msg));
    CHECK(map != nullptr);
    return map->Begin();
  }

  static void RemapAtEndTest(bool low_4gb) {
    std::string error_msg;
    // Cast the page size to size_t.
    const size_t page_size = static_cast<size_t>(kPageSize);
    // Map a two-page memory region.
    MemMap* m0 = MemMap::MapAnonymous("MemMapTest_RemapAtEndTest_map0",
                                      nullptr,
                                      2 * page_size,
                                      PROT_READ | PROT_WRITE,
                                      low_4gb,
                                      false,
                                      &error_msg);
    // Check its state and write to it.
    uint8_t* base0 = m0->Begin();
    ASSERT_TRUE(base0 != nullptr) << error_msg;
    size_t size0 = m0->Size();
    EXPECT_EQ(m0->Size(), 2 * page_size);
    EXPECT_EQ(BaseBegin(m0), base0);
    EXPECT_EQ(BaseSize(m0), size0);
    memset(base0, 42, 2 * page_size);
    // Remap the latter half into a second MemMap.
    MemMap* m1 = m0->RemapAtEnd(base0 + page_size,
                                "MemMapTest_RemapAtEndTest_map1",
                                PROT_READ | PROT_WRITE,
                                &error_msg);
    // Check the states of the two maps.
    EXPECT_EQ(m0->Begin(), base0) << error_msg;
    EXPECT_EQ(m0->Size(), page_size);
    EXPECT_EQ(BaseBegin(m0), base0);
    EXPECT_EQ(BaseSize(m0), page_size);
    uint8_t* base1 = m1->Begin();
    size_t size1 = m1->Size();
    EXPECT_EQ(base1, base0 + page_size);
    EXPECT_EQ(size1, page_size);
    EXPECT_EQ(BaseBegin(m1), base1);
    EXPECT_EQ(BaseSize(m1), size1);
    // Write to the second region.
    memset(base1, 43, page_size);
    // Check the contents of the two regions.
    for (size_t i = 0; i < page_size; ++i) {
      EXPECT_EQ(base0[i], 42);
    }
    for (size_t i = 0; i < page_size; ++i) {
      EXPECT_EQ(base1[i], 43);
    }
    // Unmap the first region.
    delete m0;
    // Make sure the second region is still accessible after the first
    // region is unmapped.
    for (size_t i = 0; i < page_size; ++i) {
      EXPECT_EQ(base1[i], 43);
    }
    delete m1;
  }

  void CommonInit() {
    MemMap::Init();
  }

#if defined(__LP64__) && !defined(__x86_64__)
  static uintptr_t GetLinearScanPos() {
    return MemMap::next_mem_pos_;
  }
#endif
};

#if defined(__LP64__) && !defined(__x86_64__)

#ifdef __BIONIC__
extern uintptr_t CreateStartPos(uint64_t input);
#endif

TEST_F(MemMapTest, Start) {
  CommonInit();
  uintptr_t start = GetLinearScanPos();
  EXPECT_LE(64 * KB, start);
  EXPECT_LT(start, static_cast<uintptr_t>(ART_BASE_ADDRESS));
#ifdef __BIONIC__
  // Test a couple of values. Make sure they are different.
  uintptr_t last = 0;
  for (size_t i = 0; i < 100; ++i) {
    uintptr_t random_start = CreateStartPos(i * kPageSize);
    EXPECT_NE(last, random_start);
    last = random_start;
  }

  // Even on max, should be below ART_BASE_ADDRESS.
  EXPECT_LT(CreateStartPos(~0), static_cast<uintptr_t>(ART_BASE_ADDRESS));
#endif
  // End of test.
}
#endif

TEST_F(MemMapTest, MapAnonymousEmpty) {
  CommonInit();
  std::string error_msg;
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous("MapAnonymousEmpty",
                                                   nullptr,
                                                   0,
                                                   PROT_READ,
                                                   false,
                                                   false,
                                                   &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  map.reset(MemMap::MapAnonymous("MapAnonymousEmpty",
                                 nullptr,
                                 kPageSize,
                                 PROT_READ | PROT_WRITE,
                                 false,
                                 false,
                                 &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
}

TEST_F(MemMapTest, MapAnonymousFailNullError) {
  CommonInit();
  // Test that we don't crash with a null error_str when mapping at an invalid location.
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous("MapAnonymousInvalid",
                                                    reinterpret_cast<uint8_t*>(kPageSize),
                                                    0x20000,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    false,
                                                    nullptr));
  ASSERT_EQ(nullptr, map.get());
}

#ifdef __LP64__
TEST_F(MemMapTest, MapAnonymousEmpty32bit) {
  CommonInit();
  std::string error_msg;
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous("MapAnonymousEmpty",
                                                   nullptr,
                                                   kPageSize,
                                                   PROT_READ | PROT_WRITE,
                                                   true,
                                                   false,
                                                   &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_LT(reinterpret_cast<uintptr_t>(BaseBegin(map.get())), 1ULL << 32);
}
TEST_F(MemMapTest, MapFile32Bit) {
  CommonInit();
  std::string error_msg;
  ScratchFile scratch_file;
  constexpr size_t kMapSize = kPageSize;
  std::unique_ptr<uint8_t[]> data(new uint8_t[kMapSize]());
  ASSERT_TRUE(scratch_file.GetFile()->WriteFully(&data[0], kMapSize));
  std::unique_ptr<MemMap> map(MemMap::MapFile(/*byte_count*/kMapSize,
                                              PROT_READ,
                                              MAP_PRIVATE,
                                              scratch_file.GetFd(),
                                              /*start*/0,
                                              /*low_4gb*/true,
                                              scratch_file.GetFilename().c_str(),
                                              &error_msg));
  ASSERT_TRUE(map != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_EQ(map->Size(), kMapSize);
  ASSERT_LT(reinterpret_cast<uintptr_t>(BaseBegin(map.get())), 1ULL << 32);
}
#endif

TEST_F(MemMapTest, MapAnonymousExactAddr) {
  CommonInit();
  std::string error_msg;
  // Find a valid address.
  uint8_t* valid_address = GetValidMapAddress(kPageSize, /*low_4gb*/false);
  // Map at an address that should work, which should succeed.
  std::unique_ptr<MemMap> map0(MemMap::MapAnonymous("MapAnonymous0",
                                                    valid_address,
                                                    kPageSize,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    false,
                                                    &error_msg));
  ASSERT_TRUE(map0.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_TRUE(map0->BaseBegin() == valid_address);
  // Map at an unspecified address, which should succeed.
  std::unique_ptr<MemMap> map1(MemMap::MapAnonymous("MapAnonymous1",
                                                    nullptr,
                                                    kPageSize,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    false,
                                                    &error_msg));
  ASSERT_TRUE(map1.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  ASSERT_TRUE(map1->BaseBegin() != nullptr);
  // Attempt to map at the same address, which should fail.
  std::unique_ptr<MemMap> map2(MemMap::MapAnonymous("MapAnonymous2",
                                                    reinterpret_cast<uint8_t*>(map1->BaseBegin()),
                                                    kPageSize,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    false,
                                                    &error_msg));
  ASSERT_TRUE(map2.get() == nullptr) << error_msg;
  ASSERT_TRUE(!error_msg.empty());
}

TEST_F(MemMapTest, RemapAtEnd) {
  RemapAtEndTest(false);
}

#ifdef __LP64__
TEST_F(MemMapTest, RemapAtEnd32bit) {
  RemapAtEndTest(true);
}
#endif

TEST_F(MemMapTest, MapAnonymousExactAddr32bitHighAddr) {
  // Some MIPS32 hardware (namely the Creator Ci20 development board)
  // cannot allocate in the 2GB-4GB region.
  TEST_DISABLED_FOR_MIPS();

  CommonInit();
  // This test may not work under valgrind.
  if (RUNNING_ON_MEMORY_TOOL == 0) {
    constexpr size_t size = 0x100000;
    // Try all addresses starting from 2GB to 4GB.
    size_t start_addr = 2 * GB;
    std::string error_msg;
    std::unique_ptr<MemMap> map;
    for (; start_addr <= std::numeric_limits<uint32_t>::max() - size; start_addr += size) {
      map.reset(MemMap::MapAnonymous("MapAnonymousExactAddr32bitHighAddr",
                                     reinterpret_cast<uint8_t*>(start_addr),
                                     size,
                                     PROT_READ | PROT_WRITE,
                                     /*low_4gb*/true,
                                     false,
                                     &error_msg));
      if (map != nullptr) {
        break;
      }
    }
    ASSERT_TRUE(map.get() != nullptr) << error_msg;
    ASSERT_GE(reinterpret_cast<uintptr_t>(map->End()), 2u * GB);
    ASSERT_TRUE(error_msg.empty());
    ASSERT_EQ(BaseBegin(map.get()), reinterpret_cast<void*>(start_addr));
  }
}

TEST_F(MemMapTest, MapAnonymousOverflow) {
  CommonInit();
  std::string error_msg;
  uintptr_t ptr = 0;
  ptr -= kPageSize;  // Now it's close to the top.
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous("MapAnonymousOverflow",
                                                   reinterpret_cast<uint8_t*>(ptr),
                                                   2 * kPageSize,  // brings it over the top.
                                                   PROT_READ | PROT_WRITE,
                                                   false,
                                                   false,
                                                   &error_msg));
  ASSERT_EQ(nullptr, map.get());
  ASSERT_FALSE(error_msg.empty());
}

#ifdef __LP64__
TEST_F(MemMapTest, MapAnonymousLow4GBExpectedTooHigh) {
  CommonInit();
  std::string error_msg;
  std::unique_ptr<MemMap> map(
      MemMap::MapAnonymous("MapAnonymousLow4GBExpectedTooHigh",
                           reinterpret_cast<uint8_t*>(UINT64_C(0x100000000)),
                           kPageSize,
                           PROT_READ | PROT_WRITE,
                           true,
                           false,
                           &error_msg));
  ASSERT_EQ(nullptr, map.get());
  ASSERT_FALSE(error_msg.empty());
}

TEST_F(MemMapTest, MapAnonymousLow4GBRangeTooHigh) {
  CommonInit();
  std::string error_msg;
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous("MapAnonymousLow4GBRangeTooHigh",
                                                   reinterpret_cast<uint8_t*>(0xF0000000),
                                                   0x20000000,
                                                   PROT_READ | PROT_WRITE,
                                                   true,
                                                   false,
                                                   &error_msg));
  ASSERT_EQ(nullptr, map.get());
  ASSERT_FALSE(error_msg.empty());
}
#endif

TEST_F(MemMapTest, MapAnonymousReuse) {
  CommonInit();
  std::string error_msg;
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous("MapAnonymousReserve",
                                                   nullptr,
                                                   0x20000,
                                                   PROT_READ | PROT_WRITE,
                                                   false,
                                                   false,
                                                   &error_msg));
  ASSERT_NE(nullptr, map.get());
  ASSERT_TRUE(error_msg.empty());
  std::unique_ptr<MemMap> map2(MemMap::MapAnonymous("MapAnonymousReused",
                                                    reinterpret_cast<uint8_t*>(map->BaseBegin()),
                                                    0x10000,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    true,
                                                    &error_msg));
  ASSERT_NE(nullptr, map2.get());
  ASSERT_TRUE(error_msg.empty());
}

TEST_F(MemMapTest, CheckNoGaps) {
  CommonInit();
  std::string error_msg;
  constexpr size_t kNumPages = 3;
  // Map a 3-page mem map.
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous("MapAnonymous0",
                                                   nullptr,
                                                   kPageSize * kNumPages,
                                                   PROT_READ | PROT_WRITE,
                                                   false,
                                                   false,
                                                   &error_msg));
  ASSERT_TRUE(map.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  // Record the base address.
  uint8_t* map_base = reinterpret_cast<uint8_t*>(map->BaseBegin());
  // Unmap it.
  map.reset();

  // Map at the same address, but in page-sized separate mem maps,
  // assuming the space at the address is still available.
  std::unique_ptr<MemMap> map0(MemMap::MapAnonymous("MapAnonymous0",
                                                    map_base,
                                                    kPageSize,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    false,
                                                    &error_msg));
  ASSERT_TRUE(map0.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  std::unique_ptr<MemMap> map1(MemMap::MapAnonymous("MapAnonymous1",
                                                    map_base + kPageSize,
                                                    kPageSize,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    false,
                                                    &error_msg));
  ASSERT_TRUE(map1.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());
  std::unique_ptr<MemMap> map2(MemMap::MapAnonymous("MapAnonymous2",
                                                    map_base + kPageSize * 2,
                                                    kPageSize,
                                                    PROT_READ | PROT_WRITE,
                                                    false,
                                                    false,
                                                    &error_msg));
  ASSERT_TRUE(map2.get() != nullptr) << error_msg;
  ASSERT_TRUE(error_msg.empty());

  // One-map cases.
  ASSERT_TRUE(MemMap::CheckNoGaps(map0.get(), map0.get()));
  ASSERT_TRUE(MemMap::CheckNoGaps(map1.get(), map1.get()));
  ASSERT_TRUE(MemMap::CheckNoGaps(map2.get(), map2.get()));

  // Two or three-map cases.
  ASSERT_TRUE(MemMap::CheckNoGaps(map0.get(), map1.get()));
  ASSERT_TRUE(MemMap::CheckNoGaps(map1.get(), map2.get()));
  ASSERT_TRUE(MemMap::CheckNoGaps(map0.get(), map2.get()));

  // Unmap the middle one.
  map1.reset();

  // Should return false now that there's a gap in the middle.
  ASSERT_FALSE(MemMap::CheckNoGaps(map0.get(), map2.get()));
}

}  // namespace art
