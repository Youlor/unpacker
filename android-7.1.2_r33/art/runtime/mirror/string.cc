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

#include "string-inl.h"

#include "arch/memcmp16.h"
#include "array.h"
#include "class-inl.h"
#include "gc/accounting/card_table-inl.h"
#include "handle_scope-inl.h"
#include "intern_table.h"
#include "object-inl.h"
#include "runtime.h"
#include "string-inl.h"
#include "thread.h"
#include "utf-inl.h"

namespace art {
namespace mirror {

// TODO: get global references for these
GcRoot<Class> String::java_lang_String_;

int32_t String::FastIndexOf(int32_t ch, int32_t start) {
  int32_t count = GetLength();
  if (start < 0) {
    start = 0;
  } else if (start > count) {
    start = count;
  }
  const uint16_t* chars = GetValue();
  const uint16_t* p = chars + start;
  const uint16_t* end = chars + count;
  while (p < end) {
    if (*p++ == ch) {
      return (p - 1) - chars;
    }
  }
  return -1;
}

void String::SetClass(Class* java_lang_String) {
  CHECK(java_lang_String_.IsNull());
  CHECK(java_lang_String != nullptr);
  CHECK(java_lang_String->IsStringClass());
  java_lang_String_ = GcRoot<Class>(java_lang_String);
}

void String::ResetClass() {
  CHECK(!java_lang_String_.IsNull());
  java_lang_String_ = GcRoot<Class>(nullptr);
}

int String::ComputeHashCode() {
  const int32_t hash_code = ComputeUtf16Hash(GetValue(), GetLength());
  SetHashCode(hash_code);
  return hash_code;
}

int32_t String::GetUtfLength() {
  return CountUtf8Bytes(GetValue(), GetLength());
}

void String::SetCharAt(int32_t index, uint16_t c) {
  DCHECK((index >= 0) && (index < count_));
  GetValue()[index] = c;
}

String* String::AllocFromStrings(Thread* self, Handle<String> string, Handle<String> string2) {
  int32_t length = string->GetLength();
  int32_t length2 = string2->GetLength();
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  SetStringCountVisitor visitor(length + length2);
  String* new_string = Alloc<true>(self, length + length2, allocator_type, visitor);
  if (UNLIKELY(new_string == nullptr)) {
    return nullptr;
  }
  uint16_t* new_value = new_string->GetValue();
  memcpy(new_value, string->GetValue(), length * sizeof(uint16_t));
  memcpy(new_value + length, string2->GetValue(), length2 * sizeof(uint16_t));
  return new_string;
}

String* String::AllocFromUtf16(Thread* self, int32_t utf16_length, const uint16_t* utf16_data_in) {
  CHECK(utf16_data_in != nullptr || utf16_length == 0);
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  SetStringCountVisitor visitor(utf16_length);
  String* string = Alloc<true>(self, utf16_length, allocator_type, visitor);
  if (UNLIKELY(string == nullptr)) {
    return nullptr;
  }
  uint16_t* array = string->GetValue();
  memcpy(array, utf16_data_in, utf16_length * sizeof(uint16_t));
  return string;
}

String* String::AllocFromModifiedUtf8(Thread* self, const char* utf) {
  DCHECK(utf != nullptr);
  size_t byte_count = strlen(utf);
  size_t char_count = CountModifiedUtf8Chars(utf, byte_count);
  return AllocFromModifiedUtf8(self, char_count, utf, byte_count);
}

String* String::AllocFromModifiedUtf8(Thread* self, int32_t utf16_length, const char* utf8_data_in) {
  return AllocFromModifiedUtf8(self, utf16_length, utf8_data_in, strlen(utf8_data_in));
}

String* String::AllocFromModifiedUtf8(Thread* self, int32_t utf16_length,
                                      const char* utf8_data_in, int32_t utf8_length) {
  gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
  SetStringCountVisitor visitor(utf16_length);
  String* string = Alloc<true>(self, utf16_length, allocator_type, visitor);
  if (UNLIKELY(string == nullptr)) {
    return nullptr;
  }
  uint16_t* utf16_data_out = string->GetValue();
  ConvertModifiedUtf8ToUtf16(utf16_data_out, utf16_length, utf8_data_in, utf8_length);
  return string;
}

bool String::Equals(String* that) {
  if (this == that) {
    // Quick reference equality test
    return true;
  } else if (that == nullptr) {
    // Null isn't an instanceof anything
    return false;
  } else if (this->GetLength() != that->GetLength()) {
    // Quick length inequality test
    return false;
  } else {
    // Note: don't short circuit on hash code as we're presumably here as the
    // hash code was already equal
    for (int32_t i = 0; i < that->GetLength(); ++i) {
      if (this->CharAt(i) != that->CharAt(i)) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const uint16_t* that_chars, int32_t that_offset, int32_t that_length) {
  if (this->GetLength() != that_length) {
    return false;
  } else {
    for (int32_t i = 0; i < that_length; ++i) {
      if (this->CharAt(i) != that_chars[that_offset + i]) {
        return false;
      }
    }
    return true;
  }
}

bool String::Equals(const char* modified_utf8) {
  const int32_t length = GetLength();
  int32_t i = 0;
  while (i < length) {
    const uint32_t ch = GetUtf16FromUtf8(&modified_utf8);
    if (ch == '\0') {
      return false;
    }

    if (GetLeadingUtf16Char(ch) != CharAt(i++)) {
      return false;
    }

    const uint16_t trailing = GetTrailingUtf16Char(ch);
    if (trailing != 0) {
      if (i == length) {
        return false;
      }

      if (CharAt(i++) != trailing) {
        return false;
      }
    }
  }
  return *modified_utf8 == '\0';
}

bool String::Equals(const StringPiece& modified_utf8) {
  const int32_t length = GetLength();
  const char* p = modified_utf8.data();
  for (int32_t i = 0; i < length; ++i) {
    uint32_t ch = GetUtf16FromUtf8(&p);

    if (GetLeadingUtf16Char(ch) != CharAt(i)) {
      return false;
    }

    const uint16_t trailing = GetTrailingUtf16Char(ch);
    if (trailing != 0) {
      if (i == (length - 1)) {
        return false;
      }

      if (CharAt(++i) != trailing) {
        return false;
      }
    }
  }
  return true;
}

// Create a modified UTF-8 encoded std::string from a java/lang/String object.
std::string String::ToModifiedUtf8() {
  const uint16_t* chars = GetValue();
  size_t byte_count = GetUtfLength();
  std::string result(byte_count, static_cast<char>(0));
  ConvertUtf16ToModifiedUtf8(&result[0], byte_count, chars, GetLength());
  return result;
}

int32_t String::CompareTo(String* rhs) {
  // Quick test for comparison of a string with itself.
  String* lhs = this;
  if (lhs == rhs) {
    return 0;
  }
  // TODO: is this still true?
  // The annoying part here is that 0x00e9 - 0xffff != 0x00ea,
  // because the interpreter converts the characters to 32-bit integers
  // *without* sign extension before it subtracts them (which makes some
  // sense since "char" is unsigned).  So what we get is the result of
  // 0x000000e9 - 0x0000ffff, which is 0xffff00ea.
  int32_t lhsCount = lhs->GetLength();
  int32_t rhsCount = rhs->GetLength();
  int32_t countDiff = lhsCount - rhsCount;
  int32_t minCount = (countDiff < 0) ? lhsCount : rhsCount;
  const uint16_t* lhsChars = lhs->GetValue();
  const uint16_t* rhsChars = rhs->GetValue();
  int32_t otherRes = MemCmp16(lhsChars, rhsChars, minCount);
  if (otherRes != 0) {
    return otherRes;
  }
  return countDiff;
}

void String::VisitRoots(RootVisitor* visitor) {
  java_lang_String_.VisitRootIfNonNull(visitor, RootInfo(kRootStickyClass));
}

CharArray* String::ToCharArray(Thread* self) {
  StackHandleScope<1> hs(self);
  Handle<String> string(hs.NewHandle(this));
  CharArray* result = CharArray::Alloc(self, GetLength());
  if (result != nullptr) {
    memcpy(result->GetData(), string->GetValue(), string->GetLength() * sizeof(uint16_t));
  } else {
    self->AssertPendingOOMException();
  }
  return result;
}

void String::GetChars(int32_t start, int32_t end, Handle<CharArray> array, int32_t index) {
  uint16_t* data = array->GetData() + index;
  uint16_t* value = GetValue() + start;
  memcpy(data, value, (end - start) * sizeof(uint16_t));
}

}  // namespace mirror
}  // namespace art
