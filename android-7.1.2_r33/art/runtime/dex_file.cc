/*
 * Copyright (C) 2011 The Android Open Source Project
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

#include "dex_file.h"

#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>

#include <memory>
#include <sstream>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/file_magic.h"
#include "base/hash_map.h"
#include "base/logging.h"
#include "base/stl_util.h"
#include "base/stringprintf.h"
#include "base/systrace.h"
#include "class_linker-inl.h"
#include "dex_file-inl.h"
#include "dex_file_verifier.h"
#include "globals.h"
#include "handle_scope-inl.h"
#include "leb128.h"
#include "mirror/field.h"
#include "mirror/method.h"
#include "mirror/string.h"
#include "os.h"
#include "reflection.h"
#include "safe_map.h"
#include "thread.h"
#include "type_lookup_table.h"
#include "utf-inl.h"
#include "utils.h"
#include "well_known_classes.h"
#include "zip_archive.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wshadow"
#include "ScopedFd.h"
#pragma GCC diagnostic pop

namespace art {

const uint8_t DexFile::kDexMagic[] = { 'd', 'e', 'x', '\n' };
const uint8_t DexFile::kDexMagicVersions[DexFile::kNumDexVersions][DexFile::kDexVersionLen] = {
  {'0', '3', '5', '\0'},
  // Dex version 036 skipped because of an old dalvik bug on some versions of android where dex
  // files with that version number would erroneously be accepted and run.
  {'0', '3', '7', '\0'}
};

bool DexFile::GetChecksum(const char* filename, uint32_t* checksum, std::string* error_msg) {
  CHECK(checksum != nullptr);
  uint32_t magic;

  // Strip ":...", which is the location
  const char* zip_entry_name = kClassesDex;
  const char* file_part = filename;
  std::string file_part_storage;

  if (DexFile::IsMultiDexLocation(filename)) {
    file_part_storage = GetBaseLocation(filename);
    file_part = file_part_storage.c_str();
    zip_entry_name = filename + file_part_storage.size() + 1;
    DCHECK_EQ(zip_entry_name[-1], kMultiDexSeparator);
  }

  ScopedFd fd(OpenAndReadMagic(file_part, &magic, error_msg));
  if (fd.get() == -1) {
    DCHECK(!error_msg->empty());
    return false;
  }
  if (IsZipMagic(magic)) {
    std::unique_ptr<ZipArchive> zip_archive(
        ZipArchive::OpenFromFd(fd.release(), filename, error_msg));
    if (zip_archive.get() == nullptr) {
      *error_msg = StringPrintf("Failed to open zip archive '%s' (error msg: %s)", file_part,
                                error_msg->c_str());
      return false;
    }
    std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find(zip_entry_name, error_msg));
    if (zip_entry.get() == nullptr) {
      *error_msg = StringPrintf("Zip archive '%s' doesn't contain %s (error msg: %s)", file_part,
                                zip_entry_name, error_msg->c_str());
      return false;
    }
    *checksum = zip_entry->GetCrc32();
    return true;
  }
  if (IsDexMagic(magic)) {
    std::unique_ptr<const DexFile> dex_file(
        DexFile::OpenFile(fd.release(), filename, false, error_msg));
    if (dex_file.get() == nullptr) {
      return false;
    }
    *checksum = dex_file->GetHeader().checksum_;
    return true;
  }
  *error_msg = StringPrintf("Expected valid zip or dex file: '%s'", filename);
  return false;
}

bool DexFile::Open(const char* filename, const char* location, std::string* error_msg,
                   std::vector<std::unique_ptr<const DexFile>>* dex_files) {
  ScopedTrace trace(std::string("Open dex file ") + location);
  DCHECK(dex_files != nullptr) << "DexFile::Open: out-param is nullptr";
  uint32_t magic;
  ScopedFd fd(OpenAndReadMagic(filename, &magic, error_msg));
  if (fd.get() == -1) {
    DCHECK(!error_msg->empty());
    return false;
  }
  if (IsZipMagic(magic)) {
    return DexFile::OpenZip(fd.release(), location, error_msg, dex_files);
  }
  if (IsDexMagic(magic)) {
    std::unique_ptr<const DexFile> dex_file(DexFile::OpenFile(fd.release(), location, true,
                                                              error_msg));
    if (dex_file.get() != nullptr) {
      dex_files->push_back(std::move(dex_file));
      return true;
    } else {
      return false;
    }
  }
  *error_msg = StringPrintf("Expected valid zip or dex file: '%s'", filename);
  return false;
}

static bool ContainsClassesDex(int fd, const char* filename) {
  std::string error_msg;
  std::unique_ptr<ZipArchive> zip_archive(ZipArchive::OpenFromFd(fd, filename, &error_msg));
  if (zip_archive.get() == nullptr) {
    return false;
  }
  std::unique_ptr<ZipEntry> zip_entry(zip_archive->Find(DexFile::kClassesDex, &error_msg));
  return (zip_entry.get() != nullptr);
}

bool DexFile::MaybeDex(const char* filename) {
  uint32_t magic;
  std::string error_msg;
  ScopedFd fd(OpenAndReadMagic(filename, &magic, &error_msg));
  if (fd.get() == -1) {
    return false;
  }
  if (IsZipMagic(magic)) {
    return ContainsClassesDex(fd.release(), filename);
  } else if (IsDexMagic(magic)) {
    return true;
  }
  return false;
}

int DexFile::GetPermissions() const {
  if (mem_map_.get() == nullptr) {
    return 0;
  } else {
    return mem_map_->GetProtect();
  }
}

bool DexFile::IsReadOnly() const {
  return GetPermissions() == PROT_READ;
}

bool DexFile::EnableWrite() const {
  CHECK(IsReadOnly());
  if (mem_map_.get() == nullptr) {
    return false;
  } else {
    return mem_map_->Protect(PROT_READ | PROT_WRITE);
  }
}

bool DexFile::DisableWrite() const {
  CHECK(!IsReadOnly());
  if (mem_map_.get() == nullptr) {
    return false;
  } else {
    return mem_map_->Protect(PROT_READ);
  }
}

std::unique_ptr<const DexFile> DexFile::Open(const uint8_t* base, size_t size,
                                             const std::string& location,
                                             uint32_t location_checksum,
                                             const OatDexFile* oat_dex_file,
                                             bool verify,
                                             std::string* error_msg) {
  ScopedTrace trace(std::string("Open dex file from RAM ") + location);
  std::unique_ptr<const DexFile> dex_file = OpenMemory(base,
                                                       size,
                                                       location,
                                                       location_checksum,
                                                       nullptr,
                                                       oat_dex_file,
                                                       error_msg);
  if (verify && !DexFileVerifier::Verify(dex_file.get(),
                                         dex_file->Begin(),
                                         dex_file->Size(),
                                         location.c_str(),
                                         error_msg)) {
    return nullptr;
  }

  return dex_file;
}

std::unique_ptr<const DexFile> DexFile::OpenFile(int fd, const char* location, bool verify,
                                                 std::string* error_msg) {
  ScopedTrace trace(std::string("Open dex file ") + location);
  CHECK(location != nullptr);
  std::unique_ptr<MemMap> map;
  {
    ScopedFd delayed_close(fd);
    struct stat sbuf;
    memset(&sbuf, 0, sizeof(sbuf));
    if (fstat(fd, &sbuf) == -1) {
      *error_msg = StringPrintf("DexFile: fstat '%s' failed: %s", location, strerror(errno));
      return nullptr;
    }
    if (S_ISDIR(sbuf.st_mode)) {
      *error_msg = StringPrintf("Attempt to mmap directory '%s'", location);
      return nullptr;
    }
    size_t length = sbuf.st_size;
    map.reset(MemMap::MapFile(length,
                              PROT_READ,
                              MAP_PRIVATE,
                              fd,
                              0,
                              /*low_4gb*/false,
                              location,
                              error_msg));
    if (map.get() == nullptr) {
      DCHECK(!error_msg->empty());
      return nullptr;
    }
  }

  if (map->Size() < sizeof(DexFile::Header)) {
    *error_msg = StringPrintf(
        "DexFile: failed to open dex file '%s' that is too short to have a header", location);
    return nullptr;
  }

  const Header* dex_header = reinterpret_cast<const Header*>(map->Begin());

  std::unique_ptr<const DexFile> dex_file(OpenMemory(location, dex_header->checksum_, map.release(),
                                                     error_msg));
  if (dex_file.get() == nullptr) {
    *error_msg = StringPrintf("Failed to open dex file '%s' from memory: %s", location,
                              error_msg->c_str());
    return nullptr;
  }

  if (verify && !DexFileVerifier::Verify(dex_file.get(), dex_file->Begin(), dex_file->Size(),
                                         location, error_msg)) {
    return nullptr;
  }

  return dex_file;
}

const char* DexFile::kClassesDex = "classes.dex";

bool DexFile::OpenZip(int fd, const std::string& location, std::string* error_msg,
                      std::vector<std::unique_ptr<const DexFile>>* dex_files) {
  ScopedTrace trace("Dex file open Zip " + std::string(location));
  DCHECK(dex_files != nullptr) << "DexFile::OpenZip: out-param is nullptr";
  std::unique_ptr<ZipArchive> zip_archive(ZipArchive::OpenFromFd(fd, location.c_str(), error_msg));
  if (zip_archive.get() == nullptr) {
    DCHECK(!error_msg->empty());
    return false;
  }
  return DexFile::OpenFromZip(*zip_archive, location, error_msg, dex_files);
}

std::unique_ptr<const DexFile> DexFile::OpenMemory(const std::string& location,
                                                   uint32_t location_checksum,
                                                   MemMap* mem_map,
                                                   std::string* error_msg) {
  return OpenMemory(mem_map->Begin(),
                    mem_map->Size(),
                    location,
                    location_checksum,
                    mem_map,
                    nullptr,
                    error_msg);
}

std::unique_ptr<const DexFile> DexFile::Open(const ZipArchive& zip_archive, const char* entry_name,
                                             const std::string& location, std::string* error_msg,
                                             ZipOpenErrorCode* error_code) {
  ScopedTrace trace("Dex file open from Zip Archive " + std::string(location));
  CHECK(!location.empty());
  std::unique_ptr<ZipEntry> zip_entry(zip_archive.Find(entry_name, error_msg));
  if (zip_entry.get() == nullptr) {
    *error_code = ZipOpenErrorCode::kEntryNotFound;
    return nullptr;
  }
  std::unique_ptr<MemMap> map(zip_entry->ExtractToMemMap(location.c_str(), entry_name, error_msg));
  if (map.get() == nullptr) {
    *error_msg = StringPrintf("Failed to extract '%s' from '%s': %s", entry_name, location.c_str(),
                              error_msg->c_str());
    *error_code = ZipOpenErrorCode::kExtractToMemoryError;
    return nullptr;
  }
  std::unique_ptr<const DexFile> dex_file(OpenMemory(location, zip_entry->GetCrc32(), map.release(),
                                               error_msg));
  if (dex_file.get() == nullptr) {
    *error_msg = StringPrintf("Failed to open dex file '%s' from memory: %s", location.c_str(),
                              error_msg->c_str());
    *error_code = ZipOpenErrorCode::kDexFileError;
    return nullptr;
  }
  if (!dex_file->DisableWrite()) {
    *error_msg = StringPrintf("Failed to make dex file '%s' read only", location.c_str());
    *error_code = ZipOpenErrorCode::kMakeReadOnlyError;
    return nullptr;
  }
  CHECK(dex_file->IsReadOnly()) << location;
  if (!DexFileVerifier::Verify(dex_file.get(), dex_file->Begin(), dex_file->Size(),
                               location.c_str(), error_msg)) {
    *error_code = ZipOpenErrorCode::kVerifyError;
    return nullptr;
  }
  *error_code = ZipOpenErrorCode::kNoError;
  return dex_file;
}

// Technically we do not have a limitation with respect to the number of dex files that can be in a
// multidex APK. However, it's bad practice, as each dex file requires its own tables for symbols
// (types, classes, methods, ...) and dex caches. So warn the user that we open a zip with what
// seems an excessive number.
static constexpr size_t kWarnOnManyDexFilesThreshold = 100;

bool DexFile::OpenFromZip(const ZipArchive& zip_archive, const std::string& location,
                          std::string* error_msg,
                          std::vector<std::unique_ptr<const DexFile>>* dex_files) {
  ScopedTrace trace("Dex file open from Zip " + std::string(location));
  DCHECK(dex_files != nullptr) << "DexFile::OpenFromZip: out-param is nullptr";
  ZipOpenErrorCode error_code;
  std::unique_ptr<const DexFile> dex_file(Open(zip_archive, kClassesDex, location, error_msg,
                                               &error_code));
  if (dex_file.get() == nullptr) {
    return false;
  } else {
    // Had at least classes.dex.
    dex_files->push_back(std::move(dex_file));

    // Now try some more.

    // We could try to avoid std::string allocations by working on a char array directly. As we
    // do not expect a lot of iterations, this seems too involved and brittle.

    for (size_t i = 1; ; ++i) {
      std::string name = GetMultiDexClassesDexName(i);
      std::string fake_location = GetMultiDexLocation(i, location.c_str());
      std::unique_ptr<const DexFile> next_dex_file(Open(zip_archive, name.c_str(), fake_location,
                                                        error_msg, &error_code));
      if (next_dex_file.get() == nullptr) {
        if (error_code != ZipOpenErrorCode::kEntryNotFound) {
          LOG(WARNING) << error_msg;
        }
        break;
      } else {
        dex_files->push_back(std::move(next_dex_file));
      }

      if (i == kWarnOnManyDexFilesThreshold) {
        LOG(WARNING) << location << " has in excess of " << kWarnOnManyDexFilesThreshold
                     << " dex files. Please consider coalescing and shrinking the number to "
                        " avoid runtime overhead.";
      }

      if (i == std::numeric_limits<size_t>::max()) {
        LOG(ERROR) << "Overflow in number of dex files!";
        break;
      }
    }

    return true;
  }
}


std::unique_ptr<const DexFile> DexFile::OpenMemory(const uint8_t* base,
                                                   size_t size,
                                                   const std::string& location,
                                                   uint32_t location_checksum,
                                                   MemMap* mem_map,
                                                   const OatDexFile* oat_dex_file,
                                                   std::string* error_msg) {
  CHECK_ALIGNED(base, 4);  // various dex file structures must be word aligned
  std::unique_ptr<DexFile> dex_file(
      new DexFile(base, size, location, location_checksum, mem_map, oat_dex_file));
  if (!dex_file->Init(error_msg)) {
    dex_file.reset();
  }
  return std::unique_ptr<const DexFile>(dex_file.release());
}

DexFile::DexFile(const uint8_t* base, size_t size,
                 const std::string& location,
                 uint32_t location_checksum,
                 MemMap* mem_map,
                 const OatDexFile* oat_dex_file)
    : begin_(base),
      size_(size),
      location_(location),
      location_checksum_(location_checksum),
      mem_map_(mem_map),
      header_(reinterpret_cast<const Header*>(base)),
      string_ids_(reinterpret_cast<const StringId*>(base + header_->string_ids_off_)),
      type_ids_(reinterpret_cast<const TypeId*>(base + header_->type_ids_off_)),
      field_ids_(reinterpret_cast<const FieldId*>(base + header_->field_ids_off_)),
      method_ids_(reinterpret_cast<const MethodId*>(base + header_->method_ids_off_)),
      proto_ids_(reinterpret_cast<const ProtoId*>(base + header_->proto_ids_off_)),
      class_defs_(reinterpret_cast<const ClassDef*>(base + header_->class_defs_off_)),
      oat_dex_file_(oat_dex_file) {
  CHECK(begin_ != nullptr) << GetLocation();
  CHECK_GT(size_, 0U) << GetLocation();
  const uint8_t* lookup_data = (oat_dex_file != nullptr)
      ? oat_dex_file->GetLookupTableData()
      : nullptr;
  if (lookup_data != nullptr) {
    if (lookup_data + TypeLookupTable::RawDataLength(*this) > oat_dex_file->GetOatFile()->End()) {
      LOG(WARNING) << "found truncated lookup table in " << GetLocation();
    } else {
      lookup_table_.reset(TypeLookupTable::Open(lookup_data, *this));
    }
  }
}

DexFile::~DexFile() {
  // We don't call DeleteGlobalRef on dex_object_ because we're only called by DestroyJavaVM, and
  // that's only called after DetachCurrentThread, which means there's no JNIEnv. We could
  // re-attach, but cleaning up these global references is not obviously useful. It's not as if
  // the global reference table is otherwise empty!
}

bool DexFile::Init(std::string* error_msg) {
  if (!CheckMagicAndVersion(error_msg)) {
    return false;
  }
  return true;
}

bool DexFile::CheckMagicAndVersion(std::string* error_msg) const {
  if (!IsMagicValid(header_->magic_)) {
    std::ostringstream oss;
    oss << "Unrecognized magic number in "  << GetLocation() << ":"
            << " " << header_->magic_[0]
            << " " << header_->magic_[1]
            << " " << header_->magic_[2]
            << " " << header_->magic_[3];
    *error_msg = oss.str();
    return false;
  }
  if (!IsVersionValid(header_->magic_)) {
    std::ostringstream oss;
    oss << "Unrecognized version number in "  << GetLocation() << ":"
            << " " << header_->magic_[4]
            << " " << header_->magic_[5]
            << " " << header_->magic_[6]
            << " " << header_->magic_[7];
    *error_msg = oss.str();
    return false;
  }
  return true;
}

bool DexFile::IsMagicValid(const uint8_t* magic) {
  return (memcmp(magic, kDexMagic, sizeof(kDexMagic)) == 0);
}

bool DexFile::IsVersionValid(const uint8_t* magic) {
  const uint8_t* version = &magic[sizeof(kDexMagic)];
  for (uint32_t i = 0; i < kNumDexVersions; i++) {
    if (memcmp(version, kDexMagicVersions[i], kDexVersionLen) == 0) {
      return true;
    }
  }
  return false;
}

uint32_t DexFile::Header::GetVersion() const {
  const char* version = reinterpret_cast<const char*>(&magic_[sizeof(kDexMagic)]);
  return atoi(version);
}

const DexFile::ClassDef* DexFile::FindClassDef(const char* descriptor, size_t hash) const {
  DCHECK_EQ(ComputeModifiedUtf8Hash(descriptor), hash);
  if (LIKELY(lookup_table_ != nullptr)) {
    const uint32_t class_def_idx = lookup_table_->Lookup(descriptor, hash);
    return (class_def_idx != DexFile::kDexNoIndex) ? &GetClassDef(class_def_idx) : nullptr;
  }

  // Fast path for rate no class defs case.
  const uint32_t num_class_defs = NumClassDefs();
  if (num_class_defs == 0) {
    return nullptr;
  }
  const TypeId* type_id = FindTypeId(descriptor);
  if (type_id != nullptr) {
    uint16_t type_idx = GetIndexForTypeId(*type_id);
    for (size_t i = 0; i < num_class_defs; ++i) {
      const ClassDef& class_def = GetClassDef(i);
      if (class_def.class_idx_ == type_idx) {
        return &class_def;
      }
    }
  }
  return nullptr;
}

const DexFile::ClassDef* DexFile::FindClassDef(uint16_t type_idx) const {
  size_t num_class_defs = NumClassDefs();
  for (size_t i = 0; i < num_class_defs; ++i) {
    const ClassDef& class_def = GetClassDef(i);
    if (class_def.class_idx_ == type_idx) {
      return &class_def;
    }
  }
  return nullptr;
}

const DexFile::FieldId* DexFile::FindFieldId(const DexFile::TypeId& declaring_klass,
                                              const DexFile::StringId& name,
                                              const DexFile::TypeId& type) const {
  // Binary search MethodIds knowing that they are sorted by class_idx, name_idx then proto_idx
  const uint16_t class_idx = GetIndexForTypeId(declaring_klass);
  const uint32_t name_idx = GetIndexForStringId(name);
  const uint16_t type_idx = GetIndexForTypeId(type);
  int32_t lo = 0;
  int32_t hi = NumFieldIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::FieldId& field = GetFieldId(mid);
    if (class_idx > field.class_idx_) {
      lo = mid + 1;
    } else if (class_idx < field.class_idx_) {
      hi = mid - 1;
    } else {
      if (name_idx > field.name_idx_) {
        lo = mid + 1;
      } else if (name_idx < field.name_idx_) {
        hi = mid - 1;
      } else {
        if (type_idx > field.type_idx_) {
          lo = mid + 1;
        } else if (type_idx < field.type_idx_) {
          hi = mid - 1;
        } else {
          return &field;
        }
      }
    }
  }
  return nullptr;
}

const DexFile::MethodId* DexFile::FindMethodId(const DexFile::TypeId& declaring_klass,
                                               const DexFile::StringId& name,
                                               const DexFile::ProtoId& signature) const {
  // Binary search MethodIds knowing that they are sorted by class_idx, name_idx then proto_idx
  const uint16_t class_idx = GetIndexForTypeId(declaring_klass);
  const uint32_t name_idx = GetIndexForStringId(name);
  const uint16_t proto_idx = GetIndexForProtoId(signature);
  int32_t lo = 0;
  int32_t hi = NumMethodIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::MethodId& method = GetMethodId(mid);
    if (class_idx > method.class_idx_) {
      lo = mid + 1;
    } else if (class_idx < method.class_idx_) {
      hi = mid - 1;
    } else {
      if (name_idx > method.name_idx_) {
        lo = mid + 1;
      } else if (name_idx < method.name_idx_) {
        hi = mid - 1;
      } else {
        if (proto_idx > method.proto_idx_) {
          lo = mid + 1;
        } else if (proto_idx < method.proto_idx_) {
          hi = mid - 1;
        } else {
          return &method;
        }
      }
    }
  }
  return nullptr;
}

const DexFile::StringId* DexFile::FindStringId(const char* string) const {
  int32_t lo = 0;
  int32_t hi = NumStringIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::StringId& str_id = GetStringId(mid);
    const char* str = GetStringData(str_id);
    int compare = CompareModifiedUtf8ToModifiedUtf8AsUtf16CodePointValues(string, str);
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &str_id;
    }
  }
  return nullptr;
}

const DexFile::TypeId* DexFile::FindTypeId(const char* string) const {
  int32_t lo = 0;
  int32_t hi = NumTypeIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const TypeId& type_id = GetTypeId(mid);
    const DexFile::StringId& str_id = GetStringId(type_id.descriptor_idx_);
    const char* str = GetStringData(str_id);
    int compare = CompareModifiedUtf8ToModifiedUtf8AsUtf16CodePointValues(string, str);
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &type_id;
    }
  }
  return nullptr;
}

const DexFile::StringId* DexFile::FindStringId(const uint16_t* string, size_t length) const {
  int32_t lo = 0;
  int32_t hi = NumStringIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::StringId& str_id = GetStringId(mid);
    const char* str = GetStringData(str_id);
    int compare = CompareModifiedUtf8ToUtf16AsCodePointValues(str, string, length);
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &str_id;
    }
  }
  return nullptr;
}

const DexFile::TypeId* DexFile::FindTypeId(uint32_t string_idx) const {
  int32_t lo = 0;
  int32_t hi = NumTypeIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const TypeId& type_id = GetTypeId(mid);
    if (string_idx > type_id.descriptor_idx_) {
      lo = mid + 1;
    } else if (string_idx < type_id.descriptor_idx_) {
      hi = mid - 1;
    } else {
      return &type_id;
    }
  }
  return nullptr;
}

const DexFile::ProtoId* DexFile::FindProtoId(uint16_t return_type_idx,
                                             const uint16_t* signature_type_idxs,
                                             uint32_t signature_length) const {
  int32_t lo = 0;
  int32_t hi = NumProtoIds() - 1;
  while (hi >= lo) {
    int32_t mid = (hi + lo) / 2;
    const DexFile::ProtoId& proto = GetProtoId(mid);
    int compare = return_type_idx - proto.return_type_idx_;
    if (compare == 0) {
      DexFileParameterIterator it(*this, proto);
      size_t i = 0;
      while (it.HasNext() && i < signature_length && compare == 0) {
        compare = signature_type_idxs[i] - it.GetTypeIdx();
        it.Next();
        i++;
      }
      if (compare == 0) {
        if (it.HasNext()) {
          compare = -1;
        } else if (i < signature_length) {
          compare = 1;
        }
      }
    }
    if (compare > 0) {
      lo = mid + 1;
    } else if (compare < 0) {
      hi = mid - 1;
    } else {
      return &proto;
    }
  }
  return nullptr;
}

void DexFile::CreateTypeLookupTable(uint8_t* storage) const {
  lookup_table_.reset(TypeLookupTable::Create(*this, storage));
}

// Given a signature place the type ids into the given vector
bool DexFile::CreateTypeList(const StringPiece& signature, uint16_t* return_type_idx,
                             std::vector<uint16_t>* param_type_idxs) const {
  if (signature[0] != '(') {
    return false;
  }
  size_t offset = 1;
  size_t end = signature.size();
  bool process_return = false;
  while (offset < end) {
    size_t start_offset = offset;
    char c = signature[offset];
    offset++;
    if (c == ')') {
      process_return = true;
      continue;
    }
    while (c == '[') {  // process array prefix
      if (offset >= end) {  // expect some descriptor following [
        return false;
      }
      c = signature[offset];
      offset++;
    }
    if (c == 'L') {  // process type descriptors
      do {
        if (offset >= end) {  // unexpected early termination of descriptor
          return false;
        }
        c = signature[offset];
        offset++;
      } while (c != ';');
    }
    // TODO: avoid creating a std::string just to get a 0-terminated char array
    std::string descriptor(signature.data() + start_offset, offset - start_offset);
    const DexFile::TypeId* type_id = FindTypeId(descriptor.c_str());
    if (type_id == nullptr) {
      return false;
    }
    uint16_t type_idx = GetIndexForTypeId(*type_id);
    if (!process_return) {
      param_type_idxs->push_back(type_idx);
    } else {
      *return_type_idx = type_idx;
      return offset == end;  // return true if the signature had reached a sensible end
    }
  }
  return false;  // failed to correctly parse return type
}

const Signature DexFile::CreateSignature(const StringPiece& signature) const {
  uint16_t return_type_idx;
  std::vector<uint16_t> param_type_indices;
  bool success = CreateTypeList(signature, &return_type_idx, &param_type_indices);
  if (!success) {
    return Signature::NoSignature();
  }
  const ProtoId* proto_id = FindProtoId(return_type_idx, param_type_indices);
  if (proto_id == nullptr) {
    return Signature::NoSignature();
  }
  return Signature(this, *proto_id);
}

int32_t DexFile::GetLineNumFromPC(ArtMethod* method, uint32_t rel_pc) const {
  // For native method, lineno should be -2 to indicate it is native. Note that
  // "line number == -2" is how libcore tells from StackTraceElement.
  if (method->GetCodeItemOffset() == 0) {
    return -2;
  }

  const CodeItem* code_item = GetCodeItem(method->GetCodeItemOffset());
  DCHECK(code_item != nullptr) << PrettyMethod(method) << " " << GetLocation();

  // A method with no line number info should return -1
  LineNumFromPcContext context(rel_pc, -1);
  DecodeDebugPositionInfo(code_item, LineNumForPcCb, &context);
  return context.line_num_;
}

int32_t DexFile::FindTryItem(const CodeItem &code_item, uint32_t address) {
  // Note: Signed type is important for max and min.
  int32_t min = 0;
  int32_t max = code_item.tries_size_ - 1;

  while (min <= max) {
    int32_t mid = min + ((max - min) / 2);

    const art::DexFile::TryItem* ti = GetTryItems(code_item, mid);
    uint32_t start = ti->start_addr_;
    uint32_t end = start + ti->insn_count_;

    if (address < start) {
      max = mid - 1;
    } else if (address >= end) {
      min = mid + 1;
    } else {  // We have a winner!
      return mid;
    }
  }
  // No match.
  return -1;
}

int32_t DexFile::FindCatchHandlerOffset(const CodeItem &code_item, uint32_t address) {
  int32_t try_item = FindTryItem(code_item, address);
  if (try_item == -1) {
    return -1;
  } else {
    return DexFile::GetTryItems(code_item, try_item)->handler_off_;
  }
}

bool DexFile::DecodeDebugLocalInfo(const CodeItem* code_item, bool is_static, uint32_t method_idx,
                                   DexDebugNewLocalCb local_cb, void* context) const {
  DCHECK(local_cb != nullptr);
  if (code_item == nullptr) {
    return false;
  }
  const uint8_t* stream = GetDebugInfoStream(code_item);
  if (stream == nullptr) {
    return false;
  }
  std::vector<LocalInfo> local_in_reg(code_item->registers_size_);

  uint16_t arg_reg = code_item->registers_size_ - code_item->ins_size_;
  if (!is_static) {
    const char* descriptor = GetMethodDeclaringClassDescriptor(GetMethodId(method_idx));
    local_in_reg[arg_reg].name_ = "this";
    local_in_reg[arg_reg].descriptor_ = descriptor;
    local_in_reg[arg_reg].signature_ = nullptr;
    local_in_reg[arg_reg].start_address_ = 0;
    local_in_reg[arg_reg].reg_ = arg_reg;
    local_in_reg[arg_reg].is_live_ = true;
    arg_reg++;
  }

  DexFileParameterIterator it(*this, GetMethodPrototype(GetMethodId(method_idx)));
  DecodeUnsignedLeb128(&stream);  // Line.
  uint32_t parameters_size = DecodeUnsignedLeb128(&stream);
  uint32_t i;
  for (i = 0; i < parameters_size && it.HasNext(); ++i, it.Next()) {
    if (arg_reg >= code_item->registers_size_) {
      LOG(ERROR) << "invalid stream - arg reg >= reg size (" << arg_reg
                 << " >= " << code_item->registers_size_ << ") in " << GetLocation();
      return false;
    }
    uint32_t name_idx = DecodeUnsignedLeb128P1(&stream);
    const char* descriptor = it.GetDescriptor();
    local_in_reg[arg_reg].name_ = StringDataByIdx(name_idx);
    local_in_reg[arg_reg].descriptor_ = descriptor;
    local_in_reg[arg_reg].signature_ = nullptr;
    local_in_reg[arg_reg].start_address_ = 0;
    local_in_reg[arg_reg].reg_ = arg_reg;
    local_in_reg[arg_reg].is_live_ = true;
    switch (*descriptor) {
      case 'D':
      case 'J':
        arg_reg += 2;
        break;
      default:
        arg_reg += 1;
        break;
    }
  }
  if (i != parameters_size || it.HasNext()) {
    LOG(ERROR) << "invalid stream - problem with parameter iterator in " << GetLocation()
               << " for method " << PrettyMethod(method_idx, *this);
    return false;
  }

  uint32_t address = 0;
  for (;;)  {
    uint8_t opcode = *stream++;
    switch (opcode) {
      case DBG_END_SEQUENCE:
        // Emit all variables which are still alive at the end of the method.
        for (uint16_t reg = 0; reg < code_item->registers_size_; reg++) {
          if (local_in_reg[reg].is_live_) {
            local_in_reg[reg].end_address_ = code_item->insns_size_in_code_units_;
            local_cb(context, local_in_reg[reg]);
          }
        }
        return true;
      case DBG_ADVANCE_PC:
        address += DecodeUnsignedLeb128(&stream);
        break;
      case DBG_ADVANCE_LINE:
        DecodeSignedLeb128(&stream);  // Line.
        break;
      case DBG_START_LOCAL:
      case DBG_START_LOCAL_EXTENDED: {
        uint16_t reg = DecodeUnsignedLeb128(&stream);
        if (reg >= code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg >= reg size (" << reg << " >= "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return false;
        }

        uint32_t name_idx = DecodeUnsignedLeb128P1(&stream);
        uint32_t descriptor_idx = DecodeUnsignedLeb128P1(&stream);
        uint32_t signature_idx = kDexNoIndex;
        if (opcode == DBG_START_LOCAL_EXTENDED) {
          signature_idx = DecodeUnsignedLeb128P1(&stream);
        }

        // Emit what was previously there, if anything
        if (local_in_reg[reg].is_live_) {
          local_in_reg[reg].end_address_ = address;
          local_cb(context, local_in_reg[reg]);
        }

        local_in_reg[reg].name_ = StringDataByIdx(name_idx);
        local_in_reg[reg].descriptor_ = StringByTypeIdx(descriptor_idx);
        local_in_reg[reg].signature_ = StringDataByIdx(signature_idx);
        local_in_reg[reg].start_address_ = address;
        local_in_reg[reg].reg_ = reg;
        local_in_reg[reg].is_live_ = true;
        break;
      }
      case DBG_END_LOCAL: {
        uint16_t reg = DecodeUnsignedLeb128(&stream);
        if (reg >= code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg >= reg size (" << reg << " >= "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return false;
        }
        if (!local_in_reg[reg].is_live_) {
          LOG(ERROR) << "invalid stream - end without start in " << GetLocation();
          return false;
        }
        local_in_reg[reg].end_address_ = address;
        local_cb(context, local_in_reg[reg]);
        local_in_reg[reg].is_live_ = false;
        break;
      }
      case DBG_RESTART_LOCAL: {
        uint16_t reg = DecodeUnsignedLeb128(&stream);
        if (reg >= code_item->registers_size_) {
          LOG(ERROR) << "invalid stream - reg >= reg size (" << reg << " >= "
                     << code_item->registers_size_ << ") in " << GetLocation();
          return false;
        }
        // If the register is live, the "restart" is superfluous,
        // and we don't want to mess with the existing start address.
        if (!local_in_reg[reg].is_live_) {
          local_in_reg[reg].start_address_ = address;
          local_in_reg[reg].is_live_ = true;
        }
        break;
      }
      case DBG_SET_PROLOGUE_END:
      case DBG_SET_EPILOGUE_BEGIN:
        break;
      case DBG_SET_FILE:
        DecodeUnsignedLeb128P1(&stream);  // name.
        break;
      default:
        address += (opcode - DBG_FIRST_SPECIAL) / DBG_LINE_RANGE;
        break;
    }
  }
}

bool DexFile::DecodeDebugPositionInfo(const CodeItem* code_item, DexDebugNewPositionCb position_cb,
                                      void* context) const {
  DCHECK(position_cb != nullptr);
  if (code_item == nullptr) {
    return false;
  }
  const uint8_t* stream = GetDebugInfoStream(code_item);
  if (stream == nullptr) {
    return false;
  }

  PositionInfo entry = PositionInfo();
  entry.line_ = DecodeUnsignedLeb128(&stream);
  uint32_t parameters_size = DecodeUnsignedLeb128(&stream);
  for (uint32_t i = 0; i < parameters_size; ++i) {
    DecodeUnsignedLeb128P1(&stream);  // Parameter name.
  }

  for (;;)  {
    uint8_t opcode = *stream++;
    switch (opcode) {
      case DBG_END_SEQUENCE:
        return true;  // end of stream.
      case DBG_ADVANCE_PC:
        entry.address_ += DecodeUnsignedLeb128(&stream);
        break;
      case DBG_ADVANCE_LINE:
        entry.line_ += DecodeSignedLeb128(&stream);
        break;
      case DBG_START_LOCAL:
        DecodeUnsignedLeb128(&stream);  // reg.
        DecodeUnsignedLeb128P1(&stream);  // name.
        DecodeUnsignedLeb128P1(&stream);  // descriptor.
        break;
      case DBG_START_LOCAL_EXTENDED:
        DecodeUnsignedLeb128(&stream);  // reg.
        DecodeUnsignedLeb128P1(&stream);  // name.
        DecodeUnsignedLeb128P1(&stream);  // descriptor.
        DecodeUnsignedLeb128P1(&stream);  // signature.
        break;
      case DBG_END_LOCAL:
      case DBG_RESTART_LOCAL:
        DecodeUnsignedLeb128(&stream);  // reg.
        break;
      case DBG_SET_PROLOGUE_END:
        entry.prologue_end_ = true;
        break;
      case DBG_SET_EPILOGUE_BEGIN:
        entry.epilogue_begin_ = true;
        break;
      case DBG_SET_FILE: {
        uint32_t name_idx = DecodeUnsignedLeb128P1(&stream);
        entry.source_file_ = StringDataByIdx(name_idx);
        break;
      }
      default: {
        int adjopcode = opcode - DBG_FIRST_SPECIAL;
        entry.address_ += adjopcode / DBG_LINE_RANGE;
        entry.line_ += DBG_LINE_BASE + (adjopcode % DBG_LINE_RANGE);
        if (position_cb(context, entry)) {
          return true;  // early exit.
        }
        entry.prologue_end_ = false;
        entry.epilogue_begin_ = false;
        break;
      }
    }
  }
}

bool DexFile::LineNumForPcCb(void* raw_context, const PositionInfo& entry) {
  LineNumFromPcContext* context = reinterpret_cast<LineNumFromPcContext*>(raw_context);

  // We know that this callback will be called in
  // ascending address order, so keep going until we find
  // a match or we've just gone past it.
  if (entry.address_ > context->address_) {
    // The line number from the previous positions callback
    // wil be the final result.
    return true;
  } else {
    context->line_num_ = entry.line_;
    return entry.address_ == context->address_;
  }
}

bool DexFile::IsMultiDexLocation(const char* location) {
  return strrchr(location, kMultiDexSeparator) != nullptr;
}

std::string DexFile::GetMultiDexClassesDexName(size_t index) {
  if (index == 0) {
    return "classes.dex";
  } else {
    return StringPrintf("classes%zu.dex", index + 1);
  }
}

std::string DexFile::GetMultiDexLocation(size_t index, const char* dex_location) {
  if (index == 0) {
    return dex_location;
  } else {
    return StringPrintf("%s" kMultiDexSeparatorString "classes%zu.dex", dex_location, index + 1);
  }
}

std::string DexFile::GetDexCanonicalLocation(const char* dex_location) {
  CHECK_NE(dex_location, static_cast<const char*>(nullptr));
  std::string base_location = GetBaseLocation(dex_location);
  const char* suffix = dex_location + base_location.size();
  DCHECK(suffix[0] == 0 || suffix[0] == kMultiDexSeparator);
  UniqueCPtr<const char[]> path(realpath(base_location.c_str(), nullptr));
  if (path != nullptr && path.get() != base_location) {
    return std::string(path.get()) + suffix;
  } else if (suffix[0] == 0) {
    return base_location;
  } else {
    return dex_location;
  }
}

// Read a signed integer.  "zwidth" is the zero-based byte count.
static int32_t ReadSignedInt(const uint8_t* ptr, int zwidth) {
  int32_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = ((uint32_t)val >> 8) | (((int32_t)*ptr++) << 24);
  }
  val >>= (3 - zwidth) * 8;
  return val;
}

// Read an unsigned integer.  "zwidth" is the zero-based byte count,
// "fill_on_right" indicates which side we want to zero-fill from.
static uint32_t ReadUnsignedInt(const uint8_t* ptr, int zwidth, bool fill_on_right) {
  uint32_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = (val >> 8) | (((uint32_t)*ptr++) << 24);
  }
  if (!fill_on_right) {
    val >>= (3 - zwidth) * 8;
  }
  return val;
}

// Read a signed long.  "zwidth" is the zero-based byte count.
static int64_t ReadSignedLong(const uint8_t* ptr, int zwidth) {
  int64_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = ((uint64_t)val >> 8) | (((int64_t)*ptr++) << 56);
  }
  val >>= (7 - zwidth) * 8;
  return val;
}

// Read an unsigned long.  "zwidth" is the zero-based byte count,
// "fill_on_right" indicates which side we want to zero-fill from.
static uint64_t ReadUnsignedLong(const uint8_t* ptr, int zwidth, bool fill_on_right) {
  uint64_t val = 0;
  for (int i = zwidth; i >= 0; --i) {
    val = (val >> 8) | (((uint64_t)*ptr++) << 56);
  }
  if (!fill_on_right) {
    val >>= (7 - zwidth) * 8;
  }
  return val;
}

// Checks that visibility is as expected. Includes special behavior for M and
// before to allow runtime and build visibility when expecting runtime.
static bool IsVisibilityCompatible(uint32_t actual, uint32_t expected) {
  if (expected == DexFile::kDexVisibilityRuntime) {
    int32_t sdk_version = Runtime::Current()->GetTargetSdkVersion();
    if (sdk_version > 0 && sdk_version <= 23) {
      return actual == DexFile::kDexVisibilityRuntime || actual == DexFile::kDexVisibilityBuild;
    }
  }
  return actual == expected;
}

const DexFile::AnnotationSetItem* DexFile::FindAnnotationSetForField(ArtField* field) const {
  mirror::Class* klass = field->GetDeclaringClass();
  const AnnotationsDirectoryItem* annotations_dir = GetAnnotationsDirectory(*klass->GetClassDef());
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const FieldAnnotationsItem* field_annotations = GetFieldAnnotations(annotations_dir);
  if (field_annotations == nullptr) {
    return nullptr;
  }
  uint32_t field_index = field->GetDexFieldIndex();
  uint32_t field_count = annotations_dir->fields_size_;
  for (uint32_t i = 0; i < field_count; ++i) {
    if (field_annotations[i].field_idx_ == field_index) {
      return GetFieldAnnotationSetItem(field_annotations[i]);
    }
  }
  return nullptr;
}

mirror::Object* DexFile::GetAnnotationForField(ArtField* field,
                                               Handle<mirror::Class> annotation_class) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> field_class(hs.NewHandle(field->GetDeclaringClass()));
  return GetAnnotationObjectFromAnnotationSet(
      field_class, annotation_set, kDexVisibilityRuntime, annotation_class);
}

mirror::ObjectArray<mirror::Object>* DexFile::GetAnnotationsForField(ArtField* field) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> field_class(hs.NewHandle(field->GetDeclaringClass()));
  return ProcessAnnotationSet(field_class, annotation_set, kDexVisibilityRuntime);
}

mirror::ObjectArray<mirror::String>* DexFile::GetSignatureAnnotationForField(ArtField* field)
    const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> field_class(hs.NewHandle(field->GetDeclaringClass()));
  return GetSignatureValue(field_class, annotation_set);
}

bool DexFile::IsFieldAnnotationPresent(ArtField* field, Handle<mirror::Class> annotation_class)
    const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForField(field);
  if (annotation_set == nullptr) {
    return false;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> field_class(hs.NewHandle(field->GetDeclaringClass()));
  const AnnotationItem* annotation_item = GetAnnotationItemFromAnnotationSet(
      field_class, annotation_set, kDexVisibilityRuntime, annotation_class);
  return annotation_item != nullptr;
}

const DexFile::AnnotationSetItem* DexFile::FindAnnotationSetForMethod(ArtMethod* method) const {
  mirror::Class* klass = method->GetDeclaringClass();
  const AnnotationsDirectoryItem* annotations_dir = GetAnnotationsDirectory(*klass->GetClassDef());
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const MethodAnnotationsItem* method_annotations = GetMethodAnnotations(annotations_dir);
  if (method_annotations == nullptr) {
    return nullptr;
  }
  uint32_t method_index = method->GetDexMethodIndex();
  uint32_t method_count = annotations_dir->methods_size_;
  for (uint32_t i = 0; i < method_count; ++i) {
    if (method_annotations[i].method_idx_ == method_index) {
      return GetMethodAnnotationSetItem(method_annotations[i]);
    }
  }
  return nullptr;
}

const DexFile::ParameterAnnotationsItem* DexFile::FindAnnotationsItemForMethod(ArtMethod* method)
    const {
  mirror::Class* klass = method->GetDeclaringClass();
  const AnnotationsDirectoryItem* annotations_dir = GetAnnotationsDirectory(*klass->GetClassDef());
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const ParameterAnnotationsItem* parameter_annotations = GetParameterAnnotations(annotations_dir);
  if (parameter_annotations == nullptr) {
    return nullptr;
  }
  uint32_t method_index = method->GetDexMethodIndex();
  uint32_t parameter_count = annotations_dir->parameters_size_;
  for (uint32_t i = 0; i < parameter_count; ++i) {
    if (parameter_annotations[i].method_idx_ == method_index) {
      return &parameter_annotations[i];
    }
  }
  return nullptr;
}

mirror::Object* DexFile::GetAnnotationDefaultValue(ArtMethod* method) const {
  mirror::Class* klass = method->GetDeclaringClass();
  const AnnotationsDirectoryItem* annotations_dir = GetAnnotationsDirectory(*klass->GetClassDef());
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  const AnnotationSetItem* annotation_set = GetClassAnnotationSet(annotations_dir);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(annotation_set,
      "Ldalvik/annotation/AnnotationDefault;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  const uint8_t* annotation = SearchEncodedAnnotation(annotation_item->annotation_, "value");
  if (annotation == nullptr) {
    return nullptr;
  }
  uint8_t header_byte = *(annotation++);
  if ((header_byte & kDexAnnotationValueTypeMask) != kDexAnnotationAnnotation) {
    return nullptr;
  }
  annotation = SearchEncodedAnnotation(annotation, method->GetName());
  if (annotation == nullptr) {
    return nullptr;
  }
  AnnotationValue annotation_value;
  StackHandleScope<2> hs(Thread::Current());
  Handle<mirror::Class> h_klass(hs.NewHandle(klass));
  size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  Handle<mirror::Class> return_type(hs.NewHandle(
      method->GetReturnType(true /* resolve */, pointer_size)));
  if (!ProcessAnnotationValue(h_klass, &annotation, &annotation_value, return_type, kAllObjects)) {
    return nullptr;
  }
  return annotation_value.value_.GetL();
}

mirror::Object* DexFile::GetAnnotationForMethod(ArtMethod* method,
                                                Handle<mirror::Class> annotation_class) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> method_class(hs.NewHandle(method->GetDeclaringClass()));
  return GetAnnotationObjectFromAnnotationSet(method_class, annotation_set,
                                              kDexVisibilityRuntime, annotation_class);
}

mirror::ObjectArray<mirror::Object>* DexFile::GetAnnotationsForMethod(ArtMethod* method) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> method_class(hs.NewHandle(method->GetDeclaringClass()));
  return ProcessAnnotationSet(method_class, annotation_set, kDexVisibilityRuntime);
}

mirror::ObjectArray<mirror::Class>* DexFile::GetExceptionTypesForMethod(ArtMethod* method) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> method_class(hs.NewHandle(method->GetDeclaringClass()));
  return GetThrowsValue(method_class, annotation_set);
}

mirror::ObjectArray<mirror::Object>* DexFile::GetParameterAnnotations(ArtMethod* method) const {
  const ParameterAnnotationsItem* parameter_annotations = FindAnnotationsItemForMethod(method);
  if (parameter_annotations == nullptr) {
    return nullptr;
  }
  const AnnotationSetRefList* set_ref_list =
      GetParameterAnnotationSetRefList(parameter_annotations);
  if (set_ref_list == nullptr) {
    return nullptr;
  }
  uint32_t size = set_ref_list->size_;
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> method_class(hs.NewHandle(method->GetDeclaringClass()));
  return ProcessAnnotationSetRefList(method_class, set_ref_list, size);
}

mirror::ObjectArray<mirror::String>* DexFile::GetSignatureAnnotationForMethod(ArtMethod* method)
    const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> method_class(hs.NewHandle(method->GetDeclaringClass()));
  return GetSignatureValue(method_class, annotation_set);
}

bool DexFile::IsMethodAnnotationPresent(ArtMethod* method, Handle<mirror::Class> annotation_class)
    const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForMethod(method);
  if (annotation_set == nullptr) {
    return false;
  }
  StackHandleScope<1> hs(Thread::Current());
  Handle<mirror::Class> method_class(hs.NewHandle(method->GetDeclaringClass()));
  const AnnotationItem* annotation_item = GetAnnotationItemFromAnnotationSet(
      method_class, annotation_set, kDexVisibilityRuntime, annotation_class);
  return annotation_item != nullptr;
}

const DexFile::AnnotationSetItem* DexFile::FindAnnotationSetForClass(Handle<mirror::Class> klass)
    const {
  const AnnotationsDirectoryItem* annotations_dir = GetAnnotationsDirectory(*klass->GetClassDef());
  if (annotations_dir == nullptr) {
    return nullptr;
  }
  return GetClassAnnotationSet(annotations_dir);
}

mirror::Object* DexFile::GetAnnotationForClass(Handle<mirror::Class> klass,
                                               Handle<mirror::Class> annotation_class) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetAnnotationObjectFromAnnotationSet(klass, annotation_set, kDexVisibilityRuntime,
                                              annotation_class);
}

mirror::ObjectArray<mirror::Object>* DexFile::GetAnnotationsForClass(Handle<mirror::Class> klass)
    const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  return ProcessAnnotationSet(klass, annotation_set, kDexVisibilityRuntime);
}

mirror::ObjectArray<mirror::Class>* DexFile::GetDeclaredClasses(Handle<mirror::Class> klass) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      annotation_set, "Ldalvik/annotation/MemberClasses;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  StackHandleScope<1> hs(Thread::Current());
  mirror::Class* class_class = mirror::Class::GetJavaLangClass();
  Handle<mirror::Class> class_array_class(hs.NewHandle(
      Runtime::Current()->GetClassLinker()->FindArrayClass(hs.Self(), &class_class)));
  if (class_array_class.Get() == nullptr) {
    return nullptr;
  }
  mirror::Object* obj = GetAnnotationValue(
      klass, annotation_item, "value", class_array_class, kDexAnnotationArray);
  if (obj == nullptr) {
    return nullptr;
  }
  return obj->AsObjectArray<mirror::Class>();
}

mirror::Class* DexFile::GetDeclaringClass(Handle<mirror::Class> klass) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      annotation_set, "Ldalvik/annotation/EnclosingClass;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  mirror::Object* obj = GetAnnotationValue(klass,
                                           annotation_item,
                                           "value",
                                           ScopedNullHandle<mirror::Class>(),
                                           kDexAnnotationType);
  if (obj == nullptr) {
    return nullptr;
  }
  return obj->AsClass();
}

mirror::Class* DexFile::GetEnclosingClass(Handle<mirror::Class> klass) const {
  mirror::Class* declaring_class = GetDeclaringClass(klass);
  if (declaring_class != nullptr) {
    return declaring_class;
  }
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      annotation_set, "Ldalvik/annotation/EnclosingMethod;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  const uint8_t* annotation = SearchEncodedAnnotation(annotation_item->annotation_, "value");
  if (annotation == nullptr) {
    return nullptr;
  }
  AnnotationValue annotation_value;
  if (!ProcessAnnotationValue(klass,
                              &annotation,
                              &annotation_value,
                              ScopedNullHandle<mirror::Class>(),
                              kAllRaw)) {
    return nullptr;
  }
  if (annotation_value.type_ != kDexAnnotationMethod) {
    return nullptr;
  }
  StackHandleScope<2> hs(Thread::Current());
  Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));
  Handle<mirror::ClassLoader> class_loader(hs.NewHandle(klass->GetClassLoader()));
  ArtMethod* method = Runtime::Current()->GetClassLinker()->ResolveMethodWithoutInvokeType(
      klass->GetDexFile(), annotation_value.value_.GetI(), dex_cache, class_loader);
  if (method == nullptr) {
    return nullptr;
  }
  return method->GetDeclaringClass();
}

mirror::Object* DexFile::GetEnclosingMethod(Handle<mirror::Class> klass) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      annotation_set, "Ldalvik/annotation/EnclosingMethod;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  return GetAnnotationValue(
      klass, annotation_item, "value", ScopedNullHandle<mirror::Class>(), kDexAnnotationMethod);
}

bool DexFile::GetInnerClass(Handle<mirror::Class> klass, mirror::String** name) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      annotation_set, "Ldalvik/annotation/InnerClass;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return false;
  }
  const uint8_t* annotation = SearchEncodedAnnotation(annotation_item->annotation_, "name");
  if (annotation == nullptr) {
    return false;
  }
  AnnotationValue annotation_value;
  if (!ProcessAnnotationValue(klass,
                              &annotation,
                              &annotation_value,
                              ScopedNullHandle<mirror::Class>(),
                              kAllObjects)) {
    return false;
  }
  if (annotation_value.type_ != kDexAnnotationNull &&
      annotation_value.type_ != kDexAnnotationString) {
    return false;
  }
  *name = down_cast<mirror::String*>(annotation_value.value_.GetL());
  return true;
}

bool DexFile::GetInnerClassFlags(Handle<mirror::Class> klass, uint32_t* flags) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = SearchAnnotationSet(
      annotation_set, "Ldalvik/annotation/InnerClass;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return false;
  }
  const uint8_t* annotation = SearchEncodedAnnotation(annotation_item->annotation_, "accessFlags");
  if (annotation == nullptr) {
    return false;
  }
  AnnotationValue annotation_value;
  if (!ProcessAnnotationValue(klass,
                              &annotation,
                              &annotation_value,
                              ScopedNullHandle<mirror::Class>(),
                              kAllRaw)) {
    return false;
  }
  if (annotation_value.type_ != kDexAnnotationInt) {
    return false;
  }
  *flags = annotation_value.value_.GetI();
  return true;
}

mirror::ObjectArray<mirror::String>* DexFile::GetSignatureAnnotationForClass(
    Handle<mirror::Class> klass) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return nullptr;
  }
  return GetSignatureValue(klass, annotation_set);
}

bool DexFile::IsClassAnnotationPresent(Handle<mirror::Class> klass,
                                       Handle<mirror::Class> annotation_class) const {
  const AnnotationSetItem* annotation_set = FindAnnotationSetForClass(klass);
  if (annotation_set == nullptr) {
    return false;
  }
  const AnnotationItem* annotation_item = GetAnnotationItemFromAnnotationSet(
      klass, annotation_set, kDexVisibilityRuntime, annotation_class);
  return annotation_item != nullptr;
}

mirror::Object* DexFile::CreateAnnotationMember(Handle<mirror::Class> klass,
    Handle<mirror::Class> annotation_class, const uint8_t** annotation) const {
  Thread* self = Thread::Current();
  ScopedObjectAccessUnchecked soa(self);
  StackHandleScope<5> hs(self);
  uint32_t element_name_index = DecodeUnsignedLeb128(annotation);
  const char* name = StringDataByIdx(element_name_index);
  Handle<mirror::String> string_name(
      hs.NewHandle(mirror::String::AllocFromModifiedUtf8(self, name)));

  ArtMethod* annotation_method =
      annotation_class->FindDeclaredVirtualMethodByName(name, sizeof(void*));
  if (annotation_method == nullptr) {
    return nullptr;
  }
  size_t pointer_size = Runtime::Current()->GetClassLinker()->GetImagePointerSize();
  Handle<mirror::Class> method_return(hs.NewHandle(
      annotation_method->GetReturnType(true /* resolve */, pointer_size)));

  AnnotationValue annotation_value;
  if (!ProcessAnnotationValue(klass, annotation, &annotation_value, method_return, kAllObjects)) {
    return nullptr;
  }
  Handle<mirror::Object> value_object(hs.NewHandle(annotation_value.value_.GetL()));

  mirror::Class* annotation_member_class =
      WellKnownClasses::ToClass(WellKnownClasses::libcore_reflect_AnnotationMember);
  Handle<mirror::Object> new_member(hs.NewHandle(annotation_member_class->AllocObject(self)));
  Handle<mirror::Method> method_object(
      hs.NewHandle(mirror::Method::CreateFromArtMethod(self, annotation_method)));

  if (new_member.Get() == nullptr || string_name.Get() == nullptr ||
      method_object.Get() == nullptr || method_return.Get() == nullptr) {
    LOG(ERROR) << StringPrintf("Failed creating annotation element (m=%p n=%p a=%p r=%p",
        new_member.Get(), string_name.Get(), method_object.Get(), method_return.Get());
    return nullptr;
  }

  JValue result;
  ArtMethod* annotation_member_init =
      soa.DecodeMethod(WellKnownClasses::libcore_reflect_AnnotationMember_init);
  uint32_t args[5] = { static_cast<uint32_t>(reinterpret_cast<uintptr_t>(new_member.Get())),
                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(string_name.Get())),
                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(value_object.Get())),
                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(method_return.Get())),
                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(method_object.Get()))
  };
  annotation_member_init->Invoke(self, args, sizeof(args), &result, "VLLLL");
  if (self->IsExceptionPending()) {
    LOG(INFO) << "Exception in AnnotationMember.<init>";
    return nullptr;
  }

  return new_member.Get();
}

const DexFile::AnnotationItem* DexFile::GetAnnotationItemFromAnnotationSet(
    Handle<mirror::Class> klass, const AnnotationSetItem* annotation_set, uint32_t visibility,
    Handle<mirror::Class> annotation_class) const {
  for (uint32_t i = 0; i < annotation_set->size_; ++i) {
    const AnnotationItem* annotation_item = GetAnnotationItem(annotation_set, i);
    if (!IsVisibilityCompatible(annotation_item->visibility_, visibility)) {
      continue;
    }
    const uint8_t* annotation = annotation_item->annotation_;
    uint32_t type_index = DecodeUnsignedLeb128(&annotation);
    mirror::Class* resolved_class = Runtime::Current()->GetClassLinker()->ResolveType(
        klass->GetDexFile(), type_index, klass.Get());
    if (resolved_class == nullptr) {
      std::string temp;
      LOG(WARNING) << StringPrintf("Unable to resolve %s annotation class %d",
                                   klass->GetDescriptor(&temp), type_index);
      CHECK(Thread::Current()->IsExceptionPending());
      Thread::Current()->ClearException();
      continue;
    }
    if (resolved_class == annotation_class.Get()) {
      return annotation_item;
    }
  }

  return nullptr;
}

mirror::Object* DexFile::GetAnnotationObjectFromAnnotationSet(Handle<mirror::Class> klass,
    const AnnotationSetItem* annotation_set, uint32_t visibility,
    Handle<mirror::Class> annotation_class) const {
  const AnnotationItem* annotation_item =
      GetAnnotationItemFromAnnotationSet(klass, annotation_set, visibility, annotation_class);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  const uint8_t* annotation = annotation_item->annotation_;
  return ProcessEncodedAnnotation(klass, &annotation);
}

mirror::Object* DexFile::GetAnnotationValue(Handle<mirror::Class> klass,
    const AnnotationItem* annotation_item, const char* annotation_name,
    Handle<mirror::Class> array_class, uint32_t expected_type) const {
  const uint8_t* annotation =
      SearchEncodedAnnotation(annotation_item->annotation_, annotation_name);
  if (annotation == nullptr) {
    return nullptr;
  }
  AnnotationValue annotation_value;
  if (!ProcessAnnotationValue(klass, &annotation, &annotation_value, array_class, kAllObjects)) {
    return nullptr;
  }
  if (annotation_value.type_ != expected_type) {
    return nullptr;
  }
  return annotation_value.value_.GetL();
}

mirror::ObjectArray<mirror::String>* DexFile::GetSignatureValue(Handle<mirror::Class> klass,
    const AnnotationSetItem* annotation_set) const {
  StackHandleScope<1> hs(Thread::Current());
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(annotation_set, "Ldalvik/annotation/Signature;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  mirror::Class* string_class = mirror::String::GetJavaLangString();
  Handle<mirror::Class> string_array_class(hs.NewHandle(
      Runtime::Current()->GetClassLinker()->FindArrayClass(Thread::Current(), &string_class)));
  if (string_array_class.Get() == nullptr) {
    return nullptr;
  }
  mirror::Object* obj =
      GetAnnotationValue(klass, annotation_item, "value", string_array_class, kDexAnnotationArray);
  if (obj == nullptr) {
    return nullptr;
  }
  return obj->AsObjectArray<mirror::String>();
}

mirror::ObjectArray<mirror::Class>* DexFile::GetThrowsValue(Handle<mirror::Class> klass,
    const AnnotationSetItem* annotation_set) const {
  StackHandleScope<1> hs(Thread::Current());
  const AnnotationItem* annotation_item =
      SearchAnnotationSet(annotation_set, "Ldalvik/annotation/Throws;", kDexVisibilitySystem);
  if (annotation_item == nullptr) {
    return nullptr;
  }
  mirror::Class* class_class = mirror::Class::GetJavaLangClass();
  Handle<mirror::Class> class_array_class(hs.NewHandle(
      Runtime::Current()->GetClassLinker()->FindArrayClass(Thread::Current(), &class_class)));
  if (class_array_class.Get() == nullptr) {
    return nullptr;
  }
  mirror::Object* obj =
      GetAnnotationValue(klass, annotation_item, "value", class_array_class, kDexAnnotationArray);
  if (obj == nullptr) {
    return nullptr;
  }
  return obj->AsObjectArray<mirror::Class>();
}

mirror::ObjectArray<mirror::Object>* DexFile::ProcessAnnotationSet(Handle<mirror::Class> klass,
    const AnnotationSetItem* annotation_set, uint32_t visibility) const {
  Thread* self = Thread::Current();
  ScopedObjectAccessUnchecked soa(self);
  StackHandleScope<2> hs(self);
  Handle<mirror::Class> annotation_array_class(hs.NewHandle(
      soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_annotation_Annotation__array)));
  if (annotation_set == nullptr) {
    return mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_class.Get(), 0);
  }

  uint32_t size = annotation_set->size_;
  Handle<mirror::ObjectArray<mirror::Object>> result(hs.NewHandle(
      mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_class.Get(), size)));
  if (result.Get() == nullptr) {
    return nullptr;
  }

  uint32_t dest_index = 0;
  for (uint32_t i = 0; i < size; ++i) {
    const AnnotationItem* annotation_item = GetAnnotationItem(annotation_set, i);
    // Note that we do not use IsVisibilityCompatible here because older code
    // was correct for this case.
    if (annotation_item->visibility_ != visibility) {
      continue;
    }
    const uint8_t* annotation = annotation_item->annotation_;
    mirror::Object* annotation_obj = ProcessEncodedAnnotation(klass, &annotation);
    if (annotation_obj != nullptr) {
      result->SetWithoutChecks<false>(dest_index, annotation_obj);
      ++dest_index;
    } else if (self->IsExceptionPending()) {
      return nullptr;
    }
  }

  if (dest_index == size) {
    return result.Get();
  }

  mirror::ObjectArray<mirror::Object>* trimmed_result =
      mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_class.Get(), dest_index);
  if (trimmed_result == nullptr) {
    return nullptr;
  }

  for (uint32_t i = 0; i < dest_index; ++i) {
    mirror::Object* obj = result->GetWithoutChecks(i);
    trimmed_result->SetWithoutChecks<false>(i, obj);
  }

  return trimmed_result;
}

mirror::ObjectArray<mirror::Object>* DexFile::ProcessAnnotationSetRefList(
    Handle<mirror::Class> klass, const AnnotationSetRefList* set_ref_list, uint32_t size) const {
  Thread* self = Thread::Current();
  ScopedObjectAccessUnchecked soa(self);
  StackHandleScope<1> hs(self);
  mirror::Class* annotation_array_class =
      soa.Decode<mirror::Class*>(WellKnownClasses::java_lang_annotation_Annotation__array);
  mirror::Class* annotation_array_array_class =
      Runtime::Current()->GetClassLinker()->FindArrayClass(self, &annotation_array_class);
  if (annotation_array_array_class == nullptr) {
    return nullptr;
  }
  Handle<mirror::ObjectArray<mirror::Object>> annotation_array_array(hs.NewHandle(
      mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_array_array_class, size)));
  if (annotation_array_array.Get() == nullptr) {
    LOG(ERROR) << "Annotation set ref array allocation failed";
    return nullptr;
  }
  for (uint32_t index = 0; index < size; ++index) {
    const AnnotationSetRefItem* set_ref_item = &set_ref_list->list_[index];
    const AnnotationSetItem* set_item = GetSetRefItemItem(set_ref_item);
    mirror::Object* annotation_set = ProcessAnnotationSet(klass, set_item, kDexVisibilityRuntime);
    if (annotation_set == nullptr) {
      return nullptr;
    }
    annotation_array_array->SetWithoutChecks<false>(index, annotation_set);
  }
  return annotation_array_array.Get();
}

bool DexFile::ProcessAnnotationValue(Handle<mirror::Class> klass, const uint8_t** annotation_ptr,
    AnnotationValue* annotation_value, Handle<mirror::Class> array_class,
    DexFile::AnnotationResultStyle result_style) const {
  Thread* self = Thread::Current();
  mirror::Object* element_object = nullptr;
  bool set_object = false;
  Primitive::Type primitive_type = Primitive::kPrimVoid;
  const uint8_t* annotation = *annotation_ptr;
  uint8_t header_byte = *(annotation++);
  uint8_t value_type = header_byte & kDexAnnotationValueTypeMask;
  uint8_t value_arg = header_byte >> kDexAnnotationValueArgShift;
  int32_t width = value_arg + 1;
  annotation_value->type_ = value_type;

  switch (value_type) {
    case kDexAnnotationByte:
      annotation_value->value_.SetB(static_cast<int8_t>(ReadSignedInt(annotation, value_arg)));
      primitive_type = Primitive::kPrimByte;
      break;
    case kDexAnnotationShort:
      annotation_value->value_.SetS(static_cast<int16_t>(ReadSignedInt(annotation, value_arg)));
      primitive_type = Primitive::kPrimShort;
      break;
    case kDexAnnotationChar:
      annotation_value->value_.SetC(static_cast<uint16_t>(ReadUnsignedInt(annotation, value_arg,
                                                                          false)));
      primitive_type = Primitive::kPrimChar;
      break;
    case kDexAnnotationInt:
      annotation_value->value_.SetI(ReadSignedInt(annotation, value_arg));
      primitive_type = Primitive::kPrimInt;
      break;
    case kDexAnnotationLong:
      annotation_value->value_.SetJ(ReadSignedLong(annotation, value_arg));
      primitive_type = Primitive::kPrimLong;
      break;
    case kDexAnnotationFloat:
      annotation_value->value_.SetI(ReadUnsignedInt(annotation, value_arg, true));
      primitive_type = Primitive::kPrimFloat;
      break;
    case kDexAnnotationDouble:
      annotation_value->value_.SetJ(ReadUnsignedLong(annotation, value_arg, true));
      primitive_type = Primitive::kPrimDouble;
      break;
    case kDexAnnotationBoolean:
      annotation_value->value_.SetZ(value_arg != 0);
      primitive_type = Primitive::kPrimBoolean;
      width = 0;
      break;
    case kDexAnnotationString: {
      uint32_t index = ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        StackHandleScope<1> hs(self);
        Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));
        element_object = Runtime::Current()->GetClassLinker()->ResolveString(
            klass->GetDexFile(), index, dex_cache);
        set_object = true;
        if (element_object == nullptr) {
          return false;
        }
      }
      break;
    }
    case kDexAnnotationType: {
      uint32_t index = ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        element_object = Runtime::Current()->GetClassLinker()->ResolveType(
            klass->GetDexFile(), index, klass.Get());
        set_object = true;
        if (element_object == nullptr) {
          CHECK(self->IsExceptionPending());
          if (result_style == kAllObjects) {
            const char* msg = StringByTypeIdx(index);
            self->ThrowNewWrappedException("Ljava/lang/TypeNotPresentException;", msg);
            element_object = self->GetException();
            self->ClearException();
          } else {
            return false;
          }
        }
      }
      break;
    }
    case kDexAnnotationMethod: {
      uint32_t index = ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        StackHandleScope<2> hs(self);
        Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));
        Handle<mirror::ClassLoader> class_loader(hs.NewHandle(klass->GetClassLoader()));
        ArtMethod* method = Runtime::Current()->GetClassLinker()->ResolveMethodWithoutInvokeType(
            klass->GetDexFile(), index, dex_cache, class_loader);
        if (method == nullptr) {
          return false;
        }
        set_object = true;
        if (method->IsConstructor()) {
          element_object = mirror::Constructor::CreateFromArtMethod(self, method);
        } else {
          element_object = mirror::Method::CreateFromArtMethod(self, method);
        }
        if (element_object == nullptr) {
          return false;
        }
      }
      break;
    }
    case kDexAnnotationField: {
      uint32_t index = ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        StackHandleScope<2> hs(self);
        Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));
        Handle<mirror::ClassLoader> class_loader(hs.NewHandle(klass->GetClassLoader()));
        ArtField* field = Runtime::Current()->GetClassLinker()->ResolveFieldJLS(
            klass->GetDexFile(), index, dex_cache, class_loader);
        if (field == nullptr) {
          return false;
        }
        set_object = true;
        element_object = mirror::Field::CreateFromArtField(self, field, true);
        if (element_object == nullptr) {
          return false;
        }
      }
      break;
    }
    case kDexAnnotationEnum: {
      uint32_t index = ReadUnsignedInt(annotation, value_arg, false);
      if (result_style == kAllRaw) {
        annotation_value->value_.SetI(index);
      } else {
        StackHandleScope<3> hs(self);
        Handle<mirror::DexCache> dex_cache(hs.NewHandle(klass->GetDexCache()));
        Handle<mirror::ClassLoader> class_loader(hs.NewHandle(klass->GetClassLoader()));
        ArtField* enum_field = Runtime::Current()->GetClassLinker()->ResolveField(
            klass->GetDexFile(), index, dex_cache, class_loader, true);
        if (enum_field == nullptr) {
          return false;
        } else {
          Handle<mirror::Class> field_class(hs.NewHandle(enum_field->GetDeclaringClass()));
          Runtime::Current()->GetClassLinker()->EnsureInitialized(self, field_class, true, true);
          element_object = enum_field->GetObject(field_class.Get());
          set_object = true;
        }
      }
      break;
    }
    case kDexAnnotationArray:
      if (result_style == kAllRaw || array_class.Get() == nullptr) {
        return false;
      } else {
        ScopedObjectAccessUnchecked soa(self);
        StackHandleScope<2> hs(self);
        uint32_t size = DecodeUnsignedLeb128(&annotation);
        Handle<mirror::Class> component_type(hs.NewHandle(array_class->GetComponentType()));
        Handle<mirror::Array> new_array(hs.NewHandle(mirror::Array::Alloc<true>(
            self, array_class.Get(), size, array_class->GetComponentSizeShift(),
            Runtime::Current()->GetHeap()->GetCurrentAllocator())));
        if (new_array.Get() == nullptr) {
          LOG(ERROR) << "Annotation element array allocation failed with size " << size;
          return false;
        }
        AnnotationValue new_annotation_value;
        for (uint32_t i = 0; i < size; ++i) {
          if (!ProcessAnnotationValue(klass, &annotation, &new_annotation_value, component_type,
                                      kPrimitivesOrObjects)) {
            return false;
          }
          if (!component_type->IsPrimitive()) {
            mirror::Object* obj = new_annotation_value.value_.GetL();
            new_array->AsObjectArray<mirror::Object>()->SetWithoutChecks<false>(i, obj);
          } else {
            switch (new_annotation_value.type_) {
              case kDexAnnotationByte:
                new_array->AsByteArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetB());
                break;
              case kDexAnnotationShort:
                new_array->AsShortArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetS());
                break;
              case kDexAnnotationChar:
                new_array->AsCharArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetC());
                break;
              case kDexAnnotationInt:
                new_array->AsIntArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetI());
                break;
              case kDexAnnotationLong:
                new_array->AsLongArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetJ());
                break;
              case kDexAnnotationFloat:
                new_array->AsFloatArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetF());
                break;
              case kDexAnnotationDouble:
                new_array->AsDoubleArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetD());
                break;
              case kDexAnnotationBoolean:
                new_array->AsBooleanArray()->SetWithoutChecks<false>(
                    i, new_annotation_value.value_.GetZ());
                break;
              default:
                LOG(FATAL) << "Found invalid annotation value type while building annotation array";
                return false;
            }
          }
        }
        element_object = new_array.Get();
        set_object = true;
        width = 0;
      }
      break;
    case kDexAnnotationAnnotation:
      if (result_style == kAllRaw) {
        return false;
      }
      element_object = ProcessEncodedAnnotation(klass, &annotation);
      if (element_object == nullptr) {
        return false;
      }
      set_object = true;
      width = 0;
      break;
    case kDexAnnotationNull:
      if (result_style == kAllRaw) {
        annotation_value->value_.SetI(0);
      } else {
        CHECK(element_object == nullptr);
        set_object = true;
      }
      width = 0;
      break;
    default:
      LOG(ERROR) << StringPrintf("Bad annotation element value type 0x%02x", value_type);
      return false;
  }

  annotation += width;
  *annotation_ptr = annotation;

  if (result_style == kAllObjects && primitive_type != Primitive::kPrimVoid) {
    element_object = BoxPrimitive(primitive_type, annotation_value->value_);
    set_object = true;
  }

  if (set_object) {
    annotation_value->value_.SetL(element_object);
  }

  return true;
}

mirror::Object* DexFile::ProcessEncodedAnnotation(Handle<mirror::Class> klass,
    const uint8_t** annotation) const {
  uint32_t type_index = DecodeUnsignedLeb128(annotation);
  uint32_t size = DecodeUnsignedLeb128(annotation);

  Thread* self = Thread::Current();
  ScopedObjectAccessUnchecked soa(self);
  StackHandleScope<2> hs(self);
  ClassLinker* class_linker = Runtime::Current()->GetClassLinker();
  Handle<mirror::Class> annotation_class(hs.NewHandle(
      class_linker->ResolveType(klass->GetDexFile(), type_index, klass.Get())));
  if (annotation_class.Get() == nullptr) {
    LOG(INFO) << "Unable to resolve " << PrettyClass(klass.Get()) << " annotation class "
              << type_index;
    DCHECK(Thread::Current()->IsExceptionPending());
    Thread::Current()->ClearException();
    return nullptr;
  }

  mirror::Class* annotation_member_class =
      soa.Decode<mirror::Class*>(WellKnownClasses::libcore_reflect_AnnotationMember);
  mirror::Class* annotation_member_array_class =
      class_linker->FindArrayClass(self, &annotation_member_class);
  if (annotation_member_array_class == nullptr) {
    return nullptr;
  }
  mirror::ObjectArray<mirror::Object>* element_array = nullptr;
  if (size > 0) {
    element_array =
        mirror::ObjectArray<mirror::Object>::Alloc(self, annotation_member_array_class, size);
    if (element_array == nullptr) {
      LOG(ERROR) << "Failed to allocate annotation member array (" << size << " elements)";
      return nullptr;
    }
  }

  Handle<mirror::ObjectArray<mirror::Object>> h_element_array(hs.NewHandle(element_array));
  for (uint32_t i = 0; i < size; ++i) {
    mirror::Object* new_member = CreateAnnotationMember(klass, annotation_class, annotation);
    if (new_member == nullptr) {
      return nullptr;
    }
    h_element_array->SetWithoutChecks<false>(i, new_member);
  }

  JValue result;
  ArtMethod* create_annotation_method =
      soa.DecodeMethod(WellKnownClasses::libcore_reflect_AnnotationFactory_createAnnotation);
  uint32_t args[2] = { static_cast<uint32_t>(reinterpret_cast<uintptr_t>(annotation_class.Get())),
                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(h_element_array.Get())) };
  create_annotation_method->Invoke(self, args, sizeof(args), &result, "LLL");
  if (self->IsExceptionPending()) {
    LOG(INFO) << "Exception in AnnotationFactory.createAnnotation";
    return nullptr;
  }

  return result.GetL();
}

const DexFile::AnnotationItem* DexFile::SearchAnnotationSet(const AnnotationSetItem* annotation_set,
    const char* descriptor, uint32_t visibility) const {
  const AnnotationItem* result = nullptr;
  for (uint32_t i = 0; i < annotation_set->size_; ++i) {
    const AnnotationItem* annotation_item = GetAnnotationItem(annotation_set, i);
    if (!IsVisibilityCompatible(annotation_item->visibility_, visibility)) {
      continue;
    }
    const uint8_t* annotation = annotation_item->annotation_;
    uint32_t type_index = DecodeUnsignedLeb128(&annotation);

    if (strcmp(descriptor, StringByTypeIdx(type_index)) == 0) {
      result = annotation_item;
      break;
    }
  }
  return result;
}

const uint8_t* DexFile::SearchEncodedAnnotation(const uint8_t* annotation, const char* name) const {
  DecodeUnsignedLeb128(&annotation);  // unused type_index
  uint32_t size = DecodeUnsignedLeb128(&annotation);

  while (size != 0) {
    uint32_t element_name_index = DecodeUnsignedLeb128(&annotation);
    const char* element_name = GetStringData(GetStringId(element_name_index));
    if (strcmp(name, element_name) == 0) {
      return annotation;
    }
    SkipAnnotationValue(&annotation);
    size--;
  }
  return nullptr;
}

bool DexFile::SkipAnnotationValue(const uint8_t** annotation_ptr) const {
  const uint8_t* annotation = *annotation_ptr;
  uint8_t header_byte = *(annotation++);
  uint8_t value_type = header_byte & kDexAnnotationValueTypeMask;
  uint8_t value_arg = header_byte >> kDexAnnotationValueArgShift;
  int32_t width = value_arg + 1;

  switch (value_type) {
    case kDexAnnotationByte:
    case kDexAnnotationShort:
    case kDexAnnotationChar:
    case kDexAnnotationInt:
    case kDexAnnotationLong:
    case kDexAnnotationFloat:
    case kDexAnnotationDouble:
    case kDexAnnotationString:
    case kDexAnnotationType:
    case kDexAnnotationMethod:
    case kDexAnnotationField:
    case kDexAnnotationEnum:
      break;
    case kDexAnnotationArray:
    {
      uint32_t size = DecodeUnsignedLeb128(&annotation);
      while (size--) {
        if (!SkipAnnotationValue(&annotation)) {
          return false;
        }
      }
      width = 0;
      break;
    }
    case kDexAnnotationAnnotation:
    {
      DecodeUnsignedLeb128(&annotation);  // unused type_index
      uint32_t size = DecodeUnsignedLeb128(&annotation);
      while (size--) {
        DecodeUnsignedLeb128(&annotation);  // unused element_name_index
        if (!SkipAnnotationValue(&annotation)) {
          return false;
        }
      }
      width = 0;
      break;
    }
    case kDexAnnotationBoolean:
    case kDexAnnotationNull:
      width = 0;
      break;
    default:
      LOG(FATAL) << StringPrintf("Bad annotation element value byte 0x%02x", value_type);
      return false;
  }

  annotation += width;
  *annotation_ptr = annotation;
  return true;
}

std::ostream& operator<<(std::ostream& os, const DexFile& dex_file) {
  os << StringPrintf("[DexFile: %s dex-checksum=%08x location-checksum=%08x %p-%p]",
                     dex_file.GetLocation().c_str(),
                     dex_file.GetHeader().checksum_, dex_file.GetLocationChecksum(),
                     dex_file.Begin(), dex_file.Begin() + dex_file.Size());
  return os;
}

std::string Signature::ToString() const {
  if (dex_file_ == nullptr) {
    CHECK(proto_id_ == nullptr);
    return "<no signature>";
  }
  const DexFile::TypeList* params = dex_file_->GetProtoParameters(*proto_id_);
  std::string result;
  if (params == nullptr) {
    result += "()";
  } else {
    result += "(";
    for (uint32_t i = 0; i < params->Size(); ++i) {
      result += dex_file_->StringByTypeIdx(params->GetTypeItem(i).type_idx_);
    }
    result += ")";
  }
  result += dex_file_->StringByTypeIdx(proto_id_->return_type_idx_);
  return result;
}

bool Signature::operator==(const StringPiece& rhs) const {
  if (dex_file_ == nullptr) {
    return false;
  }
  StringPiece tail(rhs);
  if (!tail.starts_with("(")) {
    return false;  // Invalid signature
  }
  tail.remove_prefix(1);  // "(";
  const DexFile::TypeList* params = dex_file_->GetProtoParameters(*proto_id_);
  if (params != nullptr) {
    for (uint32_t i = 0; i < params->Size(); ++i) {
      StringPiece param(dex_file_->StringByTypeIdx(params->GetTypeItem(i).type_idx_));
      if (!tail.starts_with(param)) {
        return false;
      }
      tail.remove_prefix(param.length());
    }
  }
  if (!tail.starts_with(")")) {
    return false;
  }
  tail.remove_prefix(1);  // ")";
  return tail == dex_file_->StringByTypeIdx(proto_id_->return_type_idx_);
}

std::ostream& operator<<(std::ostream& os, const Signature& sig) {
  return os << sig.ToString();
}

// Decodes the header section from the class data bytes.
void ClassDataItemIterator::ReadClassDataHeader() {
  CHECK(ptr_pos_ != nullptr);
  header_.static_fields_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.instance_fields_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.direct_methods_size_ = DecodeUnsignedLeb128(&ptr_pos_);
  header_.virtual_methods_size_ = DecodeUnsignedLeb128(&ptr_pos_);
}

void ClassDataItemIterator::ReadClassDataField() {
  field_.field_idx_delta_ = DecodeUnsignedLeb128(&ptr_pos_);
  field_.access_flags_ = DecodeUnsignedLeb128(&ptr_pos_);
  // The user of the iterator is responsible for checking if there
  // are unordered or duplicate indexes.
}

void ClassDataItemIterator::ReadClassDataMethod() {
  method_.method_idx_delta_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.access_flags_ = DecodeUnsignedLeb128(&ptr_pos_);
  method_.code_off_ = DecodeUnsignedLeb128(&ptr_pos_);
  if (last_idx_ != 0 && method_.method_idx_delta_ == 0) {
    LOG(WARNING) << "Duplicate method in " << dex_file_.GetLocation();
  }
}

EncodedStaticFieldValueIterator::EncodedStaticFieldValueIterator(
    const DexFile& dex_file,
    const DexFile::ClassDef& class_def)
    : EncodedStaticFieldValueIterator(dex_file,
                                      nullptr,
                                      nullptr,
                                      nullptr,
                                      class_def,
                                      -1,
                                      kByte) {
}

EncodedStaticFieldValueIterator::EncodedStaticFieldValueIterator(
    const DexFile& dex_file,
    Handle<mirror::DexCache>* dex_cache,
    Handle<mirror::ClassLoader>* class_loader,
    ClassLinker* linker,
    const DexFile::ClassDef& class_def)
    : EncodedStaticFieldValueIterator(dex_file,
                                      dex_cache, class_loader,
                                      linker,
                                      class_def,
                                      -1,
                                      kByte) {
  DCHECK(dex_cache_ != nullptr);
  DCHECK(class_loader_ != nullptr);
}

EncodedStaticFieldValueIterator::EncodedStaticFieldValueIterator(
    const DexFile& dex_file,
    Handle<mirror::DexCache>* dex_cache,
    Handle<mirror::ClassLoader>* class_loader,
    ClassLinker* linker,
    const DexFile::ClassDef& class_def,
    size_t pos,
    ValueType type)
    : dex_file_(dex_file),
      dex_cache_(dex_cache),
      class_loader_(class_loader),
      linker_(linker),
      array_size_(),
      pos_(pos),
      type_(type) {
  ptr_ = dex_file.GetEncodedStaticFieldValuesArray(class_def);
  if (ptr_ == nullptr) {
    array_size_ = 0;
  } else {
    array_size_ = DecodeUnsignedLeb128(&ptr_);
  }
  if (array_size_ > 0) {
    Next();
  }
}

void EncodedStaticFieldValueIterator::Next() {
  pos_++;
  if (pos_ >= array_size_) {
    return;
  }
  uint8_t value_type = *ptr_++;
  uint8_t value_arg = value_type >> kEncodedValueArgShift;
  size_t width = value_arg + 1;  // assume and correct later
  type_ = static_cast<ValueType>(value_type & kEncodedValueTypeMask);
  switch (type_) {
  case kBoolean:
    jval_.i = (value_arg != 0) ? 1 : 0;
    width = 0;
    break;
  case kByte:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    CHECK(IsInt<8>(jval_.i));
    break;
  case kShort:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    CHECK(IsInt<16>(jval_.i));
    break;
  case kChar:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, false);
    CHECK(IsUint<16>(jval_.i));
    break;
  case kInt:
    jval_.i = ReadSignedInt(ptr_, value_arg);
    break;
  case kLong:
    jval_.j = ReadSignedLong(ptr_, value_arg);
    break;
  case kFloat:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, true);
    break;
  case kDouble:
    jval_.j = ReadUnsignedLong(ptr_, value_arg, true);
    break;
  case kString:
  case kType:
    jval_.i = ReadUnsignedInt(ptr_, value_arg, false);
    break;
  case kField:
  case kMethod:
  case kEnum:
  case kArray:
  case kAnnotation:
    UNIMPLEMENTED(FATAL) << ": type " << type_;
    UNREACHABLE();
  case kNull:
    jval_.l = nullptr;
    width = 0;
    break;
  default:
    LOG(FATAL) << "Unreached";
    UNREACHABLE();
  }
  ptr_ += width;
}

template<bool kTransactionActive>
void EncodedStaticFieldValueIterator::ReadValueToField(ArtField* field) const {
  DCHECK(dex_cache_ != nullptr);
  DCHECK(class_loader_ != nullptr);
  switch (type_) {
    case kBoolean: field->SetBoolean<kTransactionActive>(field->GetDeclaringClass(), jval_.z);
        break;
    case kByte:    field->SetByte<kTransactionActive>(field->GetDeclaringClass(), jval_.b); break;
    case kShort:   field->SetShort<kTransactionActive>(field->GetDeclaringClass(), jval_.s); break;
    case kChar:    field->SetChar<kTransactionActive>(field->GetDeclaringClass(), jval_.c); break;
    case kInt:     field->SetInt<kTransactionActive>(field->GetDeclaringClass(), jval_.i); break;
    case kLong:    field->SetLong<kTransactionActive>(field->GetDeclaringClass(), jval_.j); break;
    case kFloat:   field->SetFloat<kTransactionActive>(field->GetDeclaringClass(), jval_.f); break;
    case kDouble:  field->SetDouble<kTransactionActive>(field->GetDeclaringClass(), jval_.d); break;
    case kNull:    field->SetObject<kTransactionActive>(field->GetDeclaringClass(), nullptr); break;
    case kString: {
      mirror::String* resolved = linker_->ResolveString(dex_file_, jval_.i, *dex_cache_);
      field->SetObject<kTransactionActive>(field->GetDeclaringClass(), resolved);
      break;
    }
    case kType: {
      mirror::Class* resolved = linker_->ResolveType(dex_file_, jval_.i, *dex_cache_,
                                                     *class_loader_);
      field->SetObject<kTransactionActive>(field->GetDeclaringClass(), resolved);
      break;
    }
    default: UNIMPLEMENTED(FATAL) << ": type " << type_;
  }
}
template void EncodedStaticFieldValueIterator::ReadValueToField<true>(ArtField* field) const;
template void EncodedStaticFieldValueIterator::ReadValueToField<false>(ArtField* field) const;

CatchHandlerIterator::CatchHandlerIterator(const DexFile::CodeItem& code_item, uint32_t address) {
  handler_.address_ = -1;
  int32_t offset = -1;

  // Short-circuit the overwhelmingly common cases.
  switch (code_item.tries_size_) {
    case 0:
      break;
    case 1: {
      const DexFile::TryItem* tries = DexFile::GetTryItems(code_item, 0);
      uint32_t start = tries->start_addr_;
      if (address >= start) {
        uint32_t end = start + tries->insn_count_;
        if (address < end) {
          offset = tries->handler_off_;
        }
      }
      break;
    }
    default:
      offset = DexFile::FindCatchHandlerOffset(code_item, address);
  }
  Init(code_item, offset);
}

CatchHandlerIterator::CatchHandlerIterator(const DexFile::CodeItem& code_item,
                                           const DexFile::TryItem& try_item) {
  handler_.address_ = -1;
  Init(code_item, try_item.handler_off_);
}

void CatchHandlerIterator::Init(const DexFile::CodeItem& code_item,
                                int32_t offset) {
  if (offset >= 0) {
    Init(DexFile::GetCatchHandlerData(code_item, offset));
  } else {
    // Not found, initialize as empty
    current_data_ = nullptr;
    remaining_count_ = -1;
    catch_all_ = false;
    DCHECK(!HasNext());
  }
}

void CatchHandlerIterator::Init(const uint8_t* handler_data) {
  current_data_ = handler_data;
  remaining_count_ = DecodeSignedLeb128(&current_data_);

  // If remaining_count_ is non-positive, then it is the negative of
  // the number of catch types, and the catches are followed by a
  // catch-all handler.
  if (remaining_count_ <= 0) {
    catch_all_ = true;
    remaining_count_ = -remaining_count_;
  } else {
    catch_all_ = false;
  }
  Next();
}

void CatchHandlerIterator::Next() {
  if (remaining_count_ > 0) {
    handler_.type_idx_ = DecodeUnsignedLeb128(&current_data_);
    handler_.address_  = DecodeUnsignedLeb128(&current_data_);
    remaining_count_--;
    return;
  }

  if (catch_all_) {
    handler_.type_idx_ = DexFile::kDexNoIndex16;
    handler_.address_  = DecodeUnsignedLeb128(&current_data_);
    catch_all_ = false;
    return;
  }

  // no more handler
  remaining_count_ = -1;
}

}  // namespace art
