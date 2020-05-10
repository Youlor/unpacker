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

#include "offline_profiling_info.h"

#include "errno.h"
#include <limits.h>
#include <vector>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/uio.h>

#include "art_method-inl.h"
#include "base/mutex.h"
#include "base/scoped_flock.h"
#include "base/stl_util.h"
#include "base/systrace.h"
#include "base/unix_file/fd_file.h"
#include "jit/profiling_info.h"
#include "os.h"
#include "safe_map.h"

namespace art {

const uint8_t ProfileCompilationInfo::kProfileMagic[] = { 'p', 'r', 'o', '\0' };
const uint8_t ProfileCompilationInfo::kProfileVersion[] = { '0', '0', '1', '\0' };

static constexpr uint16_t kMaxDexFileKeyLength = PATH_MAX;

// Transform the actual dex location into relative paths.
// Note: this is OK because we don't store profiles of different apps into the same file.
// Apps with split apks don't cause trouble because each split has a different name and will not
// collide with other entries.
std::string ProfileCompilationInfo::GetProfileDexFileKey(const std::string& dex_location) {
  DCHECK(!dex_location.empty());
  size_t last_sep_index = dex_location.find_last_of('/');
  if (last_sep_index == std::string::npos) {
    return dex_location;
  } else {
    DCHECK(last_sep_index < dex_location.size());
    return dex_location.substr(last_sep_index + 1);
  }
}

bool ProfileCompilationInfo::AddMethodsAndClasses(
    const std::vector<MethodReference>& methods,
    const std::set<DexCacheResolvedClasses>& resolved_classes) {
  for (const MethodReference& method : methods) {
    if (!AddMethodIndex(GetProfileDexFileKey(method.dex_file->GetLocation()),
                        method.dex_file->GetLocationChecksum(),
                        method.dex_method_index)) {
      return false;
    }
  }
  for (const DexCacheResolvedClasses& dex_cache : resolved_classes) {
    if (!AddResolvedClasses(dex_cache)) {
      return false;
    }
  }
  return true;
}

bool ProfileCompilationInfo::MergeAndSave(const std::string& filename,
                                          uint64_t* bytes_written,
                                          bool force) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  ScopedFlock flock;
  std::string error;
  if (!flock.Init(filename.c_str(), O_RDWR | O_NOFOLLOW | O_CLOEXEC, /* block */ false, &error)) {
    LOG(WARNING) << "Couldn't lock the profile file " << filename << ": " << error;
    return false;
  }

  int fd = flock.GetFile()->Fd();

  // Load the file but keep a copy around to be able to infer if the content has changed.
  ProfileCompilationInfo fileInfo;
  ProfileLoadSatus status = fileInfo.LoadInternal(fd, &error);
  if (status == kProfileLoadSuccess) {
    // Merge the content of file into the current object.
    if (MergeWith(fileInfo)) {
      // If after the merge we have the same data as what is the file there's no point
      // in actually doing the write. The file will be exactly the same as before.
      if (Equals(fileInfo)) {
        if (bytes_written != nullptr) {
          *bytes_written = 0;
        }
        return true;
      }
    } else {
      LOG(WARNING) << "Could not merge previous profile data from file " << filename;
      if (!force) {
        return false;
      }
    }
  } else if (force &&
        ((status == kProfileLoadVersionMismatch) || (status == kProfileLoadBadData))) {
      // Log a warning but don't return false. We will clear the profile anyway.
      LOG(WARNING) << "Clearing bad or obsolete profile data from file "
          << filename << ": " << error;
  } else {
    LOG(WARNING) << "Could not load profile data from file " << filename << ": " << error;
    return false;
  }

  // We need to clear the data because we don't support appending to the profiles yet.
  if (!flock.GetFile()->ClearContent()) {
    PLOG(WARNING) << "Could not clear profile file: " << filename;
    return false;
  }

  // This doesn't need locking because we are trying to lock the file for exclusive
  // access and fail immediately if we can't.
  bool result = Save(fd);
  if (result) {
    VLOG(profiler) << "Successfully saved profile info to " << filename
        << " Size: " << GetFileSizeBytes(filename);
    if (bytes_written != nullptr) {
      *bytes_written = GetFileSizeBytes(filename);
    }
  } else {
    VLOG(profiler) << "Failed to save profile info to " << filename;
  }
  return result;
}

// Returns true if all the bytes were successfully written to the file descriptor.
static bool WriteBuffer(int fd, const uint8_t* buffer, size_t byte_count) {
  while (byte_count > 0) {
    int bytes_written = TEMP_FAILURE_RETRY(write(fd, buffer, byte_count));
    if (bytes_written == -1) {
      return false;
    }
    byte_count -= bytes_written;  // Reduce the number of remaining bytes.
    buffer += bytes_written;  // Move the buffer forward.
  }
  return true;
}

// Add the string bytes to the buffer.
static void AddStringToBuffer(std::vector<uint8_t>* buffer, const std::string& value) {
  buffer->insert(buffer->end(), value.begin(), value.end());
}

// Insert each byte, from low to high into the buffer.
template <typename T>
static void AddUintToBuffer(std::vector<uint8_t>* buffer, T value) {
  for (size_t i = 0; i < sizeof(T); i++) {
    buffer->push_back((value >> (i * kBitsPerByte)) & 0xff);
  }
}

static constexpr size_t kLineHeaderSize =
    3 * sizeof(uint16_t) +  // method_set.size + class_set.size + dex_location.size
    sizeof(uint32_t);       // checksum

/**
 * Serialization format:
 *    magic,version,number_of_lines
 *    dex_location1,number_of_methods1,number_of_classes1,dex_location_checksum1, \
 *        method_id11,method_id12...,class_id1,class_id2...
 *    dex_location2,number_of_methods2,number_of_classes2,dex_location_checksum2, \
 *        method_id21,method_id22...,,class_id1,class_id2...
 *    .....
 **/
bool ProfileCompilationInfo::Save(int fd) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  DCHECK_GE(fd, 0);

  // Cache at most 5KB before writing.
  static constexpr size_t kMaxSizeToKeepBeforeWriting = 5 * KB;
  // Use a vector wrapper to avoid keeping track of offsets when we add elements.
  std::vector<uint8_t> buffer;
  WriteBuffer(fd, kProfileMagic, sizeof(kProfileMagic));
  WriteBuffer(fd, kProfileVersion, sizeof(kProfileVersion));
  AddUintToBuffer(&buffer, static_cast<uint16_t>(info_.size()));

  for (const auto& it : info_) {
    if (buffer.size() > kMaxSizeToKeepBeforeWriting) {
      if (!WriteBuffer(fd, buffer.data(), buffer.size())) {
        return false;
      }
      buffer.clear();
    }
    const std::string& dex_location = it.first;
    const DexFileData& dex_data = it.second;
    if (dex_data.method_set.empty() && dex_data.class_set.empty()) {
      continue;
    }

    if (dex_location.size() >= kMaxDexFileKeyLength) {
      LOG(WARNING) << "DexFileKey exceeds allocated limit";
      return false;
    }

    // Make sure that the buffer has enough capacity to avoid repeated resizings
    // while we add data.
    size_t required_capacity = buffer.size() +
        kLineHeaderSize +
        dex_location.size() +
        sizeof(uint16_t) * (dex_data.class_set.size() + dex_data.method_set.size());

    buffer.reserve(required_capacity);

    DCHECK_LE(dex_location.size(), std::numeric_limits<uint16_t>::max());
    DCHECK_LE(dex_data.method_set.size(), std::numeric_limits<uint16_t>::max());
    DCHECK_LE(dex_data.class_set.size(), std::numeric_limits<uint16_t>::max());
    AddUintToBuffer(&buffer, static_cast<uint16_t>(dex_location.size()));
    AddUintToBuffer(&buffer, static_cast<uint16_t>(dex_data.method_set.size()));
    AddUintToBuffer(&buffer, static_cast<uint16_t>(dex_data.class_set.size()));
    AddUintToBuffer(&buffer, dex_data.checksum);  // uint32_t

    AddStringToBuffer(&buffer, dex_location);

    for (auto method_it : dex_data.method_set) {
      AddUintToBuffer(&buffer, method_it);
    }
    for (auto class_id : dex_data.class_set) {
      AddUintToBuffer(&buffer, class_id);
    }
    DCHECK_EQ(required_capacity, buffer.size())
        << "Failed to add the expected number of bytes in the buffer";
  }

  return WriteBuffer(fd, buffer.data(), buffer.size());
}

ProfileCompilationInfo::DexFileData* ProfileCompilationInfo::GetOrAddDexFileData(
    const std::string& dex_location,
    uint32_t checksum) {
  auto info_it = info_.find(dex_location);
  if (info_it == info_.end()) {
    info_it = info_.Put(dex_location, DexFileData(checksum));
  }
  if (info_it->second.checksum != checksum) {
    LOG(WARNING) << "Checksum mismatch for dex " << dex_location;
    return nullptr;
  }
  return &info_it->second;
}

bool ProfileCompilationInfo::AddResolvedClasses(const DexCacheResolvedClasses& classes) {
  const std::string dex_location = GetProfileDexFileKey(classes.GetDexLocation());
  const uint32_t checksum = classes.GetLocationChecksum();
  DexFileData* const data = GetOrAddDexFileData(dex_location, checksum);
  if (data == nullptr) {
    return false;
  }
  data->class_set.insert(classes.GetClasses().begin(), classes.GetClasses().end());
  return true;
}

bool ProfileCompilationInfo::AddMethodIndex(const std::string& dex_location,
                                            uint32_t checksum,
                                            uint16_t method_idx) {
  DexFileData* const data = GetOrAddDexFileData(dex_location, checksum);
  if (data == nullptr) {
    return false;
  }
  data->method_set.insert(method_idx);
  return true;
}

bool ProfileCompilationInfo::AddClassIndex(const std::string& dex_location,
                                           uint32_t checksum,
                                           uint16_t class_idx) {
  DexFileData* const data = GetOrAddDexFileData(dex_location, checksum);
  if (data == nullptr) {
    return false;
  }
  data->class_set.insert(class_idx);
  return true;
}

bool ProfileCompilationInfo::ProcessLine(SafeBuffer& line_buffer,
                                         uint16_t method_set_size,
                                         uint16_t class_set_size,
                                         uint32_t checksum,
                                         const std::string& dex_location) {
  for (uint16_t i = 0; i < method_set_size; i++) {
    uint16_t method_idx = line_buffer.ReadUintAndAdvance<uint16_t>();
    if (!AddMethodIndex(dex_location, checksum, method_idx)) {
      return false;
    }
  }

  for (uint16_t i = 0; i < class_set_size; i++) {
    uint16_t class_def_idx = line_buffer.ReadUintAndAdvance<uint16_t>();
    if (!AddClassIndex(dex_location, checksum, class_def_idx)) {
      return false;
    }
  }
  return true;
}

// Tests for EOF by trying to read 1 byte from the descriptor.
// Returns:
//   0 if the descriptor is at the EOF,
//  -1 if there was an IO error
//   1 if the descriptor has more content to read
static int testEOF(int fd) {
  uint8_t buffer[1];
  return TEMP_FAILURE_RETRY(read(fd, buffer, 1));
}

// Reads an uint value previously written with AddUintToBuffer.
template <typename T>
T ProfileCompilationInfo::SafeBuffer::ReadUintAndAdvance() {
  static_assert(std::is_unsigned<T>::value, "Type is not unsigned");
  CHECK_LE(ptr_current_ + sizeof(T), ptr_end_);
  T value = 0;
  for (size_t i = 0; i < sizeof(T); i++) {
    value += ptr_current_[i] << (i * kBitsPerByte);
  }
  ptr_current_ += sizeof(T);
  return value;
}

bool ProfileCompilationInfo::SafeBuffer::CompareAndAdvance(const uint8_t* data, size_t data_size) {
  if (ptr_current_ + data_size > ptr_end_) {
    return false;
  }
  if (memcmp(ptr_current_, data, data_size) == 0) {
    ptr_current_ += data_size;
    return true;
  }
  return false;
}

ProfileCompilationInfo::ProfileLoadSatus ProfileCompilationInfo::SafeBuffer::FillFromFd(
      int fd,
      const std::string& source,
      /*out*/std::string* error) {
  size_t byte_count = ptr_end_ - ptr_current_;
  uint8_t* buffer = ptr_current_;
  while (byte_count > 0) {
    int bytes_read = TEMP_FAILURE_RETRY(read(fd, buffer, byte_count));
    if (bytes_read == 0) {
      *error += "Profile EOF reached prematurely for " + source;
      return kProfileLoadBadData;
    } else if (bytes_read < 0) {
      *error += "Profile IO error for " + source + strerror(errno);
      return kProfileLoadIOError;
    }
    byte_count -= bytes_read;
    buffer += bytes_read;
  }
  return kProfileLoadSuccess;
}

ProfileCompilationInfo::ProfileLoadSatus ProfileCompilationInfo::ReadProfileHeader(
      int fd,
      /*out*/uint16_t* number_of_lines,
      /*out*/std::string* error) {
  // Read magic and version
  const size_t kMagicVersionSize =
    sizeof(kProfileMagic) +
    sizeof(kProfileVersion) +
    sizeof(uint16_t);  // number of lines

  SafeBuffer safe_buffer(kMagicVersionSize);

  ProfileLoadSatus status = safe_buffer.FillFromFd(fd, "ReadProfileHeader", error);
  if (status != kProfileLoadSuccess) {
    return status;
  }

  if (!safe_buffer.CompareAndAdvance(kProfileMagic, sizeof(kProfileMagic))) {
    *error = "Profile missing magic";
    return kProfileLoadVersionMismatch;
  }
  if (!safe_buffer.CompareAndAdvance(kProfileVersion, sizeof(kProfileVersion))) {
    *error = "Profile version mismatch";
    return kProfileLoadVersionMismatch;
  }
  *number_of_lines = safe_buffer.ReadUintAndAdvance<uint16_t>();
  return kProfileLoadSuccess;
}

ProfileCompilationInfo::ProfileLoadSatus ProfileCompilationInfo::ReadProfileLineHeader(
      int fd,
      /*out*/ProfileLineHeader* line_header,
      /*out*/std::string* error) {
  SafeBuffer header_buffer(kLineHeaderSize);
  ProfileLoadSatus status = header_buffer.FillFromFd(fd, "ReadProfileHeader", error);
  if (status != kProfileLoadSuccess) {
    return status;
  }

  uint16_t dex_location_size = header_buffer.ReadUintAndAdvance<uint16_t>();
  line_header->method_set_size = header_buffer.ReadUintAndAdvance<uint16_t>();
  line_header->class_set_size = header_buffer.ReadUintAndAdvance<uint16_t>();
  line_header->checksum = header_buffer.ReadUintAndAdvance<uint32_t>();

  if (dex_location_size == 0 || dex_location_size > kMaxDexFileKeyLength) {
    *error = "DexFileKey has an invalid size: " + std::to_string(dex_location_size);
    return kProfileLoadBadData;
  }

  SafeBuffer location_buffer(dex_location_size);
  status = location_buffer.FillFromFd(fd, "ReadProfileHeaderDexLocation", error);
  if (status != kProfileLoadSuccess) {
    return status;
  }
  line_header->dex_location.assign(
      reinterpret_cast<char*>(location_buffer.Get()), dex_location_size);
  return kProfileLoadSuccess;
}

ProfileCompilationInfo::ProfileLoadSatus ProfileCompilationInfo::ReadProfileLine(
      int fd,
      const ProfileLineHeader& line_header,
      /*out*/std::string* error) {
  // Make sure that we don't try to read everything in memory (in case the profile if full).
  // Split readings in chunks of at most 10kb.
  static constexpr uint16_t kMaxNumberOfEntriesToRead = 5120;
  uint16_t methods_left_to_read = line_header.method_set_size;
  uint16_t classes_left_to_read = line_header.class_set_size;

  while ((methods_left_to_read > 0) || (classes_left_to_read > 0)) {
    uint16_t methods_to_read = std::min(kMaxNumberOfEntriesToRead, methods_left_to_read);
    uint16_t max_classes_to_read = kMaxNumberOfEntriesToRead - methods_to_read;
    uint16_t classes_to_read = std::min(max_classes_to_read, classes_left_to_read);

    size_t line_size = sizeof(uint16_t) * (methods_to_read + classes_to_read);
    SafeBuffer line_buffer(line_size);

    ProfileLoadSatus status = line_buffer.FillFromFd(fd, "ReadProfileLine", error);
    if (status != kProfileLoadSuccess) {
      return status;
    }
    if (!ProcessLine(line_buffer,
                     methods_to_read,
                     classes_to_read,
                     line_header.checksum,
                     line_header.dex_location)) {
      *error = "Error when reading profile file line";
      return kProfileLoadBadData;
    }
    methods_left_to_read -= methods_to_read;
    classes_left_to_read -= classes_to_read;
  }
  return kProfileLoadSuccess;
}

bool ProfileCompilationInfo::Load(int fd) {
  std::string error;
  ProfileLoadSatus status = LoadInternal(fd, &error);

  if (status == kProfileLoadSuccess) {
    return true;
  } else {
    PLOG(WARNING) << "Error when reading profile " << error;
    return false;
  }
}

ProfileCompilationInfo::ProfileLoadSatus ProfileCompilationInfo::LoadInternal(
      int fd, std::string* error) {
  ScopedTrace trace(__PRETTY_FUNCTION__);
  DCHECK_GE(fd, 0);

  struct stat stat_buffer;
  if (fstat(fd, &stat_buffer) != 0) {
    return kProfileLoadIOError;
  }
  // We allow empty profile files.
  // Profiles may be created by ActivityManager or installd before we manage to
  // process them in the runtime or profman.
  if (stat_buffer.st_size == 0) {
    return kProfileLoadSuccess;
  }
  // Read profile header: magic + version + number_of_lines.
  uint16_t number_of_lines;
  ProfileLoadSatus status = ReadProfileHeader(fd, &number_of_lines, error);
  if (status != kProfileLoadSuccess) {
    return status;
  }

  while (number_of_lines > 0) {
    ProfileLineHeader line_header;
    // First, read the line header to get the amount of data we need to read.
    status = ReadProfileLineHeader(fd, &line_header, error);
    if (status != kProfileLoadSuccess) {
      return status;
    }

    // Now read the actual profile line.
    status = ReadProfileLine(fd, line_header, error);
    if (status != kProfileLoadSuccess) {
      return status;
    }
    number_of_lines--;
  }

  // Check that we read everything and that profiles don't contain junk data.
  int result = testEOF(fd);
  if (result == 0) {
    return kProfileLoadSuccess;
  } else if (result < 0) {
    return kProfileLoadIOError;
  } else {
    *error = "Unexpected content in the profile file";
    return kProfileLoadBadData;
  }
}

bool ProfileCompilationInfo::MergeWith(const ProfileCompilationInfo& other) {
  // First verify that all checksums match. This will avoid adding garbage to
  // the current profile info.
  // Note that the number of elements should be very small, so this should not
  // be a performance issue.
  for (const auto& other_it : other.info_) {
    auto info_it = info_.find(other_it.first);
    if ((info_it != info_.end()) && (info_it->second.checksum != other_it.second.checksum)) {
      LOG(WARNING) << "Checksum mismatch for dex " << other_it.first;
      return false;
    }
  }
  // All checksums match. Import the data.
  for (const auto& other_it : other.info_) {
    const std::string& other_dex_location = other_it.first;
    const DexFileData& other_dex_data = other_it.second;
    auto info_it = info_.find(other_dex_location);
    if (info_it == info_.end()) {
      info_it = info_.Put(other_dex_location, DexFileData(other_dex_data.checksum));
    }
    info_it->second.method_set.insert(other_dex_data.method_set.begin(),
                                      other_dex_data.method_set.end());
    info_it->second.class_set.insert(other_dex_data.class_set.begin(),
                                     other_dex_data.class_set.end());
  }
  return true;
}

bool ProfileCompilationInfo::ContainsMethod(const MethodReference& method_ref) const {
  auto info_it = info_.find(GetProfileDexFileKey(method_ref.dex_file->GetLocation()));
  if (info_it != info_.end()) {
    if (method_ref.dex_file->GetLocationChecksum() != info_it->second.checksum) {
      return false;
    }
    const std::set<uint16_t>& methods = info_it->second.method_set;
    return methods.find(method_ref.dex_method_index) != methods.end();
  }
  return false;
}

bool ProfileCompilationInfo::ContainsClass(const DexFile& dex_file, uint16_t class_def_idx) const {
  auto info_it = info_.find(GetProfileDexFileKey(dex_file.GetLocation()));
  if (info_it != info_.end()) {
    if (dex_file.GetLocationChecksum() != info_it->second.checksum) {
      return false;
    }
    const std::set<uint16_t>& classes = info_it->second.class_set;
    return classes.find(class_def_idx) != classes.end();
  }
  return false;
}

uint32_t ProfileCompilationInfo::GetNumberOfMethods() const {
  uint32_t total = 0;
  for (const auto& it : info_) {
    total += it.second.method_set.size();
  }
  return total;
}

uint32_t ProfileCompilationInfo::GetNumberOfResolvedClasses() const {
  uint32_t total = 0;
  for (const auto& it : info_) {
    total += it.second.class_set.size();
  }
  return total;
}

std::string ProfileCompilationInfo::DumpInfo(const std::vector<const DexFile*>* dex_files,
                                             bool print_full_dex_location) const {
  std::ostringstream os;
  if (info_.empty()) {
    return "ProfileInfo: empty";
  }

  os << "ProfileInfo:";

  const std::string kFirstDexFileKeySubstitute = ":classes.dex";
  for (const auto& it : info_) {
    os << "\n";
    const std::string& location = it.first;
    const DexFileData& dex_data = it.second;
    if (print_full_dex_location) {
      os << location;
    } else {
      // Replace the (empty) multidex suffix of the first key with a substitute for easier reading.
      std::string multidex_suffix = DexFile::GetMultiDexSuffix(location);
      os << (multidex_suffix.empty() ? kFirstDexFileKeySubstitute : multidex_suffix);
    }
    const DexFile* dex_file = nullptr;
    if (dex_files != nullptr) {
      for (size_t i = 0; i < dex_files->size(); i++) {
        if (location == (*dex_files)[i]->GetLocation()) {
          dex_file = (*dex_files)[i];
        }
      }
    }
    os << "\n\tmethods: ";
    for (const auto method_it : dex_data.method_set) {
      if (dex_file != nullptr) {
        os << "\n\t\t" << PrettyMethod(method_it, *dex_file, true);
      } else {
        os << method_it << ",";
      }
    }
    os << "\n\tclasses: ";
    for (const auto class_it : dex_data.class_set) {
      if (dex_file != nullptr) {
        os << "\n\t\t" << dex_file->GetClassDescriptor(dex_file->GetClassDef(class_it));
      } else {
        os << class_it << ",";
      }
    }
  }
  return os.str();
}

bool ProfileCompilationInfo::Equals(const ProfileCompilationInfo& other) {
  return info_.Equals(other.info_);
}

std::set<DexCacheResolvedClasses> ProfileCompilationInfo::GetResolvedClasses() const {
  std::set<DexCacheResolvedClasses> ret;
  for (auto&& pair : info_) {
    const std::string& profile_key = pair.first;
    const DexFileData& data = pair.second;
    // TODO: Is it OK to use the same location for both base and dex location here?
    DexCacheResolvedClasses classes(profile_key, profile_key, data.checksum);
    classes.AddClasses(data.class_set.begin(), data.class_set.end());
    ret.insert(classes);
  }
  return ret;
}

void ProfileCompilationInfo::ClearResolvedClasses() {
  for (auto& pair : info_) {
    pair.second.class_set.clear();
  }
}

}  // namespace art
