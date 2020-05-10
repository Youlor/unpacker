/*
 * Copyright (C) 2015 The Android Open Source Project
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

#include "oat_file_manager.h"

#include <memory>
#include <queue>
#include <vector>

#include "base/logging.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "class_linker.h"
#include "dex_file-inl.h"
#include "gc/scoped_gc_critical_section.h"
#include "gc/space/image_space.h"
#include "handle_scope-inl.h"
#include "mirror/class_loader.h"
#include "oat_file_assistant.h"
#include "scoped_thread_state_change.h"
#include "thread-inl.h"
#include "thread_list.h"

namespace art {

// If true, then we attempt to load the application image if it exists.
static constexpr bool kEnableAppImage = true;

CompilerFilter::Filter OatFileManager::filter_ = CompilerFilter::Filter::kSpeed;

const OatFile* OatFileManager::RegisterOatFile(std::unique_ptr<const OatFile> oat_file) {
  WriterMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  DCHECK(oat_file != nullptr);
  if (kIsDebugBuild) {
    CHECK(oat_files_.find(oat_file) == oat_files_.end());
    for (const std::unique_ptr<const OatFile>& existing : oat_files_) {
      CHECK_NE(oat_file.get(), existing.get()) << oat_file->GetLocation();
      // Check that we don't have an oat file with the same address. Copies of the same oat file
      // should be loaded at different addresses.
      CHECK_NE(oat_file->Begin(), existing->Begin()) << "Oat file already mapped at that location";
    }
  }
  have_non_pic_oat_file_ = have_non_pic_oat_file_ || !oat_file->IsPic();
  const OatFile* ret = oat_file.get();
  oat_files_.insert(std::move(oat_file));
  return ret;
}

void OatFileManager::UnRegisterAndDeleteOatFile(const OatFile* oat_file) {
  WriterMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  DCHECK(oat_file != nullptr);
  std::unique_ptr<const OatFile> compare(oat_file);
  auto it = oat_files_.find(compare);
  CHECK(it != oat_files_.end());
  oat_files_.erase(it);
  compare.release();
}

const OatFile* OatFileManager::FindOpenedOatFileFromDexLocation(
    const std::string& dex_base_location) const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
    const std::vector<const OatDexFile*>& oat_dex_files = oat_file->GetOatDexFiles();
    for (const OatDexFile* oat_dex_file : oat_dex_files) {
      if (DexFile::GetBaseLocation(oat_dex_file->GetDexFileLocation()) == dex_base_location) {
        return oat_file.get();
      }
    }
  }
  return nullptr;
}

const OatFile* OatFileManager::FindOpenedOatFileFromOatLocation(const std::string& oat_location)
    const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  return FindOpenedOatFileFromOatLocationLocked(oat_location);
}

const OatFile* OatFileManager::FindOpenedOatFileFromOatLocationLocked(
    const std::string& oat_location) const {
  for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
    if (oat_file->GetLocation() == oat_location) {
      return oat_file.get();
    }
  }
  return nullptr;
}

std::vector<const OatFile*> OatFileManager::GetBootOatFiles() const {
  std::vector<const OatFile*> oat_files;
  std::vector<gc::space::ImageSpace*> image_spaces =
      Runtime::Current()->GetHeap()->GetBootImageSpaces();
  for (gc::space::ImageSpace* image_space : image_spaces) {
    oat_files.push_back(image_space->GetOatFile());
  }
  return oat_files;
}

const OatFile* OatFileManager::GetPrimaryOatFile() const {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  std::vector<const OatFile*> boot_oat_files = GetBootOatFiles();
  if (!boot_oat_files.empty()) {
    for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
      if (std::find(boot_oat_files.begin(), boot_oat_files.end(), oat_file.get()) ==
          boot_oat_files.end()) {
        return oat_file.get();
      }
    }
  }
  return nullptr;
}

OatFileManager::~OatFileManager() {
  // Explicitly clear oat_files_ since the OatFile destructor calls back into OatFileManager for
  // UnRegisterOatFileLocation.
  oat_files_.clear();
}

std::vector<const OatFile*> OatFileManager::RegisterImageOatFiles(
    std::vector<gc::space::ImageSpace*> spaces) {
  std::vector<const OatFile*> oat_files;
  for (gc::space::ImageSpace* space : spaces) {
    oat_files.push_back(RegisterOatFile(space->ReleaseOatFile()));
  }
  return oat_files;
}

class DexFileAndClassPair : ValueObject {
 public:
  DexFileAndClassPair(const DexFile* dex_file, size_t current_class_index, bool from_loaded_oat)
     : cached_descriptor_(GetClassDescriptor(dex_file, current_class_index)),
       dex_file_(dex_file),
       current_class_index_(current_class_index),
       from_loaded_oat_(from_loaded_oat) {}

  DexFileAndClassPair(const DexFileAndClassPair& rhs) = default;

  DexFileAndClassPair& operator=(const DexFileAndClassPair& rhs) = default;

  const char* GetCachedDescriptor() const {
    return cached_descriptor_;
  }

  bool operator<(const DexFileAndClassPair& rhs) const {
    const int cmp = strcmp(cached_descriptor_, rhs.cached_descriptor_);
    if (cmp != 0) {
      // Note that the order must be reversed. We want to iterate over the classes in dex files.
      // They are sorted lexicographically. Thus, the priority-queue must be a min-queue.
      return cmp > 0;
    }
    return dex_file_ < rhs.dex_file_;
  }

  bool DexFileHasMoreClasses() const {
    return current_class_index_ + 1 < dex_file_->NumClassDefs();
  }

  void Next() {
    ++current_class_index_;
    cached_descriptor_ = GetClassDescriptor(dex_file_, current_class_index_);
  }

  size_t GetCurrentClassIndex() const {
    return current_class_index_;
  }

  bool FromLoadedOat() const {
    return from_loaded_oat_;
  }

  const DexFile* GetDexFile() const {
    return dex_file_;
  }

 private:
  static const char* GetClassDescriptor(const DexFile* dex_file, size_t index) {
    DCHECK(IsUint<16>(index));
    const DexFile::ClassDef& class_def = dex_file->GetClassDef(static_cast<uint16_t>(index));
    return dex_file->StringByTypeIdx(class_def.class_idx_);
  }

  const char* cached_descriptor_;
  const DexFile* dex_file_;
  size_t current_class_index_;
  bool from_loaded_oat_;  // We only need to compare mismatches between what we load now
                          // and what was loaded before. Any old duplicates must have been
                          // OK, and any new "internal" duplicates are as well (they must
                          // be from multidex, which resolves correctly).
};

static void AddDexFilesFromOat(const OatFile* oat_file,
                               bool already_loaded,
                               /*out*/std::priority_queue<DexFileAndClassPair>* heap,
                               std::vector<std::unique_ptr<const DexFile>>* opened_dex_files) {
  for (const OatDexFile* oat_dex_file : oat_file->GetOatDexFiles()) {
    std::string error;
    std::unique_ptr<const DexFile> dex_file = oat_dex_file->OpenDexFile(&error);
    if (dex_file == nullptr) {
      LOG(WARNING) << "Could not create dex file from oat file: " << error;
    } else if (dex_file->NumClassDefs() > 0U) {
      heap->emplace(dex_file.get(), /*current_class_index*/0U, already_loaded);
      opened_dex_files->push_back(std::move(dex_file));
    }
  }
}

static void AddNext(/*inout*/DexFileAndClassPair* original,
                    /*inout*/std::priority_queue<DexFileAndClassPair>* heap) {
  if (original->DexFileHasMoreClasses()) {
    original->Next();
    heap->push(std::move(*original));
  }
}

static void IterateOverJavaDexFile(mirror::Object* dex_file,
                                   ArtField* const cookie_field,
                                   std::function<bool(const DexFile*)> fn)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (dex_file != nullptr) {
    mirror::LongArray* long_array = cookie_field->GetObject(dex_file)->AsLongArray();
    if (long_array == nullptr) {
      // This should never happen so log a warning.
      LOG(WARNING) << "Null DexFile::mCookie";
      return;
    }
    int32_t long_array_size = long_array->GetLength();
    // Start from 1 to skip the oat file.
    for (int32_t j = 1; j < long_array_size; ++j) {
      const DexFile* cp_dex_file = reinterpret_cast<const DexFile*>(static_cast<uintptr_t>(
          long_array->GetWithoutChecks(j)));
      if (!fn(cp_dex_file)) {
        return;
      }
    }
  }
}

static void IterateOverPathClassLoader(
    ScopedObjectAccessAlreadyRunnable& soa,
    Handle<mirror::ClassLoader> class_loader,
    MutableHandle<mirror::ObjectArray<mirror::Object>> dex_elements,
    std::function<bool(const DexFile*)> fn) SHARED_REQUIRES(Locks::mutator_lock_) {
  // Handle this step.
  // Handle as if this is the child PathClassLoader.
  // The class loader is a PathClassLoader which inherits from BaseDexClassLoader.
  // We need to get the DexPathList and loop through it.
  ArtField* const cookie_field = soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie);
  ArtField* const dex_file_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile);
  mirror::Object* dex_path_list =
      soa.DecodeField(WellKnownClasses::dalvik_system_PathClassLoader_pathList)->
      GetObject(class_loader.Get());
  if (dex_path_list != nullptr && dex_file_field != nullptr && cookie_field != nullptr) {
    // DexPathList has an array dexElements of Elements[] which each contain a dex file.
    mirror::Object* dex_elements_obj =
        soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList_dexElements)->
        GetObject(dex_path_list);
    // Loop through each dalvik.system.DexPathList$Element's dalvik.system.DexFile and look
    // at the mCookie which is a DexFile vector.
    if (dex_elements_obj != nullptr) {
      dex_elements.Assign(dex_elements_obj->AsObjectArray<mirror::Object>());
      for (int32_t i = 0; i < dex_elements->GetLength(); ++i) {
        mirror::Object* element = dex_elements->GetWithoutChecks(i);
        if (element == nullptr) {
          // Should never happen, fall back to java code to throw a NPE.
          break;
        }
        mirror::Object* dex_file = dex_file_field->GetObject(element);
        IterateOverJavaDexFile(dex_file, cookie_field, fn);
      }
    }
  }
}

static bool GetDexFilesFromClassLoader(
    ScopedObjectAccessAlreadyRunnable& soa,
    mirror::ClassLoader* class_loader,
    std::priority_queue<DexFileAndClassPair>* queue) SHARED_REQUIRES(Locks::mutator_lock_) {
  if (ClassLinker::IsBootClassLoader(soa, class_loader)) {
    // The boot class loader. We don't load any of these files, as we know we compiled against
    // them correctly.
    return true;
  }

  // Unsupported class-loader?
  if (class_loader->GetClass() !=
      soa.Decode<mirror::Class*>(WellKnownClasses::dalvik_system_PathClassLoader)) {
    VLOG(class_linker) << "Unsupported class-loader " << PrettyClass(class_loader->GetClass());
    return false;
  }

  bool recursive_result = GetDexFilesFromClassLoader(soa, class_loader->GetParent(), queue);
  if (!recursive_result) {
    // Something wrong up the chain.
    return false;
  }

  // Collect all the dex files.
  auto GetDexFilesFn = [&] (const DexFile* cp_dex_file)
            SHARED_REQUIRES(Locks::mutator_lock_) {
    if (cp_dex_file->NumClassDefs() > 0) {
      queue->emplace(cp_dex_file, 0U, true);
    }
    return true;  // Continue looking.
  };

  // Handle for dex-cache-element.
  StackHandleScope<3> hs(soa.Self());
  MutableHandle<mirror::ObjectArray<mirror::Object>> dex_elements(
      hs.NewHandle<mirror::ObjectArray<mirror::Object>>(nullptr));
  Handle<mirror::ClassLoader> h_class_loader(hs.NewHandle(class_loader));

  IterateOverPathClassLoader(soa, h_class_loader, dex_elements, GetDexFilesFn);

  return true;
}

static void GetDexFilesFromDexElementsArray(
    ScopedObjectAccessAlreadyRunnable& soa,
    Handle<mirror::ObjectArray<mirror::Object>> dex_elements,
    std::priority_queue<DexFileAndClassPair>* queue) SHARED_REQUIRES(Locks::mutator_lock_) {
  if (dex_elements.Get() == nullptr) {
    // Nothing to do.
    return;
  }

  ArtField* const cookie_field = soa.DecodeField(WellKnownClasses::dalvik_system_DexFile_cookie);
  ArtField* const dex_file_field =
      soa.DecodeField(WellKnownClasses::dalvik_system_DexPathList__Element_dexFile);
  const mirror::Class* const element_class = soa.Decode<mirror::Class*>(
      WellKnownClasses::dalvik_system_DexPathList__Element);
  const mirror::Class* const dexfile_class = soa.Decode<mirror::Class*>(
        WellKnownClasses::dalvik_system_DexFile);

  // Collect all the dex files.
  auto GetDexFilesFn = [&] (const DexFile* cp_dex_file)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    if (cp_dex_file != nullptr && cp_dex_file->NumClassDefs() > 0) {
      queue->emplace(cp_dex_file, 0U, true);
    }
    return true;  // Continue looking.
  };

  for (int32_t i = 0; i < dex_elements->GetLength(); ++i) {
    mirror::Object* element = dex_elements->GetWithoutChecks(i);
    if (element == nullptr) {
      continue;
    }

    // We support this being dalvik.system.DexPathList$Element and dalvik.system.DexFile.

    mirror::Object* dex_file;
    if (element->GetClass() == element_class) {
      dex_file = dex_file_field->GetObject(element);
    } else if (element->GetClass() == dexfile_class) {
      dex_file = element;
    } else {
      LOG(WARNING) << "Unsupported element in dex_elements: " << PrettyClass(element->GetClass());
      continue;
    }

    IterateOverJavaDexFile(dex_file, cookie_field, GetDexFilesFn);
  }
}

static bool AreSharedLibrariesOk(const std::string shared_libraries,
                                 std::priority_queue<DexFileAndClassPair>& queue) {
  if (shared_libraries.empty()) {
    if (queue.empty()) {
      // No shared libraries or oat files, as expected.
      return true;
    }
  } else {
    if (shared_libraries.compare(OatFile::kSpecialSharedLibrary) == 0) {
      // If we find the special shared library, skip the shared libraries check.
      return true;
    }
    // Shared libraries is a series of dex file paths and their checksums, each separated by '*'.
    std::vector<std::string> shared_libraries_split;
    Split(shared_libraries, '*', &shared_libraries_split);

    size_t index = 0;
    std::priority_queue<DexFileAndClassPair> temp = queue;
    while (!temp.empty() && index < shared_libraries_split.size() - 1) {
      DexFileAndClassPair pair(temp.top());
      const DexFile* dex_file = pair.GetDexFile();
      std::string dex_filename(dex_file->GetLocation());
      uint32_t dex_checksum = dex_file->GetLocationChecksum();
      if (dex_filename != shared_libraries_split[index] ||
          dex_checksum != std::stoul(shared_libraries_split[index + 1])) {
        break;
      }
      temp.pop();
      index += 2;
    }

    // Check is successful if it made it through the queue and all the shared libraries.
    return temp.empty() && index == shared_libraries_split.size();
  }
  return false;
}

// Check for class-def collisions in dex files.
//
// This first walks the class loader chain, getting all the dex files from the class loader. If
// the class loader is null or one of the class loaders in the chain is unsupported, we collect
// dex files from all open non-boot oat files to be safe.
//
// This first checks whether the shared libraries are in the expected order and the oat files
// have the expected checksums. If so, we exit early. Otherwise, we do the collision check.
//
// The collision check works by maintaining a heap with one class from each dex file, sorted by the
// class descriptor. Then a dex-file/class pair is continually removed from the heap and compared
// against the following top element. If the descriptor is the same, it is now checked whether
// the two elements agree on whether their dex file was from an already-loaded oat-file or the
// new oat file. Any disagreement indicates a collision.
bool OatFileManager::HasCollisions(const OatFile* oat_file,
                                   jobject class_loader,
                                   jobjectArray dex_elements,
                                   std::string* error_msg /*out*/) const {
  DCHECK(oat_file != nullptr);
  DCHECK(error_msg != nullptr);

  std::priority_queue<DexFileAndClassPair> queue;

  // Try to get dex files from the given class loader. If the class loader is null, or we do
  // not support one of the class loaders in the chain, conservatively compare against all
  // (non-boot) oat files.
  bool class_loader_ok = false;
  {
    ScopedObjectAccess soa(Thread::Current());
    StackHandleScope<2> hs(Thread::Current());
    Handle<mirror::ClassLoader> h_class_loader =
        hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader));
    Handle<mirror::ObjectArray<mirror::Object>> h_dex_elements =
        hs.NewHandle(soa.Decode<mirror::ObjectArray<mirror::Object>*>(dex_elements));
    if (h_class_loader.Get() != nullptr &&
        GetDexFilesFromClassLoader(soa, h_class_loader.Get(), &queue)) {
      class_loader_ok = true;

      // In this case, also take into account the dex_elements array, if given. We don't need to
      // read it otherwise, as we'll compare against all open oat files anyways.
      GetDexFilesFromDexElementsArray(soa, h_dex_elements, &queue);
    } else if (h_class_loader.Get() != nullptr) {
      VLOG(class_linker) << "Something unsupported with "
                         << PrettyClass(h_class_loader->GetClass());
    }
  }

  // Dex files are registered late - once a class is actually being loaded. We have to compare
  // against the open oat files. Take the oat_file_manager_lock_ that protects oat_files_ accesses.
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);

  // Vector that holds the newly opened dex files live, this is done to prevent leaks.
  std::vector<std::unique_ptr<const DexFile>> opened_dex_files;

  if (!class_loader_ok) {
    // Add dex files from already loaded oat files, but skip boot.

    // Clean up the queue.
    while (!queue.empty()) {
      queue.pop();
    }

    std::vector<const OatFile*> boot_oat_files = GetBootOatFiles();
    // The same OatFile can be loaded multiple times at different addresses. In this case, we don't
    // need to check both against each other since they would have resolved the same way at compile
    // time.
    std::unordered_set<std::string> unique_locations;
    for (const std::unique_ptr<const OatFile>& loaded_oat_file : oat_files_) {
      DCHECK_NE(loaded_oat_file.get(), oat_file);
      const std::string& location = loaded_oat_file->GetLocation();
      if (std::find(boot_oat_files.begin(), boot_oat_files.end(), loaded_oat_file.get()) ==
          boot_oat_files.end() && location != oat_file->GetLocation() &&
          unique_locations.find(location) == unique_locations.end()) {
        unique_locations.insert(location);
        AddDexFilesFromOat(loaded_oat_file.get(),
                           /*already_loaded*/true,
                           &queue,
                           /*out*/&opened_dex_files);
      }
    }
  }

  // Exit if shared libraries are ok. Do a full duplicate classes check otherwise.
  const std::string
      shared_libraries(oat_file->GetOatHeader().GetStoreValueByKey(OatHeader::kClassPathKey));
  if (AreSharedLibrariesOk(shared_libraries, queue)) {
    return false;
  }

  // Add dex files from the oat file to check.
  AddDexFilesFromOat(oat_file, /*already_loaded*/false, &queue, &opened_dex_files);

  // Now drain the queue.
  while (!queue.empty()) {
    // Modifying the top element is only safe if we pop right after.
    DexFileAndClassPair compare_pop(queue.top());
    queue.pop();

    // Compare against the following elements.
    while (!queue.empty()) {
      DexFileAndClassPair top(queue.top());

      if (strcmp(compare_pop.GetCachedDescriptor(), top.GetCachedDescriptor()) == 0) {
        // Same descriptor. Check whether it's crossing old-oat-files to new-oat-files.
        if (compare_pop.FromLoadedOat() != top.FromLoadedOat()) {
          *error_msg =
              StringPrintf("Found duplicated class when checking oat files: '%s' in %s and %s",
                           compare_pop.GetCachedDescriptor(),
                           compare_pop.GetDexFile()->GetLocation().c_str(),
                           top.GetDexFile()->GetLocation().c_str());
          return true;
        }
        queue.pop();
        AddNext(&top, &queue);
      } else {
        // Something else. Done here.
        break;
      }
    }
    AddNext(&compare_pop, &queue);
  }

  return false;
}

std::vector<std::unique_ptr<const DexFile>> OatFileManager::OpenDexFilesFromOat(
    const char* dex_location,
    const char* oat_location,
    jobject class_loader,
    jobjectArray dex_elements,
    const OatFile** out_oat_file,
    std::vector<std::string>* error_msgs) {
  ScopedTrace trace(__FUNCTION__);
  CHECK(dex_location != nullptr);
  CHECK(error_msgs != nullptr);

  // Verify we aren't holding the mutator lock, which could starve GC if we
  // have to generate or relocate an oat file.
  Thread* const self = Thread::Current();
  Locks::mutator_lock_->AssertNotHeld(self);
  Runtime* const runtime = Runtime::Current();

  OatFileAssistant oat_file_assistant(dex_location,
                                      oat_location,
                                      kRuntimeISA,
                                      /*profile_changed*/false,
                                      !runtime->IsAotCompiler());

  // Lock the target oat location to avoid races generating and loading the
  // oat file.
  std::string error_msg;
  if (!oat_file_assistant.Lock(/*out*/&error_msg)) {
    // Don't worry too much if this fails. If it does fail, it's unlikely we
    // can generate an oat file anyway.
    VLOG(class_linker) << "OatFileAssistant::Lock: " << error_msg;
  }

  const OatFile* source_oat_file = nullptr;

  if (!oat_file_assistant.IsUpToDate()) {
    // Update the oat file on disk if we can. This may fail, but that's okay.
    // Best effort is all that matters here.
    switch (oat_file_assistant.MakeUpToDate(filter_, /*out*/ &error_msg)) {
      case OatFileAssistant::kUpdateFailed:
        LOG(WARNING) << error_msg;
        break;

      case OatFileAssistant::kUpdateNotAttempted:
        // Avoid spamming the logs if we decided not to attempt making the oat
        // file up to date.
        VLOG(oat) << error_msg;
        break;

      case OatFileAssistant::kUpdateSucceeded:
        // Nothing to do.
        break;
    }
  }

  // Get the oat file on disk.
  std::unique_ptr<const OatFile> oat_file(oat_file_assistant.GetBestOatFile().release());

  if (oat_file != nullptr) {
    // Take the file only if it has no collisions, or we must take it because of preopting.
    bool accept_oat_file =
        !HasCollisions(oat_file.get(), class_loader, dex_elements, /*out*/ &error_msg);
    if (!accept_oat_file) {
      // Failed the collision check. Print warning.
      if (Runtime::Current()->IsDexFileFallbackEnabled()) {
        LOG(WARNING) << "Found duplicate classes, falling back to interpreter mode for "
                     << dex_location;
      } else {
        LOG(WARNING) << "Found duplicate classes, dex-file-fallback disabled, will be failing to "
                        " load classes for " << dex_location;
      }
      LOG(WARNING) << error_msg;

      // However, if the app was part of /system and preopted, there is no original dex file
      // available. In that case grudgingly accept the oat file.
      if (!oat_file_assistant.HasOriginalDexFiles()) {
        accept_oat_file = true;
        LOG(WARNING) << "Dex location " << dex_location << " does not seem to include dex file. "
                     << "Allow oat file use. This is potentially dangerous.";
      }
    }

    if (accept_oat_file) {
      VLOG(class_linker) << "Registering " << oat_file->GetLocation();
      source_oat_file = RegisterOatFile(std::move(oat_file));
      *out_oat_file = source_oat_file;
    }
  }

  std::vector<std::unique_ptr<const DexFile>> dex_files;

  // Load the dex files from the oat file.
  if (source_oat_file != nullptr) {
    bool added_image_space = false;
    if (source_oat_file->IsExecutable()) {
      std::unique_ptr<gc::space::ImageSpace> image_space(
          kEnableAppImage ? oat_file_assistant.OpenImageSpace(source_oat_file) : nullptr);
      if (image_space != nullptr) {
        ScopedObjectAccess soa(self);
        StackHandleScope<1> hs(self);
        Handle<mirror::ClassLoader> h_loader(
            hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader)));
        // Can not load app image without class loader.
        if (h_loader.Get() != nullptr) {
          std::string temp_error_msg;
          // Add image space has a race condition since other threads could be reading from the
          // spaces array.
          {
            ScopedThreadSuspension sts(self, kSuspended);
            gc::ScopedGCCriticalSection gcs(self,
                                            gc::kGcCauseAddRemoveAppImageSpace,
                                            gc::kCollectorTypeAddRemoveAppImageSpace);
            ScopedSuspendAll ssa("Add image space");
            runtime->GetHeap()->AddSpace(image_space.get());
          }
          {
            ScopedTrace trace2(StringPrintf("Adding image space for location %s", dex_location));
            added_image_space = runtime->GetClassLinker()->AddImageSpace(image_space.get(),
                                                                         h_loader,
                                                                         dex_elements,
                                                                         dex_location,
                                                                         /*out*/&dex_files,
                                                                         /*out*/&temp_error_msg);
          }
          if (added_image_space) {
            // Successfully added image space to heap, release the map so that it does not get
            // freed.
            image_space.release();
          } else {
            LOG(INFO) << "Failed to add image file " << temp_error_msg;
            dex_files.clear();
            {
              ScopedThreadSuspension sts(self, kSuspended);
              gc::ScopedGCCriticalSection gcs(self,
                                              gc::kGcCauseAddRemoveAppImageSpace,
                                              gc::kCollectorTypeAddRemoveAppImageSpace);
              ScopedSuspendAll ssa("Remove image space");
              runtime->GetHeap()->RemoveSpace(image_space.get());
            }
            // Non-fatal, don't update error_msg.
          }
        }
      }
    }
    if (!added_image_space) {
      DCHECK(dex_files.empty());
      dex_files = oat_file_assistant.LoadDexFiles(*source_oat_file, dex_location);
    }
    if (dex_files.empty()) {
      error_msgs->push_back("Failed to open dex files from " + source_oat_file->GetLocation());
    }
  }

  // Fall back to running out of the original dex file if we couldn't load any
  // dex_files from the oat file.
  if (dex_files.empty()) {
    if (oat_file_assistant.HasOriginalDexFiles()) {
      if (Runtime::Current()->IsDexFileFallbackEnabled()) {
        if (!DexFile::Open(dex_location, dex_location, /*out*/ &error_msg, &dex_files)) {
          LOG(WARNING) << error_msg;
          error_msgs->push_back("Failed to open dex files from " + std::string(dex_location)
                                + " because: " + error_msg);
        }
      } else {
        error_msgs->push_back("Fallback mode disabled, skipping dex files.");
      }
    } else {
      error_msgs->push_back("No original dex files found for dex location "
          + std::string(dex_location));
    }
  }

  // TODO(calin): Consider optimizing this knowing that is useless to record the
  // use of fully compiled apks.
  Runtime::Current()->NotifyDexLoaded(dex_location);
  return dex_files;
}

void OatFileManager::DumpForSigQuit(std::ostream& os) {
  ReaderMutexLock mu(Thread::Current(), *Locks::oat_file_manager_lock_);
  std::vector<const OatFile*> boot_oat_files = GetBootOatFiles();
  for (const std::unique_ptr<const OatFile>& oat_file : oat_files_) {
    if (ContainsElement(boot_oat_files, oat_file.get())) {
      continue;
    }
    os << oat_file->GetLocation() << ": " << oat_file->GetCompilerFilter() << "\n";
  }
}

}  // namespace art
