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

#include "hex_dump.h"

#include "globals.h"

#include <string.h>

namespace art {

void HexDump::Dump(std::ostream& os) const {
  if (byte_count_ == 0) {
    return;
  }

  if (address_ == nullptr) {
    os << "00000000:";
    return;
  }

  static const char gHexDigit[] = "0123456789abcdef";
  const unsigned char* addr = reinterpret_cast<const unsigned char*>(address_);
  // 01234560: 00 11 22 33 44 55 66 77 88 99 aa bb cc dd ee ff  0123456789abcdef
  char out[(kBitsPerIntPtrT / 4) + /* offset */
           1 + /* colon */
           (16 * 3) + /* 16 hex digits and space */
           2 + /* white space */
           16 + /* 16 characters*/
           1 /* \0 */ ];
  size_t offset;    /* offset to show while printing */

  if (show_actual_addresses_) {
    offset = reinterpret_cast<size_t>(addr);
  } else {
    offset = 0;
  }
  memset(out, ' ', sizeof(out)-1);
  out[kBitsPerIntPtrT / 4] = ':';
  out[sizeof(out)-1] = '\0';

  size_t byte_count = byte_count_;
  size_t gap = offset & 0x0f;
  while (byte_count > 0) {
    size_t line_offset = offset & ~0x0f;

    char* hex = out;
    char* asc = out + (kBitsPerIntPtrT / 4) + /* offset */ 1 + /* colon */
        (16 * 3) + /* 16 hex digits and space */ 2 /* white space */;

    for (int i = 0; i < (kBitsPerIntPtrT / 4); i++) {
      *hex++ = gHexDigit[line_offset >> (kBitsPerIntPtrT - 4)];
      line_offset <<= 4;
    }
    hex++;
    hex++;

    size_t count = std::min(byte_count, 16 - gap);
    // CHECK_NE(count, 0U);
    // CHECK_LE(count + gap, 16U);

    if (gap) {
      /* only on first line */
      hex += gap * 3;
      asc += gap;
    }

    size_t i;
    for (i = gap ; i < count + gap; i++) {
      *hex++ = gHexDigit[*addr >> 4];
      *hex++ = gHexDigit[*addr & 0x0f];
      hex++;
      if (*addr >= 0x20 && *addr < 0x7f /*isprint(*addr)*/) {
        *asc++ = *addr;
      } else {
        *asc++ = '.';
      }
      addr++;
    }
    for (; i < 16; i++) {
      /* erase extra stuff; only happens on last line */
      *hex++ = ' ';
      *hex++ = ' ';
      hex++;
      *asc++ = ' ';
    }

    os << prefix_ << out;

    gap = 0;
    byte_count -= count;
    offset += count;
    if (byte_count > 0) {
      os << "\n";
    }
  }
}

}  // namespace art
