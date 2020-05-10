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

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>
#include <sys/param.h>

#include <backtrace/BacktraceMap.h>
#include <gtest/gtest.h>

#include "art_field-inl.h"
#include "class_linker-inl.h"
#include "common_runtime_test.h"
#include "compiler_callbacks.h"
#include "dex2oat_environment_test.h"
#include "gc/space/image_space.h"
#include "mem_map.h"
#include "oat_file_assistant.h"
#include "oat_file_manager.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "utils.h"

namespace art {

class OatFileAssistantTest : public Dex2oatEnvironmentTest {
 public:
  virtual void SetUp() OVERRIDE {
    ReserveImageSpace();
    Dex2oatEnvironmentTest::SetUp();
  }

  // Pre-Relocate the image to a known non-zero offset so we don't have to
  // deal with the runtime randomly relocating the image by 0 and messing up
  // the expected results of the tests.
  bool PreRelocateImage(std::string* error_msg) {
    std::string image;
    if (!GetCachedImageFile(&image, error_msg)) {
      return false;
    }

    std::string patchoat = GetAndroidRoot();
    patchoat += kIsDebugBuild ? "/bin/patchoatd" : "/bin/patchoat";

    std::vector<std::string> argv;
    argv.push_back(patchoat);
    argv.push_back("--input-image-location=" + GetImageLocation());
    argv.push_back("--output-image-file=" + image);
    argv.push_back("--instruction-set=" + std::string(GetInstructionSetString(kRuntimeISA)));
    argv.push_back("--base-offset-delta=0x00008000");
    return Exec(argv, error_msg);
  }

  virtual void PreRuntimeCreate() {
    std::string error_msg;
    ASSERT_TRUE(PreRelocateImage(&error_msg)) << error_msg;

    UnreserveImageSpace();
  }

  virtual void PostRuntimeCreate() OVERRIDE {
    ReserveImageSpace();
  }

  // Generate a non-PIC odex file for the purposes of test.
  // The generated odex file will be un-relocated.
  void GenerateOdexForTest(const std::string& dex_location,
                           const std::string& odex_location,
                           CompilerFilter::Filter filter,
                           bool pic = false,
                           bool with_patch_info = true) {
    // Temporarily redirect the dalvik cache so dex2oat doesn't find the
    // relocated image file.
    std::string dalvik_cache = GetDalvikCache(GetInstructionSetString(kRuntimeISA));
    std::string dalvik_cache_tmp = dalvik_cache + ".redirected";
    ASSERT_EQ(0, rename(dalvik_cache.c_str(), dalvik_cache_tmp.c_str())) << strerror(errno);

    std::vector<std::string> args;
    args.push_back("--dex-file=" + dex_location);
    args.push_back("--oat-file=" + odex_location);
    args.push_back("--compiler-filter=" + CompilerFilter::NameOfFilter(filter));
    args.push_back("--runtime-arg");
    args.push_back("-Xnorelocate");

    if (pic) {
      args.push_back("--compile-pic");
    }

    if (with_patch_info) {
      args.push_back("--include-patch-information");
    }

    std::string error_msg;
    ASSERT_TRUE(OatFileAssistant::Dex2Oat(args, &error_msg)) << error_msg;
    ASSERT_EQ(0, rename(dalvik_cache_tmp.c_str(), dalvik_cache.c_str())) << strerror(errno);

    // Verify the odex file was generated as expected and really is
    // unrelocated.
    std::unique_ptr<OatFile> odex_file(OatFile::Open(odex_location.c_str(),
                                                     odex_location.c_str(),
                                                     nullptr,
                                                     nullptr,
                                                     false,
                                                     /*low_4gb*/false,
                                                     dex_location.c_str(),
                                                     &error_msg));
    ASSERT_TRUE(odex_file.get() != nullptr) << error_msg;
    EXPECT_EQ(pic, odex_file->IsPic());
    EXPECT_EQ(with_patch_info, odex_file->HasPatchInfo());
    EXPECT_EQ(filter, odex_file->GetCompilerFilter());

    if (CompilerFilter::IsBytecodeCompilationEnabled(filter)) {
      const std::vector<gc::space::ImageSpace*> image_spaces =
        Runtime::Current()->GetHeap()->GetBootImageSpaces();
      ASSERT_TRUE(!image_spaces.empty() && image_spaces[0] != nullptr);
      const ImageHeader& image_header = image_spaces[0]->GetImageHeader();
      const OatHeader& oat_header = odex_file->GetOatHeader();
      uint32_t combined_checksum = OatFileAssistant::CalculateCombinedImageChecksum();
      EXPECT_EQ(combined_checksum, oat_header.GetImageFileLocationOatChecksum());
      EXPECT_NE(reinterpret_cast<uintptr_t>(image_header.GetOatDataBegin()),
          oat_header.GetImageFileLocationOatDataBegin());
      EXPECT_NE(image_header.GetPatchDelta(), oat_header.GetImagePatchDelta());
    }
  }

  void GeneratePicOdexForTest(const std::string& dex_location,
                              const std::string& odex_location,
                              CompilerFilter::Filter filter) {
    GenerateOdexForTest(dex_location, odex_location, filter, true, false);
  }

  // Generate a non-PIC odex file without patch information for the purposes
  // of test.  The generated odex file will be un-relocated.
  void GenerateNoPatchOdexForTest(const std::string& dex_location,
                                  const std::string& odex_location,
                                  CompilerFilter::Filter filter) {
    GenerateOdexForTest(dex_location, odex_location, filter, false, false);
  }

 private:
  // Reserve memory around where the image will be loaded so other memory
  // won't conflict when it comes time to load the image.
  // This can be called with an already loaded image to reserve the space
  // around it.
  void ReserveImageSpace() {
    MemMap::Init();

    // Ensure a chunk of memory is reserved for the image space.
    // The reservation_end includes room for the main space that has to come
    // right after the image in case of the GSS collector.
    uintptr_t reservation_start = ART_BASE_ADDRESS;
    uintptr_t reservation_end = ART_BASE_ADDRESS + 384 * MB;

    std::unique_ptr<BacktraceMap> map(BacktraceMap::Create(getpid(), true));
    ASSERT_TRUE(map.get() != nullptr) << "Failed to build process map";
    for (BacktraceMap::const_iterator it = map->begin();
        reservation_start < reservation_end && it != map->end(); ++it) {
      ReserveImageSpaceChunk(reservation_start, std::min(it->start, reservation_end));
      reservation_start = std::max(reservation_start, it->end);
    }
    ReserveImageSpaceChunk(reservation_start, reservation_end);
  }

  // Reserve a chunk of memory for the image space in the given range.
  // Only has effect for chunks with a positive number of bytes.
  void ReserveImageSpaceChunk(uintptr_t start, uintptr_t end) {
    if (start < end) {
      std::string error_msg;
      image_reservation_.push_back(std::unique_ptr<MemMap>(
          MemMap::MapAnonymous("image reservation",
              reinterpret_cast<uint8_t*>(start), end - start,
              PROT_NONE, false, false, &error_msg)));
      ASSERT_TRUE(image_reservation_.back().get() != nullptr) << error_msg;
      LOG(INFO) << "Reserved space for image " <<
        reinterpret_cast<void*>(image_reservation_.back()->Begin()) << "-" <<
        reinterpret_cast<void*>(image_reservation_.back()->End());
    }
  }


  // Unreserve any memory reserved by ReserveImageSpace. This should be called
  // before the image is loaded.
  void UnreserveImageSpace() {
    image_reservation_.clear();
  }

  std::vector<std::unique_ptr<MemMap>> image_reservation_;
};

class OatFileAssistantNoDex2OatTest : public OatFileAssistantTest {
 public:
  virtual void SetUpRuntimeOptions(RuntimeOptions* options) {
    OatFileAssistantTest::SetUpRuntimeOptions(options);
    options->push_back(std::make_pair("-Xnodex2oat", nullptr));
  }
};

// Generate an oat file for the purposes of test, as opposed to testing
// generation of oat files.
static void GenerateOatForTest(const char* dex_location, CompilerFilter::Filter filter) {
  // Use an oat file assistant to find the proper oat location.
  OatFileAssistant ofa(dex_location, kRuntimeISA, false, false);
  const std::string* oat_location = ofa.OatFileName();
  ASSERT_TRUE(oat_location != nullptr);

  std::vector<std::string> args;
  args.push_back("--dex-file=" + std::string(dex_location));
  args.push_back("--oat-file=" + *oat_location);
  args.push_back("--compiler-filter=" + CompilerFilter::NameOfFilter(filter));
  args.push_back("--runtime-arg");
  args.push_back("-Xnorelocate");
  std::string error_msg;
  ASSERT_TRUE(OatFileAssistant::Dex2Oat(args, &error_msg)) << error_msg;

  // Verify the oat file was generated as expected.
  std::unique_ptr<OatFile> oat_file(OatFile::Open(oat_location->c_str(),
                                                  oat_location->c_str(),
                                                  nullptr,
                                                  nullptr,
                                                  false,
                                                  /*low_4gb*/false,
                                                  dex_location,
                                                  &error_msg));
  ASSERT_TRUE(oat_file.get() != nullptr) << error_msg;
  EXPECT_EQ(filter, oat_file->GetCompilerFilter());
}

// Case: We have a DEX file, but no OAT file for it.
// Expect: The status is kDex2OatNeeded.
TEST_F(OatFileAssistantTest, DexNoOat) {
  std::string dex_location = GetScratchDir() + "/DexNoOat.jar";
  Copy(GetDexSrc1(), dex_location);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kVerifyAtRuntime));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kInterpretOnly));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeedProfile));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kOatOutOfDate, oat_file_assistant.OdexFileStatus());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kOatOutOfDate, oat_file_assistant.OatFileStatus());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have no DEX file and no OAT file.
// Expect: Status is kNoDexOptNeeded. Loading should fail, but not crash.
TEST_F(OatFileAssistantTest, NoDexNoOat) {
  std::string dex_location = GetScratchDir() + "/NoDexNoOat.jar";

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());

  // Trying to make the oat file up to date should not fail or crash.
  std::string error_msg;
  EXPECT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg));

  // Trying to get the best oat file should fail, but not crash.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  EXPECT_EQ(nullptr, oat_file.get());
}

// Case: We have a DEX file and up-to-date OAT file for it.
// Expect: The status is kNoDexOptNeeded.
TEST_F(OatFileAssistantTest, OatUpToDate) {
  std::string dex_location = GetScratchDir() + "/OatUpToDate.jar";
  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeed);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kInterpretOnly));
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kVerifyAtRuntime));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kEverything));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kOatUpToDate, oat_file_assistant.OatFileStatus());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a DEX file and speed-profile OAT file for it.
// Expect: The status is kNoDexOptNeeded if the profile hasn't changed.
TEST_F(OatFileAssistantTest, ProfileOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/ProfileOatUpToDate.jar";
  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeedProfile);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeedProfile));
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kInterpretOnly));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kOatUpToDate, oat_file_assistant.OatFileStatus());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a DEX file and speed-profile OAT file for it.
// Expect: The status is kNoDex2OatNeeded if the profile has changed.
TEST_F(OatFileAssistantTest, ProfileOatOutOfDate) {
  std::string dex_location = GetScratchDir() + "/ProfileOatOutOfDate.jar";
  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeedProfile);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, true, false);

  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeedProfile));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kInterpretOnly));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_EQ(OatFileAssistant::kOatOutOfDate, oat_file_assistant.OatFileStatus());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a MultiDEX file and up-to-date OAT file for it.
// Expect: The status is kNoDexOptNeeded and we load all dex files.
TEST_F(OatFileAssistantTest, MultiDexOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/MultiDexOatUpToDate.jar";
  Copy(GetMultiDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeed);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());

  // Verify we can load both dex files.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(2u, dex_files.size());
}

// Case: We have a MultiDEX file where the secondary dex file is out of date.
// Expect: The status is kDex2OatNeeded.
TEST_F(OatFileAssistantTest, MultiDexSecondaryOutOfDate) {
  std::string dex_location = GetScratchDir() + "/MultiDexSecondaryOutOfDate.jar";

  // Compile code for GetMultiDexSrc1.
  Copy(GetMultiDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeed);

  // Now overwrite the dex file with GetMultiDexSrc2 so the secondary checksum
  // is out of date.
  Copy(GetMultiDexSrc2(), dex_location);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a MultiDEX file and up-to-date OAT file for it with relative
// encoded dex locations.
// Expect: The oat file status is kNoDexOptNeeded.
TEST_F(OatFileAssistantTest, RelativeEncodedDexLocation) {
  std::string dex_location = GetScratchDir() + "/RelativeEncodedDexLocation.jar";
  std::string oat_location = GetOdexDir() + "/RelativeEncodedDexLocation.oat";

  // Create the dex file
  Copy(GetMultiDexSrc1(), dex_location);

  // Create the oat file with relative encoded dex location.
  std::vector<std::string> args;
  args.push_back("--dex-file=" + dex_location);
  args.push_back("--dex-location=" + std::string("RelativeEncodedDexLocation.jar"));
  args.push_back("--oat-file=" + oat_location);
  args.push_back("--compiler-filter=speed");

  std::string error_msg;
  ASSERT_TRUE(OatFileAssistant::Dex2Oat(args, &error_msg)) << error_msg;

  // Verify we can load both dex files.
  OatFileAssistant oat_file_assistant(dex_location.c_str(),
                                      oat_location.c_str(),
                                      kRuntimeISA, false, true);
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(2u, dex_files.size());
}

// Case: We have a DEX file and out-of-date OAT file.
// Expect: The status is kDex2OatNeeded.
TEST_F(OatFileAssistantTest, OatOutOfDate) {
  std::string dex_location = GetScratchDir() + "/OatOutOfDate.jar";

  // We create a dex, generate an oat for it, then overwrite the dex with a
  // different dex to make the oat out of date.
  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeed);
  Copy(GetDexSrc2(), dex_location);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kVerifyAtRuntime));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a DEX file and an ODEX file, but no OAT file.
// Expect: The status is kPatchOatNeeded.
TEST_F(OatFileAssistantTest, DexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/DexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/DexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kVerifyAtRuntime));
  EXPECT_EQ(OatFileAssistant::kPatchOatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());

  // We should still be able to get the non-executable odex file to run from.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
}

// Case: We have a stripped DEX file and an ODEX file, but no OAT file.
// Expect: The status is kPatchOatNeeded
TEST_F(OatFileAssistantTest, StrippedDexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/StrippedDexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/StrippedDexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed);

  // Strip the dex file
  Copy(GetStrippedDexSrc1(), dex_location);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  EXPECT_EQ(OatFileAssistant::kPatchOatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());

  // Make the oat file up to date.
  std::string error_msg;
  ASSERT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg)) << error_msg;

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());

  // Verify we can load the dex files from it.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a stripped DEX file, an ODEX file, and an out-of-date OAT file.
// Expect: The status is kPatchOatNeeded.
TEST_F(OatFileAssistantTest, StrippedDexOdexOat) {
  std::string dex_location = GetScratchDir() + "/StrippedDexOdexOat.jar";
  std::string odex_location = GetOdexDir() + "/StrippedDexOdexOat.odex";

  // Create the oat file from a different dex file so it looks out of date.
  Copy(GetDexSrc2(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeed);

  // Create the odex file
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed);

  // Strip the dex file.
  Copy(GetStrippedDexSrc1(), dex_location);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kVerifyAtRuntime));
  EXPECT_EQ(OatFileAssistant::kPatchOatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,  // Can't run dex2oat because dex file is stripped.
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kEverything));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());

  // Make the oat file up to date.
  std::string error_msg;
  ASSERT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg)) << error_msg;

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,  // Can't run dex2oat because dex file is stripped.
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kEverything));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());

  // Verify we can load the dex files from it.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a stripped (or resource-only) DEX file, no ODEX file and no
// OAT file. Expect: The status is kNoDexOptNeeded.
TEST_F(OatFileAssistantTest, ResourceOnlyDex) {
  std::string dex_location = GetScratchDir() + "/ResourceOnlyDex.jar";

  Copy(GetStrippedDexSrc1(), dex_location);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kVerifyAtRuntime));
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kInterpretOnly));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());

  // Make the oat file up to date. This should have no effect.
  std::string error_msg;
  EXPECT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg)) << error_msg;

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a DEX file, no ODEX file and an OAT file that needs
// relocation.
// Expect: The status is kSelfPatchOatNeeded.
TEST_F(OatFileAssistantTest, SelfRelocation) {
  std::string dex_location = GetScratchDir() + "/SelfRelocation.jar";
  std::string oat_location = GetOdexDir() + "/SelfRelocation.oat";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, oat_location, CompilerFilter::kSpeed);

  OatFileAssistant oat_file_assistant(dex_location.c_str(),
      oat_location.c_str(), kRuntimeISA, false, true);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kInterpretOnly));
  EXPECT_EQ(OatFileAssistant::kSelfPatchOatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kEverything));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());

  // Make the oat file up to date.
  std::string error_msg;
  ASSERT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg)) << error_msg;

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileNeedsRelocation());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileNeedsRelocation());
  EXPECT_TRUE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file, no ODEX file and an OAT file that needs
// relocation but doesn't have patch info.
// Expect: The status is kDex2OatNeeded, because we can't run patchoat.
TEST_F(OatFileAssistantTest, NoSelfRelocation) {
  std::string dex_location = GetScratchDir() + "/NoSelfRelocation.jar";
  std::string oat_location = GetOdexDir() + "/NoSelfRelocation.oat";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateNoPatchOdexForTest(dex_location, oat_location, CompilerFilter::kSpeed);

  OatFileAssistant oat_file_assistant(dex_location.c_str(),
      oat_location.c_str(), kRuntimeISA, false, true);

  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  // Make the oat file up to date.
  std::string error_msg;
  ASSERT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg)) << error_msg;
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file, an ODEX file and an OAT file, where the ODEX and
// OAT files both have patch delta of 0.
// Expect: It shouldn't crash, and status is kPatchOatNeeded.
TEST_F(OatFileAssistantTest, OdexOatOverlap) {
  std::string dex_location = GetScratchDir() + "/OdexOatOverlap.jar";
  std::string odex_location = GetOdexDir() + "/OdexOatOverlap.odex";
  std::string oat_location = GetOdexDir() + "/OdexOatOverlap.oat";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed);

  // Create the oat file by copying the odex so they are located in the same
  // place in memory.
  Copy(odex_location, oat_location);

  // Verify things don't go bad.
  OatFileAssistant oat_file_assistant(dex_location.c_str(),
      oat_location.c_str(), kRuntimeISA, false, true);

  EXPECT_EQ(OatFileAssistant::kPatchOatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());

  // Things aren't relocated, so it should fall back to interpreted.
  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);

  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file and a PIC ODEX file, but no OAT file.
// Expect: The status is kNoDexOptNeeded, because PIC needs no relocation.
TEST_F(OatFileAssistantTest, DexPicOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/DexPicOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/DexPicOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GeneratePicOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kEverything));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a DEX file and a VerifyAtRuntime ODEX file, but no OAT file.
// Expect: The status is kNoDexOptNeeded, because VerifyAtRuntime contains no code.
TEST_F(OatFileAssistantTest, DexVerifyAtRuntimeOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/DexVerifyAtRuntimeOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/DexVerifyAtRuntimeOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kVerifyAtRuntime);

  // Verify the status.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kVerifyAtRuntime));
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_TRUE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.HasOriginalDexFiles());
}

// Case: We have a DEX file and up-to-date OAT file for it.
// Expect: We should load an executable dex file.
TEST_F(OatFileAssistantTest, LoadOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/LoadOatUpToDate.jar";

  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeed);

  // Load the oat using an oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file and up-to-date interpret-only OAT file for it.
// Expect: We should still load the oat file as executable.
TEST_F(OatFileAssistantTest, LoadExecInterpretOnlyOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/LoadExecInterpretOnlyOatUpToDate.jar";

  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kInterpretOnly);

  // Load the oat using an oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file and up-to-date OAT file for it.
// Expect: Loading non-executable should load the oat non-executable.
TEST_F(OatFileAssistantTest, LoadNoExecOatUpToDate) {
  std::string dex_location = GetScratchDir() + "/LoadNoExecOatUpToDate.jar";

  Copy(GetDexSrc1(), dex_location);
  GenerateOatForTest(dex_location.c_str(), CompilerFilter::kSpeed);

  // Load the oat using an oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a DEX file.
// Expect: We should load an executable dex file from an alternative oat
// location.
TEST_F(OatFileAssistantTest, LoadDexNoAlternateOat) {
  std::string dex_location = GetScratchDir() + "/LoadDexNoAlternateOat.jar";
  std::string oat_location = GetScratchDir() + "/LoadDexNoAlternateOat.oat";

  Copy(GetDexSrc1(), dex_location);

  OatFileAssistant oat_file_assistant(
      dex_location.c_str(), oat_location.c_str(), kRuntimeISA, false, true);
  std::string error_msg;
  ASSERT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg)) << error_msg;

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_TRUE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());

  EXPECT_TRUE(OS::FileExists(oat_location.c_str()));

  // Verify it didn't create an oat in the default location.
  OatFileAssistant ofm(dex_location.c_str(), kRuntimeISA, false, false);
  EXPECT_FALSE(ofm.OatFileExists());
}

// Case: We have a DEX file but can't write the oat file.
// Expect: We should fail to make the oat file up to date.
TEST_F(OatFileAssistantTest, LoadDexUnwriteableAlternateOat) {
  std::string dex_location = GetScratchDir() + "/LoadDexUnwriteableAlternateOat.jar";

  // Make the oat location unwritable by inserting some non-existent
  // intermediate directories.
  std::string oat_location = GetScratchDir() + "/foo/bar/LoadDexUnwriteableAlternateOat.oat";

  Copy(GetDexSrc1(), dex_location);

  OatFileAssistant oat_file_assistant(
      dex_location.c_str(), oat_location.c_str(), kRuntimeISA, false, true);
  std::string error_msg;
  ASSERT_EQ(OatFileAssistant::kUpdateNotAttempted,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg));

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() == nullptr);
}

// Case: We don't have a DEX file and can't write the oat file.
// Expect: We should fail to generate the oat file without crashing.
TEST_F(OatFileAssistantTest, GenNoDex) {
  std::string dex_location = GetScratchDir() + "/GenNoDex.jar";
  std::string oat_location = GetScratchDir() + "/GenNoDex.oat";

  OatFileAssistant oat_file_assistant(
      dex_location.c_str(), oat_location.c_str(), kRuntimeISA, false, true);
  std::string error_msg;
  EXPECT_EQ(OatFileAssistant::kUpdateNotAttempted,
      oat_file_assistant.GenerateOatFile(CompilerFilter::kSpeed, &error_msg));
}

// Turn an absolute path into a path relative to the current working
// directory.
static std::string MakePathRelative(std::string target) {
  char buf[MAXPATHLEN];
  std::string cwd = getcwd(buf, MAXPATHLEN);

  // Split the target and cwd paths into components.
  std::vector<std::string> target_path;
  std::vector<std::string> cwd_path;
  Split(target, '/', &target_path);
  Split(cwd, '/', &cwd_path);

  // Reverse the path components, so we can use pop_back().
  std::reverse(target_path.begin(), target_path.end());
  std::reverse(cwd_path.begin(), cwd_path.end());

  // Drop the common prefix of the paths. Because we reversed the path
  // components, this becomes the common suffix of target_path and cwd_path.
  while (!target_path.empty() && !cwd_path.empty()
      && target_path.back() == cwd_path.back()) {
    target_path.pop_back();
    cwd_path.pop_back();
  }

  // For each element of the remaining cwd_path, add '..' to the beginning
  // of the target path. Because we reversed the path components, we add to
  // the end of target_path.
  for (unsigned int i = 0; i < cwd_path.size(); i++) {
    target_path.push_back("..");
  }

  // Reverse again to get the right path order, and join to get the result.
  std::reverse(target_path.begin(), target_path.end());
  return Join(target_path, '/');
}

// Case: Non-absolute path to Dex location.
// Expect: Not sure, but it shouldn't crash.
TEST_F(OatFileAssistantTest, NonAbsoluteDexLocation) {
  std::string abs_dex_location = GetScratchDir() + "/NonAbsoluteDexLocation.jar";
  Copy(GetDexSrc1(), abs_dex_location);

  std::string dex_location = MakePathRelative(abs_dex_location);
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
}

// Case: Very short, non-existent Dex location.
// Expect: kNoDexOptNeeded.
TEST_F(OatFileAssistantTest, ShortDexLocation) {
  std::string dex_location = "/xx";

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.HasOriginalDexFiles());

  // Trying to make it up to date should have no effect.
  std::string error_msg;
  EXPECT_EQ(OatFileAssistant::kUpdateSucceeded,
      oat_file_assistant.MakeUpToDate(CompilerFilter::kSpeed, &error_msg));
  EXPECT_TRUE(error_msg.empty());
}

// Case: Non-standard extension for dex file.
// Expect: The status is kDex2OatNeeded.
TEST_F(OatFileAssistantTest, LongDexExtension) {
  std::string dex_location = GetScratchDir() + "/LongDexExtension.jarx";
  Copy(GetDexSrc1(), dex_location);

  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, false);

  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded,
      oat_file_assistant.GetDexOptNeeded(CompilerFilter::kSpeed));

  EXPECT_FALSE(oat_file_assistant.IsInBootClassPath());
  EXPECT_FALSE(oat_file_assistant.OdexFileExists());
  EXPECT_TRUE(oat_file_assistant.OdexFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OdexFileIsUpToDate());
  EXPECT_FALSE(oat_file_assistant.OatFileExists());
  EXPECT_TRUE(oat_file_assistant.OatFileIsOutOfDate());
  EXPECT_FALSE(oat_file_assistant.OatFileIsUpToDate());
}

// A task to generate a dex location. Used by the RaceToGenerate test.
class RaceGenerateTask : public Task {
 public:
  explicit RaceGenerateTask(const std::string& dex_location, const std::string& oat_location)
    : dex_location_(dex_location), oat_location_(oat_location), loaded_oat_file_(nullptr)
  {}

  void Run(Thread* self ATTRIBUTE_UNUSED) {
    // Load the dex files, and save a pointer to the loaded oat file, so that
    // we can verify only one oat file was loaded for the dex location.
    std::vector<std::unique_ptr<const DexFile>> dex_files;
    std::vector<std::string> error_msgs;
    const OatFile* oat_file = nullptr;
    dex_files = Runtime::Current()->GetOatFileManager().OpenDexFilesFromOat(
        dex_location_.c_str(),
        oat_location_.c_str(),
        /*class_loader*/nullptr,
        /*dex_elements*/nullptr,
        &oat_file,
        &error_msgs);
    CHECK(!dex_files.empty()) << Join(error_msgs, '\n');
    CHECK(dex_files[0]->GetOatDexFile() != nullptr) << dex_files[0]->GetLocation();
    loaded_oat_file_ = dex_files[0]->GetOatDexFile()->GetOatFile();
    CHECK_EQ(loaded_oat_file_, oat_file);
  }

  const OatFile* GetLoadedOatFile() const {
    return loaded_oat_file_;
  }

 private:
  std::string dex_location_;
  std::string oat_location_;
  const OatFile* loaded_oat_file_;
};

// Test the case where multiple processes race to generate an oat file.
// This simulates multiple processes using multiple threads.
//
// We want unique Oat files to be loaded even when there is a race to load.
// TODO: The test case no longer tests locking the way it was intended since we now get multiple
// copies of the same Oat files mapped at different locations.
TEST_F(OatFileAssistantTest, RaceToGenerate) {
  std::string dex_location = GetScratchDir() + "/RaceToGenerate.jar";
  std::string oat_location = GetOdexDir() + "/RaceToGenerate.oat";

  // We use the lib core dex file, because it's large, and hopefully should
  // take a while to generate.
  Copy(GetLibCoreDexFileNames()[0], dex_location);

  const int kNumThreads = 32;
  Thread* self = Thread::Current();
  ThreadPool thread_pool("Oat file assistant test thread pool", kNumThreads);
  std::vector<std::unique_ptr<RaceGenerateTask>> tasks;
  for (int i = 0; i < kNumThreads; i++) {
    std::unique_ptr<RaceGenerateTask> task(new RaceGenerateTask(dex_location, oat_location));
    thread_pool.AddTask(self, task.get());
    tasks.push_back(std::move(task));
  }
  thread_pool.StartWorkers(self);
  thread_pool.Wait(self, true, false);

  // Verify every task got a unique oat file.
  std::set<const OatFile*> oat_files;
  for (auto& task : tasks) {
    const OatFile* oat_file = task->GetLoadedOatFile();
    EXPECT_TRUE(oat_files.find(oat_file) == oat_files.end());
    oat_files.insert(oat_file);
  }
}

// Case: We have a DEX file and an ODEX file, no OAT file, and dex2oat is
// disabled.
// Expect: We should load the odex file non-executable.
TEST_F(OatFileAssistantNoDex2OatTest, LoadDexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/LoadDexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/LoadDexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed);

  // Load the oat using an executable oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(1u, dex_files.size());
}

// Case: We have a MultiDEX file and an ODEX file, no OAT file, and dex2oat is
// disabled.
// Expect: We should load the odex file non-executable.
TEST_F(OatFileAssistantNoDex2OatTest, LoadMultiDexOdexNoOat) {
  std::string dex_location = GetScratchDir() + "/LoadMultiDexOdexNoOat.jar";
  std::string odex_location = GetOdexDir() + "/LoadMultiDexOdexNoOat.odex";

  // Create the dex and odex files
  Copy(GetMultiDexSrc1(), dex_location);
  GenerateOdexForTest(dex_location, odex_location, CompilerFilter::kSpeed);

  // Load the oat using an executable oat file assistant.
  OatFileAssistant oat_file_assistant(dex_location.c_str(), kRuntimeISA, false, true);

  std::unique_ptr<OatFile> oat_file = oat_file_assistant.GetBestOatFile();
  ASSERT_TRUE(oat_file.get() != nullptr);
  EXPECT_FALSE(oat_file->IsExecutable());
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  dex_files = oat_file_assistant.LoadDexFiles(*oat_file, dex_location.c_str());
  EXPECT_EQ(2u, dex_files.size());
}

TEST(OatFileAssistantUtilsTest, DexFilenameToOdexFilename) {
  std::string error_msg;
  std::string odex_file;

  EXPECT_TRUE(OatFileAssistant::DexFilenameToOdexFilename(
        "/foo/bar/baz.jar", kArm, &odex_file, &error_msg)) << error_msg;
  EXPECT_EQ("/foo/bar/oat/arm/baz.odex", odex_file);

  EXPECT_TRUE(OatFileAssistant::DexFilenameToOdexFilename(
        "/foo/bar/baz.funnyext", kArm, &odex_file, &error_msg)) << error_msg;
  EXPECT_EQ("/foo/bar/oat/arm/baz.odex", odex_file);

  EXPECT_FALSE(OatFileAssistant::DexFilenameToOdexFilename(
        "nopath.jar", kArm, &odex_file, &error_msg));
  EXPECT_FALSE(OatFileAssistant::DexFilenameToOdexFilename(
        "/foo/bar/baz_noext", kArm, &odex_file, &error_msg));
}

// Verify the dexopt status values from dalvik.system.DexFile
// match the OatFileAssistant::DexOptStatus values.
TEST_F(OatFileAssistantTest, DexOptStatusValues) {
  ScopedObjectAccess soa(Thread::Current());
  StackHandleScope<1> hs(soa.Self());
  ClassLinker* linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> dexfile(
      hs.NewHandle(linker->FindSystemClass(soa.Self(), "Ldalvik/system/DexFile;")));
  ASSERT_FALSE(dexfile.Get() == nullptr);
  linker->EnsureInitialized(soa.Self(), dexfile, true, true);

  ArtField* no_dexopt_needed = mirror::Class::FindStaticField(
      soa.Self(), dexfile, "NO_DEXOPT_NEEDED", "I");
  ASSERT_FALSE(no_dexopt_needed == nullptr);
  EXPECT_EQ(no_dexopt_needed->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  EXPECT_EQ(OatFileAssistant::kNoDexOptNeeded, no_dexopt_needed->GetInt(dexfile.Get()));

  ArtField* dex2oat_needed = mirror::Class::FindStaticField(
      soa.Self(), dexfile, "DEX2OAT_NEEDED", "I");
  ASSERT_FALSE(dex2oat_needed == nullptr);
  EXPECT_EQ(dex2oat_needed->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  EXPECT_EQ(OatFileAssistant::kDex2OatNeeded, dex2oat_needed->GetInt(dexfile.Get()));

  ArtField* patchoat_needed = mirror::Class::FindStaticField(
      soa.Self(), dexfile, "PATCHOAT_NEEDED", "I");
  ASSERT_FALSE(patchoat_needed == nullptr);
  EXPECT_EQ(patchoat_needed->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  EXPECT_EQ(OatFileAssistant::kPatchOatNeeded, patchoat_needed->GetInt(dexfile.Get()));

  ArtField* self_patchoat_needed = mirror::Class::FindStaticField(
      soa.Self(), dexfile, "SELF_PATCHOAT_NEEDED", "I");
  ASSERT_FALSE(self_patchoat_needed == nullptr);
  EXPECT_EQ(self_patchoat_needed->GetTypeAsPrimitiveType(), Primitive::kPrimInt);
  EXPECT_EQ(OatFileAssistant::kSelfPatchOatNeeded, self_patchoat_needed->GetInt(dexfile.Get()));
}

// TODO: More Tests:
//  * Image checksum change is out of date for kIntepretOnly, but not
//    kVerifyAtRuntime. But target of kVerifyAtRuntime still says current
//    kInterpretOnly is out of date.
//  * Test class linker falls back to unquickened dex for DexNoOat
//  * Test class linker falls back to unquickened dex for MultiDexNoOat
//  * Test using secondary isa
//  * Test for status of oat while oat is being generated (how?)
//  * Test case where 32 and 64 bit boot class paths differ,
//      and we ask IsInBootClassPath for a class in exactly one of the 32 or
//      64 bit boot class paths.
//  * Test unexpected scenarios (?):
//    - Dex is stripped, don't have odex.
//    - Oat file corrupted after status check, before reload unexecutable
//    because it's unrelocated and no dex2oat
//  * Test unrelocated specific target compilation type can be relocated to
//    make it up to date.

}  // namespace art
