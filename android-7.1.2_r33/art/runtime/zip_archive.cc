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

#include "zip_archive.h"

#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "base/stringprintf.h"
#include "base/unix_file/fd_file.h"

namespace art {

uint32_t ZipEntry::GetUncompressedLength() {
  return zip_entry_->uncompressed_length;
}

uint32_t ZipEntry::GetCrc32() {
  return zip_entry_->crc32;
}

ZipEntry::~ZipEntry() {
  delete zip_entry_;
}

bool ZipEntry::ExtractToFile(File& file, std::string* error_msg) {
  const int32_t error = ExtractEntryToFile(handle_, zip_entry_, file.Fd());
  if (error) {
    *error_msg = std::string(ErrorCodeString(error));
    return false;
  }

  return true;
}

MemMap* ZipEntry::ExtractToMemMap(const char* zip_filename, const char* entry_filename,
                                  std::string* error_msg) {
  std::string name(entry_filename);
  name += " extracted in memory from ";
  name += zip_filename;
  std::unique_ptr<MemMap> map(MemMap::MapAnonymous(name.c_str(),
                                                   nullptr, GetUncompressedLength(),
                                                   PROT_READ | PROT_WRITE, false, false,
                                                   error_msg));
  if (map.get() == nullptr) {
    DCHECK(!error_msg->empty());
    return nullptr;
  }

  const int32_t error = ExtractToMemory(handle_, zip_entry_,
                                        map->Begin(), map->Size());
  if (error) {
    *error_msg = std::string(ErrorCodeString(error));
    return nullptr;
  }

  return map.release();
}

static void SetCloseOnExec(int fd) {
  // This dance is more portable than Linux's O_CLOEXEC open(2) flag.
  int flags = fcntl(fd, F_GETFD);
  if (flags == -1) {
    PLOG(WARNING) << "fcntl(" << fd << ", F_GETFD) failed";
    return;
  }
  int rc = fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  if (rc == -1) {
    PLOG(WARNING) << "fcntl(" << fd << ", F_SETFD, " << flags << ") failed";
    return;
  }
}

ZipArchive* ZipArchive::Open(const char* filename, std::string* error_msg) {
  DCHECK(filename != nullptr);

  ZipArchiveHandle handle;
  const int32_t error = OpenArchive(filename, &handle);
  if (error) {
    *error_msg = std::string(ErrorCodeString(error));
    CloseArchive(handle);
    return nullptr;
  }

  SetCloseOnExec(GetFileDescriptor(handle));
  return new ZipArchive(handle);
}

ZipArchive* ZipArchive::OpenFromFd(int fd, const char* filename, std::string* error_msg) {
  DCHECK(filename != nullptr);
  DCHECK_GT(fd, 0);

  ZipArchiveHandle handle;
  const int32_t error = OpenArchiveFd(fd, filename, &handle);
  if (error) {
    *error_msg = std::string(ErrorCodeString(error));
    CloseArchive(handle);
    return nullptr;
  }

  SetCloseOnExec(GetFileDescriptor(handle));
  return new ZipArchive(handle);
}

ZipEntry* ZipArchive::Find(const char* name, std::string* error_msg) const {
  DCHECK(name != nullptr);

  // Resist the urge to delete the space. <: is a bigraph sequence.
  std::unique_ptr< ::ZipEntry> zip_entry(new ::ZipEntry);
  const int32_t error = FindEntry(handle_, ZipString(name), zip_entry.get());
  if (error) {
    *error_msg = std::string(ErrorCodeString(error));
    return nullptr;
  }

  return new ZipEntry(handle_, zip_entry.release());
}

ZipArchive::~ZipArchive() {
  CloseArchive(handle_);
}

}  // namespace art
