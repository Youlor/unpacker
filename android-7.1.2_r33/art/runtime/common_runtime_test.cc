/*
 * Copyright (C) 2012 The Android Open Source Project
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

#include "common_runtime_test.h"

#include <cstdio>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ScopedLocalRef.h>
#include <stdlib.h>

#include "../../external/icu/icu4c/source/common/unicode/uvernum.h"
#include "art_field-inl.h"
#include "base/macros.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"
#include "class_linker.h"
#include "compiler_callbacks.h"
#include "dex_file-inl.h"
#include "gc_root-inl.h"
#include "gc/heap.h"
#include "gtest/gtest.h"
#include "handle_scope-inl.h"
#include "interpreter/unstarted_runtime.h"
#include "jni_internal.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mem_map.h"
#include "native/dalvik_system_DexFile.h"
#include "noop_compiler_callbacks.h"
#include "os.h"
#include "primitive.h"
#include "runtime-inl.h"
#include "scoped_thread_state_change.h"
#include "thread.h"
#include "well_known_classes.h"

int main(int argc, char **argv) {
  // Gtests can be very noisy. For example, an executable with multiple tests will trigger native
  // bridge warnings. The following line reduces the minimum log severity to ERROR and suppresses
  // everything else. In case you want to see all messages, comment out the line.
  setenv("ANDROID_LOG_TAGS", "*:e", 1);

  art::InitLogging(argv);
  LOG(::art::INFO) << "Running main() from common_runtime_test.cc...";
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace art {

ScratchFile::ScratchFile() {
  // ANDROID_DATA needs to be set
  CHECK_NE(static_cast<char*>(nullptr), getenv("ANDROID_DATA")) <<
      "Are you subclassing RuntimeTest?";
  filename_ = getenv("ANDROID_DATA");
  filename_ += "/TmpFile-XXXXXX";
  int fd = mkstemp(&filename_[0]);
  CHECK_NE(-1, fd) << strerror(errno) << " for " << filename_;
  file_.reset(new File(fd, GetFilename(), true));
}

ScratchFile::ScratchFile(const ScratchFile& other, const char* suffix)
    : ScratchFile(other.GetFilename() + suffix) {}

ScratchFile::ScratchFile(const std::string& filename) : filename_(filename) {
  int fd = open(filename_.c_str(), O_RDWR | O_CREAT, 0666);
  CHECK_NE(-1, fd);
  file_.reset(new File(fd, GetFilename(), true));
}

ScratchFile::ScratchFile(File* file) {
  CHECK(file != nullptr);
  filename_ = file->GetPath();
  file_.reset(file);
}

ScratchFile::ScratchFile(ScratchFile&& other) {
  *this = std::move(other);
}

ScratchFile& ScratchFile::operator=(ScratchFile&& other) {
  if (GetFile() != other.GetFile()) {
    std::swap(filename_, other.filename_);
    std::swap(file_, other.file_);
  }
  return *this;
}

ScratchFile::~ScratchFile() {
  Unlink();
}

int ScratchFile::GetFd() const {
  return file_->Fd();
}

void ScratchFile::Close() {
  if (file_.get() != nullptr) {
    if (file_->FlushCloseOrErase() != 0) {
      PLOG(WARNING) << "Error closing scratch file.";
    }
  }
}

void ScratchFile::Unlink() {
  if (!OS::FileExists(filename_.c_str())) {
    return;
  }
  Close();
  int unlink_result = unlink(filename_.c_str());
  CHECK_EQ(0, unlink_result);
}

static bool unstarted_initialized_ = false;

CommonRuntimeTestImpl::CommonRuntimeTestImpl() {}

CommonRuntimeTestImpl::~CommonRuntimeTestImpl() {
  // Ensure the dex files are cleaned up before the runtime.
  loaded_dex_files_.clear();
  runtime_.reset();
}

void CommonRuntimeTestImpl::SetUpAndroidRoot() {
  if (IsHost()) {
    // $ANDROID_ROOT is set on the device, but not necessarily on the host.
    // But it needs to be set so that icu4c can find its locale data.
    const char* android_root_from_env = getenv("ANDROID_ROOT");
    if (android_root_from_env == nullptr) {
      // Use ANDROID_HOST_OUT for ANDROID_ROOT if it is set.
      const char* android_host_out = getenv("ANDROID_HOST_OUT");
      if (android_host_out != nullptr) {
        setenv("ANDROID_ROOT", android_host_out, 1);
      } else {
        // Build it from ANDROID_BUILD_TOP or cwd
        std::string root;
        const char* android_build_top = getenv("ANDROID_BUILD_TOP");
        if (android_build_top != nullptr) {
          root += android_build_top;
        } else {
          // Not set by build server, so default to current directory
          char* cwd = getcwd(nullptr, 0);
          setenv("ANDROID_BUILD_TOP", cwd, 1);
          root += cwd;
          free(cwd);
        }
#if defined(__linux__)
        root += "/out/host/linux-x86";
#elif defined(__APPLE__)
        root += "/out/host/darwin-x86";
#else
#error unsupported OS
#endif
        setenv("ANDROID_ROOT", root.c_str(), 1);
      }
    }
    setenv("LD_LIBRARY_PATH", ":", 0);  // Required by java.lang.System.<clinit>.

    // Not set by build server, so default
    if (getenv("ANDROID_HOST_OUT") == nullptr) {
      setenv("ANDROID_HOST_OUT", getenv("ANDROID_ROOT"), 1);
    }
  }
}

void CommonRuntimeTestImpl::SetUpAndroidData(std::string& android_data) {
  // On target, Cannot use /mnt/sdcard because it is mounted noexec, so use subdir of dalvik-cache
  if (IsHost()) {
    const char* tmpdir = getenv("TMPDIR");
    if (tmpdir != nullptr && tmpdir[0] != 0) {
      android_data = tmpdir;
    } else {
      android_data = "/tmp";
    }
  } else {
    android_data = "/data/dalvik-cache";
  }
  android_data += "/art-data-XXXXXX";
  if (mkdtemp(&android_data[0]) == nullptr) {
    PLOG(FATAL) << "mkdtemp(\"" << &android_data[0] << "\") failed";
  }
  setenv("ANDROID_DATA", android_data.c_str(), 1);
}

void CommonRuntimeTestImpl::TearDownAndroidData(const std::string& android_data,
                                                bool fail_on_error) {
  if (fail_on_error) {
    ASSERT_EQ(rmdir(android_data.c_str()), 0);
  } else {
    rmdir(android_data.c_str());
  }
}

// Helper - find directory with the following format:
// ${ANDROID_BUILD_TOP}/${subdir1}/${subdir2}-${version}/${subdir3}/bin/
static std::string GetAndroidToolsDir(const std::string& subdir1,
                                      const std::string& subdir2,
                                      const std::string& subdir3) {
  std::string root;
  const char* android_build_top = getenv("ANDROID_BUILD_TOP");
  if (android_build_top != nullptr) {
    root = android_build_top;
  } else {
    // Not set by build server, so default to current directory
    char* cwd = getcwd(nullptr, 0);
    setenv("ANDROID_BUILD_TOP", cwd, 1);
    root = cwd;
    free(cwd);
  }

  std::string toolsdir = root + "/" + subdir1;
  std::string founddir;
  DIR* dir;
  if ((dir = opendir(toolsdir.c_str())) != nullptr) {
    float maxversion = 0;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
      std::string format = subdir2 + "-%f";
      float version;
      if (std::sscanf(entry->d_name, format.c_str(), &version) == 1) {
        if (version > maxversion) {
          maxversion = version;
          founddir = toolsdir + "/" + entry->d_name + "/" + subdir3 + "/bin/";
        }
      }
    }
    closedir(dir);
  }

  if (founddir.empty()) {
    ADD_FAILURE() << "Cannot find Android tools directory.";
  }
  return founddir;
}

std::string CommonRuntimeTestImpl::GetAndroidHostToolsDir() {
  return GetAndroidToolsDir("prebuilts/gcc/linux-x86/host",
                            "x86_64-linux-glibc2.15",
                            "x86_64-linux");
}

std::string CommonRuntimeTestImpl::GetAndroidTargetToolsDir(InstructionSet isa) {
  switch (isa) {
    case kArm:
    case kThumb2:
      return GetAndroidToolsDir("prebuilts/gcc/linux-x86/arm",
                                "arm-linux-androideabi",
                                "arm-linux-androideabi");
    case kArm64:
      return GetAndroidToolsDir("prebuilts/gcc/linux-x86/aarch64",
                                "aarch64-linux-android",
                                "aarch64-linux-android");
    case kX86:
    case kX86_64:
      return GetAndroidToolsDir("prebuilts/gcc/linux-x86/x86",
                                "x86_64-linux-android",
                                "x86_64-linux-android");
    case kMips:
    case kMips64:
      return GetAndroidToolsDir("prebuilts/gcc/linux-x86/mips",
                                "mips64el-linux-android",
                                "mips64el-linux-android");
    case kNone:
      break;
  }
  ADD_FAILURE() << "Invalid isa " << isa;
  return "";
}

std::string CommonRuntimeTestImpl::GetCoreArtLocation() {
  return GetCoreFileLocation("art");
}

std::string CommonRuntimeTestImpl::GetCoreOatLocation() {
  return GetCoreFileLocation("oat");
}

std::unique_ptr<const DexFile> CommonRuntimeTestImpl::LoadExpectSingleDexFile(
    const char* location) {
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  std::string error_msg;
  MemMap::Init();
  if (!DexFile::Open(location, location, &error_msg, &dex_files)) {
    LOG(FATAL) << "Could not open .dex file '" << location << "': " << error_msg << "\n";
    UNREACHABLE();
  } else {
    CHECK_EQ(1U, dex_files.size()) << "Expected only one dex file in " << location;
    return std::move(dex_files[0]);
  }
}

void CommonRuntimeTestImpl::SetUp() {
  SetUpAndroidRoot();
  SetUpAndroidData(android_data_);
  dalvik_cache_.append(android_data_.c_str());
  dalvik_cache_.append("/dalvik-cache");
  int mkdir_result = mkdir(dalvik_cache_.c_str(), 0700);
  ASSERT_EQ(mkdir_result, 0);

  std::string min_heap_string(StringPrintf("-Xms%zdm", gc::Heap::kDefaultInitialSize / MB));
  std::string max_heap_string(StringPrintf("-Xmx%zdm", gc::Heap::kDefaultMaximumSize / MB));


  RuntimeOptions options;
  std::string boot_class_path_string = "-Xbootclasspath";
  for (const std::string &core_dex_file_name : GetLibCoreDexFileNames()) {
    boot_class_path_string += ":";
    boot_class_path_string += core_dex_file_name;
  }

  options.push_back(std::make_pair(boot_class_path_string, nullptr));
  options.push_back(std::make_pair("-Xcheck:jni", nullptr));
  options.push_back(std::make_pair(min_heap_string, nullptr));
  options.push_back(std::make_pair(max_heap_string, nullptr));

  callbacks_.reset(new NoopCompilerCallbacks());

  SetUpRuntimeOptions(&options);

  // Install compiler-callbacks if SetupRuntimeOptions hasn't deleted them.
  if (callbacks_.get() != nullptr) {
    options.push_back(std::make_pair("compilercallbacks", callbacks_.get()));
  }

  PreRuntimeCreate();
  if (!Runtime::Create(options, false)) {
    LOG(FATAL) << "Failed to create runtime";
    return;
  }
  PostRuntimeCreate();
  runtime_.reset(Runtime::Current());
  class_linker_ = runtime_->GetClassLinker();
  class_linker_->FixupDexCaches(runtime_->GetResolutionMethod());

  // Runtime::Create acquired the mutator_lock_ that is normally given away when we
  // Runtime::Start, give it away now and then switch to a more managable ScopedObjectAccess.
  Thread::Current()->TransitionFromRunnableToSuspended(kNative);

  // Get the boot class path from the runtime so it can be used in tests.
  boot_class_path_ = class_linker_->GetBootClassPath();
  ASSERT_FALSE(boot_class_path_.empty());
  java_lang_dex_file_ = boot_class_path_[0];

  FinalizeSetup();
}

void CommonRuntimeTestImpl::FinalizeSetup() {
  // Initialize maps for unstarted runtime. This needs to be here, as running clinits needs this
  // set up.
  if (!unstarted_initialized_) {
    interpreter::UnstartedRuntime::Initialize();
    unstarted_initialized_ = true;
  }

  {
    ScopedObjectAccess soa(Thread::Current());
    class_linker_->RunRootClinits();
  }

  // We're back in native, take the opportunity to initialize well known classes.
  WellKnownClasses::Init(Thread::Current()->GetJniEnv());

  // Create the heap thread pool so that the GC runs in parallel for tests. Normally, the thread
  // pool is created by the runtime.
  runtime_->GetHeap()->CreateThreadPool();
  runtime_->GetHeap()->VerifyHeap();  // Check for heap corruption before the test
  // Reduce timinig-dependent flakiness in OOME behavior (eg StubTest.AllocObject).
  runtime_->GetHeap()->SetMinIntervalHomogeneousSpaceCompactionByOom(0U);
}

void CommonRuntimeTestImpl::ClearDirectory(const char* dirpath) {
  ASSERT_TRUE(dirpath != nullptr);
  DIR* dir = opendir(dirpath);
  ASSERT_TRUE(dir != nullptr);
  dirent* e;
  struct stat s;
  while ((e = readdir(dir)) != nullptr) {
    if ((strcmp(e->d_name, ".") == 0) || (strcmp(e->d_name, "..") == 0)) {
      continue;
    }
    std::string filename(dirpath);
    filename.push_back('/');
    filename.append(e->d_name);
    int stat_result = lstat(filename.c_str(), &s);
    ASSERT_EQ(0, stat_result) << "unable to stat " << filename;
    if (S_ISDIR(s.st_mode)) {
      ClearDirectory(filename.c_str());
      int rmdir_result = rmdir(filename.c_str());
      ASSERT_EQ(0, rmdir_result) << filename;
    } else {
      int unlink_result = unlink(filename.c_str());
      ASSERT_EQ(0, unlink_result) << filename;
    }
  }
  closedir(dir);
}

void CommonRuntimeTestImpl::TearDown() {
  const char* android_data = getenv("ANDROID_DATA");
  ASSERT_TRUE(android_data != nullptr);
  ClearDirectory(dalvik_cache_.c_str());
  int rmdir_cache_result = rmdir(dalvik_cache_.c_str());
  ASSERT_EQ(0, rmdir_cache_result);
  TearDownAndroidData(android_data_, true);
  dalvik_cache_.clear();

  // icu4c has a fixed 10-element array "gCommonICUDataArray".
  // If we run > 10 tests, we fill that array and u_setCommonData fails.
  // There's a function to clear the array, but it's not public...
  typedef void (*IcuCleanupFn)();
  void* sym = dlsym(RTLD_DEFAULT, "u_cleanup_" U_ICU_VERSION_SHORT);
  CHECK(sym != nullptr) << dlerror();
  IcuCleanupFn icu_cleanup_fn = reinterpret_cast<IcuCleanupFn>(sym);
  (*icu_cleanup_fn)();

  Runtime::Current()->GetHeap()->VerifyHeap();  // Check for heap corruption after the test
}

static std::string GetDexFileName(const std::string& jar_prefix, bool host) {
  std::string path;
  if (host) {
    const char* host_dir = getenv("ANDROID_HOST_OUT");
    CHECK(host_dir != nullptr);
    path = host_dir;
  } else {
    path = GetAndroidRoot();
  }

  std::string suffix = host
      ? "-hostdex"                 // The host version.
      : "-testdex";                // The unstripped target version.

  return StringPrintf("%s/framework/%s%s.jar", path.c_str(), jar_prefix.c_str(), suffix.c_str());
}

std::vector<std::string> CommonRuntimeTestImpl::GetLibCoreDexFileNames() {
  return std::vector<std::string>({GetDexFileName("core-oj", IsHost()),
                                   GetDexFileName("core-libart", IsHost())});
}

std::string CommonRuntimeTestImpl::GetTestAndroidRoot() {
  if (IsHost()) {
    const char* host_dir = getenv("ANDROID_HOST_OUT");
    CHECK(host_dir != nullptr);
    return host_dir;
  }
  return GetAndroidRoot();
}

// Check that for target builds we have ART_TARGET_NATIVETEST_DIR set.
#ifdef ART_TARGET
#ifndef ART_TARGET_NATIVETEST_DIR
#error "ART_TARGET_NATIVETEST_DIR not set."
#endif
// Wrap it as a string literal.
#define ART_TARGET_NATIVETEST_DIR_STRING STRINGIFY(ART_TARGET_NATIVETEST_DIR) "/"
#else
#define ART_TARGET_NATIVETEST_DIR_STRING ""
#endif

std::string CommonRuntimeTestImpl::GetTestDexFileName(const char* name) const {
  CHECK(name != nullptr);
  std::string filename;
  if (IsHost()) {
    filename += getenv("ANDROID_HOST_OUT");
    filename += "/framework/";
  } else {
    filename += ART_TARGET_NATIVETEST_DIR_STRING;
  }
  filename += "art-gtest-";
  filename += name;
  filename += ".jar";
  return filename;
}

std::vector<std::unique_ptr<const DexFile>> CommonRuntimeTestImpl::OpenTestDexFiles(
    const char* name) {
  std::string filename = GetTestDexFileName(name);
  std::string error_msg;
  std::vector<std::unique_ptr<const DexFile>> dex_files;
  bool success = DexFile::Open(filename.c_str(), filename.c_str(), &error_msg, &dex_files);
  CHECK(success) << "Failed to open '" << filename << "': " << error_msg;
  for (auto& dex_file : dex_files) {
    CHECK_EQ(PROT_READ, dex_file->GetPermissions());
    CHECK(dex_file->IsReadOnly());
  }
  return dex_files;
}

std::unique_ptr<const DexFile> CommonRuntimeTestImpl::OpenTestDexFile(const char* name) {
  std::vector<std::unique_ptr<const DexFile>> vector = OpenTestDexFiles(name);
  EXPECT_EQ(1U, vector.size());
  return std::move(vector[0]);
}

std::vector<const DexFile*> CommonRuntimeTestImpl::GetDexFiles(jobject jclass_loader) {
  std::vector<const DexFile*> ret;

  ScopedObjectAccess soa(Thread::Current());

  StackHandleScope<2> hs(soa.Self());
  Handle<mirror::ClassLoader> class_loader = hs.NewHandle(
      soa.Decode<mirror::ClassLoader*>(jclass_loader));

  DCHECK_EQ(class_loader->GetClass(),
            soa.Decode<mirror::Class*>(WellKnownClasses::dalvik_system_PathClassLoader));
  DCHECK_EQ(class_loader->GetParent()->GetClass(),
            soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_BootClassLoader));

  // The class loader is a PathClassLoader which inherits from BaseDexClassLoader.
  // We need to get the DexPathList and loop through it.
  ArtField* cookie_field = soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie);
  ArtField* dex_file_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile);
  mirror::Object* dex_path_list =
      soa.DecodeField(WellKnownClasses::dalvik_system_PathClassLoader_pathList)->
      GetObject(class_loader.Get());
  if (dex_path_list != nullptr && dex_file_field!= nullptr && cookie_field != nullptr) {
    // DexPathList has an array dexElements of Elements[] which each contain a dex file.
    mirror::Object* dex_elements_obj =
        soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList_dexElements)->
        GetObject(dex_path_list);
    // Loop through each dalvik.system.DexPathList$Element's dalvik.system.DexFile and look
    // at the mCookie which is a DexFile vector.
    if (dex_elements_obj != nullptr) {
      Handle<mirror::ObjectArray<mirror::Object>> dex_elements =
          hs.NewHandle(dex_elements_obj->AsObjectArray<mirror::Object>());
      for (int32_t i = 0; i < dex_elements->GetLength(); ++i) {
        mirror::Object* element = dex_elements->GetWithoutChecks(i);
        if (element == nullptr) {
          // Should never happen, fall back to java code to throw a NPE.
          break;
        }
        mirror::Object* dex_file = dex_file_field->GetObject(element);
        if (dex_file != nullptr) {
          mirror::LongArray* long_array = cookie_field->GetObject(dex_file)->AsLongArray();
          DCHECK(long_array != nullptr);
          int32_t long_array_size = long_array->GetLength();
          for (int32_t j = kDexFileIndexStart; j < long_array_size; ++j) {
            const DexFile* cp_dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(
                long_array->GetWithoutChecks(j)));
            if (cp_dex_file == nullptr) {
              LOG(WARNING) << "Null DexFile";
              continue;
            }
            ret.push_back(cp_dex_file);
          }
        }
      }
    }
  }

  return ret;
}

const DexFile* CommonRuntimeTestImpl::GetFirstDexFile(jobject jclass_loader) {
  std::vector<const DexFile*> tmp(GetDexFiles(jclass_loader));
  DCHECK(!tmp.empty());
  const DexFile* ret = tmp[0];
  DCHECK(ret != nullptr);
  return ret;
}

jobject CommonRuntimeTestImpl::LoadDex(const char* dex_name) {
  std::vector<std::unique_ptr<const DexFile>> dex_files = OpenTestDexFiles(dex_name);
  std::vector<const DexFile*> class_path;
  CHECK_NE(0U, dex_files.size());
  for (auto& dex_file : dex_files) {
    class_path.push_back(dex_file.get());
    loaded_dex_files_.push_back(std::move(dex_file));
  }

  Thread* self = Thread::Current();
  jobject class_loader = Runtime::Current()->GetClassLinker()->CreatePathClassLoader(self,
                                                                                     class_path);
  self->SetClassLoaderOverride(class_loader);
  return class_loader;
}

std::string CommonRuntimeTestImpl::GetCoreFileLocation(const char* suffix) {
  CHECK(suffix != nullptr);

  std::string location;
  if (IsHost()) {
    const char* host_dir = getenv("ANDROID_HOST_OUT");
    CHECK(host_dir != nullptr);
    location = StringPrintf("%s/framework/core.%s", host_dir, suffix);
  } else {
    location = StringPrintf("/data/art-test/core.%s", suffix);
  }

  return location;
}

CheckJniAbortCatcher::CheckJniAbortCatcher() : vm_(Runtime::Current()->GetJavaVM()) {
  vm_->SetCheckJniAbortHook(Hook, &actual_);
}

CheckJniAbortCatcher::~CheckJniAbortCatcher() {
  vm_->SetCheckJniAbortHook(nullptr, nullptr);
  EXPECT_TRUE(actual_.empty()) << actual_;
}

void CheckJniAbortCatcher::Check(const char* expected_text) {
  EXPECT_TRUE(actual_.find(expected_text) != std::string::npos) << "\n"
      << "Expected to find: " << expected_text << "\n"
      << "In the output   : " << actual_;
  actual_.clear();
}

void CheckJniAbortCatcher::Hook(void* data, const std::string& reason) {
  // We use += because when we're hooking the aborts like this, multiple problems can be found.
  *reinterpret_cast<std::string*>(data) += reason;
}

}  // namespace art

namespace std {

template <typename T>
std::ostream& operator<<(std::ostream& os, const std::vector<T>& rhs) {
os << ::art::ToString(rhs);
return os;
}

}  // namespace std
