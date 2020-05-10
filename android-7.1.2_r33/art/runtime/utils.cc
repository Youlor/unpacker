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

#include "utils.h"

#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <memory>

#include "art_field-inl.h"
#include "art_method-inl.h"
#include "base/stl_util.h"
#include "base/unix_file/fd_file.h"
#include "dex_file-inl.h"
#include "dex_instruction.h"
#include "mirror/class-inl.h"
#include "mirror/class_loader.h"
#include "mirror/object-inl.h"
#include "mirror/object_array-inl.h"
#include "mirror/string.h"
#include "oat_quick_method_header.h"
#include "os.h"
#include "scoped_thread_state_change.h"
#include "utf-inl.h"

#if defined(__APPLE__)
#include "AvailabilityMacros.h"  // For MAC_OS_X_VERSION_MAX_ALLOWED
#include <sys/syscall.h>
#endif

// For DumpNativeStack.
#include <backtrace/Backtrace.h>
#include <backtrace/BacktraceMap.h>

#if defined(__linux__)
#include <linux/unistd.h>
#endif

namespace art {

#if defined(__linux__)
static constexpr bool kUseAddr2line = !kIsTargetBuild;
#endif

pid_t GetTid() {
#if defined(__APPLE__)
  uint64_t owner;
  CHECK_PTHREAD_CALL(pthread_threadid_np, (nullptr, &owner), __FUNCTION__);  // Requires Mac OS 10.6
  return owner;
#elif defined(__BIONIC__)
  return gettid();
#else
  return syscall(__NR_gettid);
#endif
}

std::string GetThreadName(pid_t tid) {
  std::string result;
  if (ReadFileToString(StringPrintf("/proc/self/task/%d/comm", tid), &result)) {
    result.resize(result.size() - 1);  // Lose the trailing '\n'.
  } else {
    result = "<unknown>";
  }
  return result;
}

void GetThreadStack(pthread_t thread, void** stack_base, size_t* stack_size, size_t* guard_size) {
#if defined(__APPLE__)
  *stack_size = pthread_get_stacksize_np(thread);
  void* stack_addr = pthread_get_stackaddr_np(thread);

  // Check whether stack_addr is the base or end of the stack.
  // (On Mac OS 10.7, it's the end.)
  int stack_variable;
  if (stack_addr > &stack_variable) {
    *stack_base = reinterpret_cast<uint8_t*>(stack_addr) - *stack_size;
  } else {
    *stack_base = stack_addr;
  }

  // This is wrong, but there doesn't seem to be a way to get the actual value on the Mac.
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_attr_init, (&attributes), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getguardsize, (&attributes, guard_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);
#else
  pthread_attr_t attributes;
  CHECK_PTHREAD_CALL(pthread_getattr_np, (thread, &attributes), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getstack, (&attributes, stack_base, stack_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_getguardsize, (&attributes, guard_size), __FUNCTION__);
  CHECK_PTHREAD_CALL(pthread_attr_destroy, (&attributes), __FUNCTION__);

#if defined(__GLIBC__)
  // If we're the main thread, check whether we were run with an unlimited stack. In that case,
  // glibc will have reported a 2GB stack for our 32-bit process, and our stack overflow detection
  // will be broken because we'll die long before we get close to 2GB.
  bool is_main_thread = (::art::GetTid() == getpid());
  if (is_main_thread) {
    rlimit stack_limit;
    if (getrlimit(RLIMIT_STACK, &stack_limit) == -1) {
      PLOG(FATAL) << "getrlimit(RLIMIT_STACK) failed";
    }
    if (stack_limit.rlim_cur == RLIM_INFINITY) {
      size_t old_stack_size = *stack_size;

      // Use the kernel default limit as our size, and adjust the base to match.
      *stack_size = 8 * MB;
      *stack_base = reinterpret_cast<uint8_t*>(*stack_base) + (old_stack_size - *stack_size);

      VLOG(threads) << "Limiting unlimited stack (reported as " << PrettySize(old_stack_size) << ")"
                    << " to " << PrettySize(*stack_size)
                    << " with base " << *stack_base;
    }
  }
#endif

#endif
}

bool ReadFileToString(const std::string& file_name, std::string* result) {
  File file;
  if (!file.Open(file_name, O_RDONLY)) {
    return false;
  }

  std::vector<char> buf(8 * KB);
  while (true) {
    int64_t n = TEMP_FAILURE_RETRY(read(file.Fd(), &buf[0], buf.size()));
    if (n == -1) {
      return false;
    }
    if (n == 0) {
      return true;
    }
    result->append(&buf[0], n);
  }
}

bool PrintFileToLog(const std::string& file_name, LogSeverity level) {
  File file;
  if (!file.Open(file_name, O_RDONLY)) {
    return false;
  }

  constexpr size_t kBufSize = 256;  // Small buffer. Avoid stack overflow and stack size warnings.
  char buf[kBufSize + 1];           // +1 for terminator.
  size_t filled_to = 0;
  while (true) {
    DCHECK_LT(filled_to, kBufSize);
    int64_t n = TEMP_FAILURE_RETRY(read(file.Fd(), &buf[filled_to], kBufSize - filled_to));
    if (n <= 0) {
      // Print the rest of the buffer, if it exists.
      if (filled_to > 0) {
        buf[filled_to] = 0;
        LOG(level) << buf;
      }
      return n == 0;
    }
    // Scan for '\n'.
    size_t i = filled_to;
    bool found_newline = false;
    for (; i < filled_to + n; ++i) {
      if (buf[i] == '\n') {
        // Found a line break, that's something to print now.
        buf[i] = 0;
        LOG(level) << buf;
        // Copy the rest to the front.
        if (i + 1 < filled_to + n) {
          memmove(&buf[0], &buf[i + 1], filled_to + n - i - 1);
          filled_to = filled_to + n - i - 1;
        } else {
          filled_to = 0;
        }
        found_newline = true;
        break;
      }
    }
    if (found_newline) {
      continue;
    } else {
      filled_to += n;
      // Check if we must flush now.
      if (filled_to == kBufSize) {
        buf[kBufSize] = 0;
        LOG(level) << buf;
        filled_to = 0;
      }
    }
  }
}

std::string PrettyDescriptor(mirror::String* java_descriptor) {
  if (java_descriptor == nullptr) {
    return "null";
  }
  return PrettyDescriptor(java_descriptor->ToModifiedUtf8().c_str());
}

std::string PrettyDescriptor(mirror::Class* klass) {
  if (klass == nullptr) {
    return "null";
  }
  std::string temp;
  return PrettyDescriptor(klass->GetDescriptor(&temp));
}

std::string PrettyDescriptor(const char* descriptor) {
  // Count the number of '['s to get the dimensionality.
  const char* c = descriptor;
  size_t dim = 0;
  while (*c == '[') {
    dim++;
    c++;
  }

  // Reference or primitive?
  if (*c == 'L') {
    // "[[La/b/C;" -> "a.b.C[][]".
    c++;  // Skip the 'L'.
  } else {
    // "[[B" -> "byte[][]".
    // To make life easier, we make primitives look like unqualified
    // reference types.
    switch (*c) {
    case 'B': c = "byte;"; break;
    case 'C': c = "char;"; break;
    case 'D': c = "double;"; break;
    case 'F': c = "float;"; break;
    case 'I': c = "int;"; break;
    case 'J': c = "long;"; break;
    case 'S': c = "short;"; break;
    case 'Z': c = "boolean;"; break;
    case 'V': c = "void;"; break;  // Used when decoding return types.
    default: return descriptor;
    }
  }

  // At this point, 'c' is a string of the form "fully/qualified/Type;"
  // or "primitive;". Rewrite the type with '.' instead of '/':
  std::string result;
  const char* p = c;
  while (*p != ';') {
    char ch = *p++;
    if (ch == '/') {
      ch = '.';
    }
    result.push_back(ch);
  }
  // ...and replace the semicolon with 'dim' "[]" pairs:
  for (size_t i = 0; i < dim; ++i) {
    result += "[]";
  }
  return result;
}

std::string PrettyField(ArtField* f, bool with_type) {
  if (f == nullptr) {
    return "null";
  }
  std::string result;
  if (with_type) {
    result += PrettyDescriptor(f->GetTypeDescriptor());
    result += ' ';
  }
  std::string temp;
  result += PrettyDescriptor(f->GetDeclaringClass()->GetDescriptor(&temp));
  result += '.';
  result += f->GetName();
  return result;
}

std::string PrettyField(uint32_t field_idx, const DexFile& dex_file, bool with_type) {
  if (field_idx >= dex_file.NumFieldIds()) {
    return StringPrintf("<<invalid-field-idx-%d>>", field_idx);
  }
  const DexFile::FieldId& field_id = dex_file.GetFieldId(field_idx);
  std::string result;
  if (with_type) {
    result += dex_file.GetFieldTypeDescriptor(field_id);
    result += ' ';
  }
  result += PrettyDescriptor(dex_file.GetFieldDeclaringClassDescriptor(field_id));
  result += '.';
  result += dex_file.GetFieldName(field_id);
  return result;
}

std::string PrettyType(uint32_t type_idx, const DexFile& dex_file) {
  if (type_idx >= dex_file.NumTypeIds()) {
    return StringPrintf("<<invalid-type-idx-%d>>", type_idx);
  }
  const DexFile::TypeId& type_id = dex_file.GetTypeId(type_idx);
  return PrettyDescriptor(dex_file.GetTypeDescriptor(type_id));
}

std::string PrettyArguments(const char* signature) {
  std::string result;
  result += '(';
  CHECK_EQ(*signature, '(');
  ++signature;  // Skip the '('.
  while (*signature != ')') {
    size_t argument_length = 0;
    while (signature[argument_length] == '[') {
      ++argument_length;
    }
    if (signature[argument_length] == 'L') {
      argument_length = (strchr(signature, ';') - signature + 1);
    } else {
      ++argument_length;
    }
    {
      std::string argument_descriptor(signature, argument_length);
      result += PrettyDescriptor(argument_descriptor.c_str());
    }
    if (signature[argument_length] != ')') {
      result += ", ";
    }
    signature += argument_length;
  }
  CHECK_EQ(*signature, ')');
  ++signature;  // Skip the ')'.
  result += ')';
  return result;
}

std::string PrettyReturnType(const char* signature) {
  const char* return_type = strchr(signature, ')');
  CHECK(return_type != nullptr);
  ++return_type;  // Skip ')'.
  return PrettyDescriptor(return_type);
}

std::string PrettyMethod(ArtMethod* m, bool with_signature) {
  if (m == nullptr) {
    return "null";
  }
  if (!m->IsRuntimeMethod()) {
    m = m->GetInterfaceMethodIfProxy(Runtime::Current()->GetClassLinker()->GetImagePointerSize());
  }
  std::string result(PrettyDescriptor(m->GetDeclaringClassDescriptor()));
  result += '.';
  result += m->GetName();
  if (UNLIKELY(m->IsFastNative())) {
    result += "!";
  }
  if (with_signature) {
    const Signature signature = m->GetSignature();
    std::string sig_as_string(signature.ToString());
    if (signature == Signature::NoSignature()) {
      return result + sig_as_string;
    }
    result = PrettyReturnType(sig_as_string.c_str()) + " " + result +
        PrettyArguments(sig_as_string.c_str());
  }
  return result;
}

std::string PrettyMethod(uint32_t method_idx, const DexFile& dex_file, bool with_signature) {
  if (method_idx >= dex_file.NumMethodIds()) {
    return StringPrintf("<<invalid-method-idx-%d>>", method_idx);
  }
  const DexFile::MethodId& method_id = dex_file.GetMethodId(method_idx);
  std::string result(PrettyDescriptor(dex_file.GetMethodDeclaringClassDescriptor(method_id)));
  result += '.';
  result += dex_file.GetMethodName(method_id);
  if (with_signature) {
    const Signature signature = dex_file.GetMethodSignature(method_id);
    std::string sig_as_string(signature.ToString());
    if (signature == Signature::NoSignature()) {
      return result + sig_as_string;
    }
    result = PrettyReturnType(sig_as_string.c_str()) + " " + result +
        PrettyArguments(sig_as_string.c_str());
  }
  return result;
}

std::string PrettyTypeOf(mirror::Object* obj) {
  if (obj == nullptr) {
    return "null";
  }
  if (obj->GetClass() == nullptr) {
    return "(raw)";
  }
  std::string temp;
  std::string result(PrettyDescriptor(obj->GetClass()->GetDescriptor(&temp)));
  if (obj->IsClass()) {
    result += "<" + PrettyDescriptor(obj->AsClass()->GetDescriptor(&temp)) + ">";
  }
  return result;
}

std::string PrettyClass(mirror::Class* c) {
  if (c == nullptr) {
    return "null";
  }
  std::string result;
  result += "java.lang.Class<";
  result += PrettyDescriptor(c);
  result += ">";
  return result;
}

std::string PrettyClassAndClassLoader(mirror::Class* c) {
  if (c == nullptr) {
    return "null";
  }
  std::string result;
  result += "java.lang.Class<";
  result += PrettyDescriptor(c);
  result += ",";
  result += PrettyTypeOf(c->GetClassLoader());
  // TODO: add an identifying hash value for the loader
  result += ">";
  return result;
}

std::string PrettyJavaAccessFlags(uint32_t access_flags) {
  std::string result;
  if ((access_flags & kAccPublic) != 0) {
    result += "public ";
  }
  if ((access_flags & kAccProtected) != 0) {
    result += "protected ";
  }
  if ((access_flags & kAccPrivate) != 0) {
    result += "private ";
  }
  if ((access_flags & kAccFinal) != 0) {
    result += "final ";
  }
  if ((access_flags & kAccStatic) != 0) {
    result += "static ";
  }
  if ((access_flags & kAccTransient) != 0) {
    result += "transient ";
  }
  if ((access_flags & kAccVolatile) != 0) {
    result += "volatile ";
  }
  if ((access_flags & kAccSynchronized) != 0) {
    result += "synchronized ";
  }
  return result;
}

std::string PrettySize(int64_t byte_count) {
  // The byte thresholds at which we display amounts.  A byte count is displayed
  // in unit U when kUnitThresholds[U] <= bytes < kUnitThresholds[U+1].
  static const int64_t kUnitThresholds[] = {
    0,              // B up to...
    3*1024,         // KB up to...
    2*1024*1024,    // MB up to...
    1024*1024*1024  // GB from here.
  };
  static const int64_t kBytesPerUnit[] = { 1, KB, MB, GB };
  static const char* const kUnitStrings[] = { "B", "KB", "MB", "GB" };
  const char* negative_str = "";
  if (byte_count < 0) {
    negative_str = "-";
    byte_count = -byte_count;
  }
  int i = arraysize(kUnitThresholds);
  while (--i > 0) {
    if (byte_count >= kUnitThresholds[i]) {
      break;
    }
  }
  return StringPrintf("%s%" PRId64 "%s",
                      negative_str, byte_count / kBytesPerUnit[i], kUnitStrings[i]);
}

std::string PrintableChar(uint16_t ch) {
  std::string result;
  result += '\'';
  if (NeedsEscaping(ch)) {
    StringAppendF(&result, "\\u%04x", ch);
  } else {
    result += ch;
  }
  result += '\'';
  return result;
}

std::string PrintableString(const char* utf) {
  std::string result;
  result += '"';
  const char* p = utf;
  size_t char_count = CountModifiedUtf8Chars(p);
  for (size_t i = 0; i < char_count; ++i) {
    uint32_t ch = GetUtf16FromUtf8(&p);
    if (ch == '\\') {
      result += "\\\\";
    } else if (ch == '\n') {
      result += "\\n";
    } else if (ch == '\r') {
      result += "\\r";
    } else if (ch == '\t') {
      result += "\\t";
    } else {
      const uint16_t leading = GetLeadingUtf16Char(ch);

      if (NeedsEscaping(leading)) {
        StringAppendF(&result, "\\u%04x", leading);
      } else {
        result += leading;
      }

      const uint32_t trailing = GetTrailingUtf16Char(ch);
      if (trailing != 0) {
        // All high surrogates will need escaping.
        StringAppendF(&result, "\\u%04x", trailing);
      }
    }
  }
  result += '"';
  return result;
}

// See http://java.sun.com/j2se/1.5.0/docs/guide/jni/spec/design.html#wp615 for the full rules.
std::string MangleForJni(const std::string& s) {
  std::string result;
  size_t char_count = CountModifiedUtf8Chars(s.c_str());
  const char* cp = &s[0];
  for (size_t i = 0; i < char_count; ++i) {
    uint32_t ch = GetUtf16FromUtf8(&cp);
    if ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
      result.push_back(ch);
    } else if (ch == '.' || ch == '/') {
      result += "_";
    } else if (ch == '_') {
      result += "_1";
    } else if (ch == ';') {
      result += "_2";
    } else if (ch == '[') {
      result += "_3";
    } else {
      const uint16_t leading = GetLeadingUtf16Char(ch);
      const uint32_t trailing = GetTrailingUtf16Char(ch);

      StringAppendF(&result, "_0%04x", leading);
      if (trailing != 0) {
        StringAppendF(&result, "_0%04x", trailing);
      }
    }
  }
  return result;
}

std::string DotToDescriptor(const char* class_name) {
  std::string descriptor(class_name);
  std::replace(descriptor.begin(), descriptor.end(), '.', '/');
  if (descriptor.length() > 0 && descriptor[0] != '[') {
    descriptor = "L" + descriptor + ";";
  }
  return descriptor;
}

std::string DescriptorToDot(const char* descriptor) {
  size_t length = strlen(descriptor);
  if (length > 1) {
    if (descriptor[0] == 'L' && descriptor[length - 1] == ';') {
      // Descriptors have the leading 'L' and trailing ';' stripped.
      std::string result(descriptor + 1, length - 2);
      std::replace(result.begin(), result.end(), '/', '.');
      return result;
    } else {
      // For arrays the 'L' and ';' remain intact.
      std::string result(descriptor);
      std::replace(result.begin(), result.end(), '/', '.');
      return result;
    }
  }
  // Do nothing for non-class/array descriptors.
  return descriptor;
}

std::string DescriptorToName(const char* descriptor) {
  size_t length = strlen(descriptor);
  if (descriptor[0] == 'L' && descriptor[length - 1] == ';') {
    std::string result(descriptor + 1, length - 2);
    return result;
  }
  return descriptor;
}

std::string JniShortName(ArtMethod* m) {
  std::string class_name(m->GetDeclaringClassDescriptor());
  // Remove the leading 'L' and trailing ';'...
  CHECK_EQ(class_name[0], 'L') << class_name;
  CHECK_EQ(class_name[class_name.size() - 1], ';') << class_name;
  class_name.erase(0, 1);
  class_name.erase(class_name.size() - 1, 1);

  std::string method_name(m->GetName());

  std::string short_name;
  short_name += "Java_";
  short_name += MangleForJni(class_name);
  short_name += "_";
  short_name += MangleForJni(method_name);
  return short_name;
}

std::string JniLongName(ArtMethod* m) {
  std::string long_name;
  long_name += JniShortName(m);
  long_name += "__";

  std::string signature(m->GetSignature().ToString());
  signature.erase(0, 1);
  signature.erase(signature.begin() + signature.find(')'), signature.end());

  long_name += MangleForJni(signature);

  return long_name;
}

// Helper for IsValidPartOfMemberNameUtf8(), a bit vector indicating valid low ascii.
uint32_t DEX_MEMBER_VALID_LOW_ASCII[4] = {
  0x00000000,  // 00..1f low control characters; nothing valid
  0x03ff2010,  // 20..3f digits and symbols; valid: '0'..'9', '$', '-'
  0x87fffffe,  // 40..5f uppercase etc.; valid: 'A'..'Z', '_'
  0x07fffffe   // 60..7f lowercase etc.; valid: 'a'..'z'
};

// Helper for IsValidPartOfMemberNameUtf8(); do not call directly.
bool IsValidPartOfMemberNameUtf8Slow(const char** pUtf8Ptr) {
  /*
   * It's a multibyte encoded character. Decode it and analyze. We
   * accept anything that isn't (a) an improperly encoded low value,
   * (b) an improper surrogate pair, (c) an encoded '\0', (d) a high
   * control character, or (e) a high space, layout, or special
   * character (U+00a0, U+2000..U+200f, U+2028..U+202f,
   * U+fff0..U+ffff). This is all specified in the dex format
   * document.
   */

  const uint32_t pair = GetUtf16FromUtf8(pUtf8Ptr);
  const uint16_t leading = GetLeadingUtf16Char(pair);

  // We have a surrogate pair resulting from a valid 4 byte UTF sequence.
  // No further checks are necessary because 4 byte sequences span code
  // points [U+10000, U+1FFFFF], which are valid codepoints in a dex
  // identifier. Furthermore, GetUtf16FromUtf8 guarantees that each of
  // the surrogate halves are valid and well formed in this instance.
  if (GetTrailingUtf16Char(pair) != 0) {
    return true;
  }


  // We've encountered a one, two or three byte UTF-8 sequence. The
  // three byte UTF-8 sequence could be one half of a surrogate pair.
  switch (leading >> 8) {
    case 0x00:
      // It's only valid if it's above the ISO-8859-1 high space (0xa0).
      return (leading > 0x00a0);
    case 0xd8:
    case 0xd9:
    case 0xda:
    case 0xdb:
      {
        // We found a three byte sequence encoding one half of a surrogate.
        // Look for the other half.
        const uint32_t pair2 = GetUtf16FromUtf8(pUtf8Ptr);
        const uint16_t trailing = GetLeadingUtf16Char(pair2);

        return (GetTrailingUtf16Char(pair2) == 0) && (0xdc00 <= trailing && trailing <= 0xdfff);
      }
    case 0xdc:
    case 0xdd:
    case 0xde:
    case 0xdf:
      // It's a trailing surrogate, which is not valid at this point.
      return false;
    case 0x20:
    case 0xff:
      // It's in the range that has spaces, controls, and specials.
      switch (leading & 0xfff8) {
        case 0x2000:
        case 0x2008:
        case 0x2028:
        case 0xfff0:
        case 0xfff8:
          return false;
      }
      return true;
    default:
      return true;
  }

  UNREACHABLE();
}

/* Return whether the pointed-at modified-UTF-8 encoded character is
 * valid as part of a member name, updating the pointer to point past
 * the consumed character. This will consume two encoded UTF-16 code
 * points if the character is encoded as a surrogate pair. Also, if
 * this function returns false, then the given pointer may only have
 * been partially advanced.
 */
static bool IsValidPartOfMemberNameUtf8(const char** pUtf8Ptr) {
  uint8_t c = (uint8_t) **pUtf8Ptr;
  if (LIKELY(c <= 0x7f)) {
    // It's low-ascii, so check the table.
    uint32_t wordIdx = c >> 5;
    uint32_t bitIdx = c & 0x1f;
    (*pUtf8Ptr)++;
    return (DEX_MEMBER_VALID_LOW_ASCII[wordIdx] & (1 << bitIdx)) != 0;
  }

  // It's a multibyte encoded character. Call a non-inline function
  // for the heavy lifting.
  return IsValidPartOfMemberNameUtf8Slow(pUtf8Ptr);
}

bool IsValidMemberName(const char* s) {
  bool angle_name = false;

  switch (*s) {
    case '\0':
      // The empty string is not a valid name.
      return false;
    case '<':
      angle_name = true;
      s++;
      break;
  }

  while (true) {
    switch (*s) {
      case '\0':
        return !angle_name;
      case '>':
        return angle_name && s[1] == '\0';
    }

    if (!IsValidPartOfMemberNameUtf8(&s)) {
      return false;
    }
  }
}

enum ClassNameType { kName, kDescriptor };
template<ClassNameType kType, char kSeparator>
static bool IsValidClassName(const char* s) {
  int arrayCount = 0;
  while (*s == '[') {
    arrayCount++;
    s++;
  }

  if (arrayCount > 255) {
    // Arrays may have no more than 255 dimensions.
    return false;
  }

  ClassNameType type = kType;
  if (type != kDescriptor && arrayCount != 0) {
    /*
     * If we're looking at an array of some sort, then it doesn't
     * matter if what is being asked for is a class name; the
     * format looks the same as a type descriptor in that case, so
     * treat it as such.
     */
    type = kDescriptor;
  }

  if (type == kDescriptor) {
    /*
     * We are looking for a descriptor. Either validate it as a
     * single-character primitive type, or continue on to check the
     * embedded class name (bracketed by "L" and ";").
     */
    switch (*(s++)) {
    case 'B':
    case 'C':
    case 'D':
    case 'F':
    case 'I':
    case 'J':
    case 'S':
    case 'Z':
      // These are all single-character descriptors for primitive types.
      return (*s == '\0');
    case 'V':
      // Non-array void is valid, but you can't have an array of void.
      return (arrayCount == 0) && (*s == '\0');
    case 'L':
      // Class name: Break out and continue below.
      break;
    default:
      // Oddball descriptor character.
      return false;
    }
  }

  /*
   * We just consumed the 'L' that introduces a class name as part
   * of a type descriptor, or we are looking for an unadorned class
   * name.
   */

  bool sepOrFirst = true;  // first character or just encountered a separator.
  for (;;) {
    uint8_t c = (uint8_t) *s;
    switch (c) {
    case '\0':
      /*
       * Premature end for a type descriptor, but valid for
       * a class name as long as we haven't encountered an
       * empty component (including the degenerate case of
       * the empty string "").
       */
      return (type == kName) && !sepOrFirst;
    case ';':
      /*
       * Invalid character for a class name, but the
       * legitimate end of a type descriptor. In the latter
       * case, make sure that this is the end of the string
       * and that it doesn't end with an empty component
       * (including the degenerate case of "L;").
       */
      return (type == kDescriptor) && !sepOrFirst && (s[1] == '\0');
    case '/':
    case '.':
      if (c != kSeparator) {
        // The wrong separator character.
        return false;
      }
      if (sepOrFirst) {
        // Separator at start or two separators in a row.
        return false;
      }
      sepOrFirst = true;
      s++;
      break;
    default:
      if (!IsValidPartOfMemberNameUtf8(&s)) {
        return false;
      }
      sepOrFirst = false;
      break;
    }
  }
}

bool IsValidBinaryClassName(const char* s) {
  return IsValidClassName<kName, '.'>(s);
}

bool IsValidJniClassName(const char* s) {
  return IsValidClassName<kName, '/'>(s);
}

bool IsValidDescriptor(const char* s) {
  return IsValidClassName<kDescriptor, '/'>(s);
}

void Split(const std::string& s, char separator, std::vector<std::string>* result) {
  const char* p = s.data();
  const char* end = p + s.size();
  while (p != end) {
    if (*p == separator) {
      ++p;
    } else {
      const char* start = p;
      while (++p != end && *p != separator) {
        // Skip to the next occurrence of the separator.
      }
      result->push_back(std::string(start, p - start));
    }
  }
}

std::string Trim(const std::string& s) {
  std::string result;
  unsigned int start_index = 0;
  unsigned int end_index = s.size() - 1;

  // Skip initial whitespace.
  while (start_index < s.size()) {
    if (!isspace(s[start_index])) {
      break;
    }
    start_index++;
  }

  // Skip terminating whitespace.
  while (end_index >= start_index) {
    if (!isspace(s[end_index])) {
      break;
    }
    end_index--;
  }

  // All spaces, no beef.
  if (end_index < start_index) {
    return "";
  }
  // Start_index is the first non-space, end_index is the last one.
  return s.substr(start_index, end_index - start_index + 1);
}

template <typename StringT>
std::string Join(const std::vector<StringT>& strings, char separator) {
  if (strings.empty()) {
    return "";
  }

  std::string result(strings[0]);
  for (size_t i = 1; i < strings.size(); ++i) {
    result += separator;
    result += strings[i];
  }
  return result;
}

// Explicit instantiations.
template std::string Join<std::string>(const std::vector<std::string>& strings, char separator);
template std::string Join<const char*>(const std::vector<const char*>& strings, char separator);

bool StartsWith(const std::string& s, const char* prefix) {
  return s.compare(0, strlen(prefix), prefix) == 0;
}

bool EndsWith(const std::string& s, const char* suffix) {
  size_t suffix_length = strlen(suffix);
  size_t string_length = s.size();
  if (suffix_length > string_length) {
    return false;
  }
  size_t offset = string_length - suffix_length;
  return s.compare(offset, suffix_length, suffix) == 0;
}

void SetThreadName(const char* thread_name) {
  int hasAt = 0;
  int hasDot = 0;
  const char* s = thread_name;
  while (*s) {
    if (*s == '.') {
      hasDot = 1;
    } else if (*s == '@') {
      hasAt = 1;
    }
    s++;
  }
  int len = s - thread_name;
  if (len < 15 || hasAt || !hasDot) {
    s = thread_name;
  } else {
    s = thread_name + len - 15;
  }
#if defined(__linux__)
  // pthread_setname_np fails rather than truncating long strings.
  char buf[16];       // MAX_TASK_COMM_LEN=16 is hard-coded in the kernel.
  strncpy(buf, s, sizeof(buf)-1);
  buf[sizeof(buf)-1] = '\0';
  errno = pthread_setname_np(pthread_self(), buf);
  if (errno != 0) {
    PLOG(WARNING) << "Unable to set the name of current thread to '" << buf << "'";
  }
#else  // __APPLE__
  pthread_setname_np(thread_name);
#endif
}

void GetTaskStats(pid_t tid, char* state, int* utime, int* stime, int* task_cpu) {
  *utime = *stime = *task_cpu = 0;
  std::string stats;
  if (!ReadFileToString(StringPrintf("/proc/self/task/%d/stat", tid), &stats)) {
    return;
  }
  // Skip the command, which may contain spaces.
  stats = stats.substr(stats.find(')') + 2);
  // Extract the three fields we care about.
  std::vector<std::string> fields;
  Split(stats, ' ', &fields);
  *state = fields[0][0];
  *utime = strtoull(fields[11].c_str(), nullptr, 10);
  *stime = strtoull(fields[12].c_str(), nullptr, 10);
  *task_cpu = strtoull(fields[36].c_str(), nullptr, 10);
}

std::string GetSchedulerGroupName(pid_t tid) {
  // /proc/<pid>/cgroup looks like this:
  // 2:devices:/
  // 1:cpuacct,cpu:/
  // We want the third field from the line whose second field contains the "cpu" token.
  std::string cgroup_file;
  if (!ReadFileToString(StringPrintf("/proc/self/task/%d/cgroup", tid), &cgroup_file)) {
    return "";
  }
  std::vector<std::string> cgroup_lines;
  Split(cgroup_file, '\n', &cgroup_lines);
  for (size_t i = 0; i < cgroup_lines.size(); ++i) {
    std::vector<std::string> cgroup_fields;
    Split(cgroup_lines[i], ':', &cgroup_fields);
    std::vector<std::string> cgroups;
    Split(cgroup_fields[1], ',', &cgroups);
    for (size_t j = 0; j < cgroups.size(); ++j) {
      if (cgroups[j] == "cpu") {
        return cgroup_fields[2].substr(1);  // Skip the leading slash.
      }
    }
  }
  return "";
}

#if defined(__linux__)

ALWAYS_INLINE
static inline void WritePrefix(std::ostream* os, const char* prefix, bool odd) {
  if (prefix != nullptr) {
    *os << prefix;
  }
  *os << "  ";
  if (!odd) {
    *os << " ";
  }
}

static bool RunCommand(std::string cmd, std::ostream* os, const char* prefix) {
  FILE* stream = popen(cmd.c_str(), "r");
  if (stream) {
    if (os != nullptr) {
      bool odd_line = true;               // We indent them differently.
      bool wrote_prefix = false;          // Have we already written a prefix?
      constexpr size_t kMaxBuffer = 128;  // Relatively small buffer. Should be OK as we're on an
                                          // alt stack, but just to be sure...
      char buffer[kMaxBuffer];
      while (!feof(stream)) {
        if (fgets(buffer, kMaxBuffer, stream) != nullptr) {
          // Split on newlines.
          char* tmp = buffer;
          for (;;) {
            char* new_line = strchr(tmp, '\n');
            if (new_line == nullptr) {
              // Print the rest.
              if (*tmp != 0) {
                if (!wrote_prefix) {
                  WritePrefix(os, prefix, odd_line);
                }
                wrote_prefix = true;
                *os << tmp;
              }
              break;
            }
            if (!wrote_prefix) {
              WritePrefix(os, prefix, odd_line);
            }
            char saved = *(new_line + 1);
            *(new_line + 1) = 0;
            *os << tmp;
            *(new_line + 1) = saved;
            tmp = new_line + 1;
            odd_line = !odd_line;
            wrote_prefix = false;
          }
        }
      }
    }
    pclose(stream);
    return true;
  } else {
    return false;
  }
}

static void Addr2line(const std::string& map_src, uintptr_t offset, std::ostream& os,
                      const char* prefix) {
  std::string cmdline(StringPrintf("addr2line --functions --inlines --demangle -e %s %zx",
                                   map_src.c_str(), offset));
  RunCommand(cmdline.c_str(), &os, prefix);
}

static bool PcIsWithinQuickCode(ArtMethod* method, uintptr_t pc) NO_THREAD_SAFETY_ANALYSIS {
  uintptr_t code = reinterpret_cast<uintptr_t>(EntryPointToCodePointer(
      method->GetEntryPointFromQuickCompiledCode()));
  if (code == 0) {
    return pc == 0;
  }
  uintptr_t code_size = reinterpret_cast<const OatQuickMethodHeader*>(code)[-1].code_size_;
  return code <= pc && pc <= (code + code_size);
}
#endif

void DumpNativeStack(std::ostream& os, pid_t tid, BacktraceMap* existing_map, const char* prefix,
    ArtMethod* current_method, void* ucontext_ptr) {
#if __linux__
  // b/18119146
  if (RUNNING_ON_MEMORY_TOOL != 0) {
    return;
  }

  BacktraceMap* map = existing_map;
  std::unique_ptr<BacktraceMap> tmp_map;
  if (map == nullptr) {
    tmp_map.reset(BacktraceMap::Create(getpid()));
    map = tmp_map.get();
  }
  std::unique_ptr<Backtrace> backtrace(Backtrace::Create(BACKTRACE_CURRENT_PROCESS, tid, map));
  if (!backtrace->Unwind(0, reinterpret_cast<ucontext*>(ucontext_ptr))) {
    os << prefix << "(backtrace::Unwind failed for thread " << tid
       << ": " <<  backtrace->GetErrorString(backtrace->GetError()) << ")\n";
    return;
  } else if (backtrace->NumFrames() == 0) {
    os << prefix << "(no native stack frames for thread " << tid << ")\n";
    return;
  }

  // Check whether we have and should use addr2line.
  bool use_addr2line;
  if (kUseAddr2line) {
    // Try to run it to see whether we have it. Push an argument so that it doesn't assume a.out
    // and print to stderr.
    use_addr2line = (gAborting > 0) && RunCommand("addr2line -h", nullptr, nullptr);
  } else {
    use_addr2line = false;
  }

  for (Backtrace::const_iterator it = backtrace->begin();
       it != backtrace->end(); ++it) {
    // We produce output like this:
    // ]    #00 pc 000075bb8  /system/lib/libc.so (unwind_backtrace_thread+536)
    // In order for parsing tools to continue to function, the stack dump
    // format must at least adhere to this format:
    //  #XX pc <RELATIVE_ADDR>  <FULL_PATH_TO_SHARED_LIBRARY> ...
    // The parsers require a single space before and after pc, and two spaces
    // after the <RELATIVE_ADDR>. There can be any prefix data before the
    // #XX. <RELATIVE_ADDR> has to be a hex number but with no 0x prefix.
    os << prefix << StringPrintf("#%02zu pc ", it->num);
    bool try_addr2line = false;
    if (!BacktraceMap::IsValid(it->map)) {
      os << StringPrintf(Is64BitInstructionSet(kRuntimeISA) ? "%016" PRIxPTR "  ???"
                                                            : "%08" PRIxPTR "  ???",
                         it->pc);
    } else {
      os << StringPrintf(Is64BitInstructionSet(kRuntimeISA) ? "%016" PRIxPTR "  "
                                                            : "%08" PRIxPTR "  ",
                         BacktraceMap::GetRelativePc(it->map, it->pc));
      os << it->map.name;
      os << " (";
      if (!it->func_name.empty()) {
        os << it->func_name;
        if (it->func_offset != 0) {
          os << "+" << it->func_offset;
        }
        try_addr2line = true;
      } else if (current_method != nullptr &&
          Locks::mutator_lock_->IsSharedHeld(Thread::Current()) &&
          PcIsWithinQuickCode(current_method, it->pc)) {
        const void* start_of_code = current_method->GetEntryPointFromQuickCompiledCode();
        os << JniLongName(current_method) << "+"
           << (it->pc - reinterpret_cast<uintptr_t>(start_of_code));
      } else {
        os << "???";
      }
      os << ")";
    }
    os << "\n";
    if (try_addr2line && use_addr2line) {
      Addr2line(it->map.name, it->pc - it->map.start, os, prefix);
    }
  }
#else
  UNUSED(os, tid, existing_map, prefix, current_method, ucontext_ptr);
#endif
}

#if defined(__APPLE__)

// TODO: is there any way to get the kernel stack on Mac OS?
void DumpKernelStack(std::ostream&, pid_t, const char*, bool) {}

#else

void DumpKernelStack(std::ostream& os, pid_t tid, const char* prefix, bool include_count) {
  if (tid == GetTid()) {
    // There's no point showing that we're reading our stack out of /proc!
    return;
  }

  std::string kernel_stack_filename(StringPrintf("/proc/self/task/%d/stack", tid));
  std::string kernel_stack;
  if (!ReadFileToString(kernel_stack_filename, &kernel_stack)) {
    os << prefix << "(couldn't read " << kernel_stack_filename << ")\n";
    return;
  }

  std::vector<std::string> kernel_stack_frames;
  Split(kernel_stack, '\n', &kernel_stack_frames);
  // We skip the last stack frame because it's always equivalent to "[<ffffffff>] 0xffffffff",
  // which looking at the source appears to be the kernel's way of saying "that's all, folks!".
  kernel_stack_frames.pop_back();
  for (size_t i = 0; i < kernel_stack_frames.size(); ++i) {
    // Turn "[<ffffffff8109156d>] futex_wait_queue_me+0xcd/0x110"
    // into "futex_wait_queue_me+0xcd/0x110".
    const char* text = kernel_stack_frames[i].c_str();
    const char* close_bracket = strchr(text, ']');
    if (close_bracket != nullptr) {
      text = close_bracket + 2;
    }
    os << prefix;
    if (include_count) {
      os << StringPrintf("#%02zd ", i);
    }
    os << text << "\n";
  }
}

#endif

const char* GetAndroidRoot() {
  const char* android_root = getenv("ANDROID_ROOT");
  if (android_root == nullptr) {
    if (OS::DirectoryExists("/system")) {
      android_root = "/system";
    } else {
      LOG(FATAL) << "ANDROID_ROOT not set and /system does not exist";
      return "";
    }
  }
  if (!OS::DirectoryExists(android_root)) {
    LOG(FATAL) << "Failed to find ANDROID_ROOT directory " << android_root;
    return "";
  }
  return android_root;
}

const char* GetAndroidData() {
  std::string error_msg;
  const char* dir = GetAndroidDataSafe(&error_msg);
  if (dir != nullptr) {
    return dir;
  } else {
    LOG(FATAL) << error_msg;
    return "";
  }
}

const char* GetAndroidDataSafe(std::string* error_msg) {
  const char* android_data = getenv("ANDROID_DATA");
  if (android_data == nullptr) {
    if (OS::DirectoryExists("/data")) {
      android_data = "/data";
    } else {
      *error_msg = "ANDROID_DATA not set and /data does not exist";
      return nullptr;
    }
  }
  if (!OS::DirectoryExists(android_data)) {
    *error_msg = StringPrintf("Failed to find ANDROID_DATA directory %s", android_data);
    return nullptr;
  }
  return android_data;
}

void GetDalvikCache(const char* subdir, const bool create_if_absent, std::string* dalvik_cache,
                    bool* have_android_data, bool* dalvik_cache_exists, bool* is_global_cache) {
  CHECK(subdir != nullptr);
  std::string error_msg;
  const char* android_data = GetAndroidDataSafe(&error_msg);
  if (android_data == nullptr) {
    *have_android_data = false;
    *dalvik_cache_exists = false;
    *is_global_cache = false;
    return;
  } else {
    *have_android_data = true;
  }
  const std::string dalvik_cache_root(StringPrintf("%s/dalvik-cache/", android_data));
  *dalvik_cache = dalvik_cache_root + subdir;
  *dalvik_cache_exists = OS::DirectoryExists(dalvik_cache->c_str());
  *is_global_cache = strcmp(android_data, "/data") == 0;
  if (create_if_absent && !*dalvik_cache_exists && !*is_global_cache) {
    // Don't create the system's /data/dalvik-cache/... because it needs special permissions.
    *dalvik_cache_exists = ((mkdir(dalvik_cache_root.c_str(), 0700) == 0 || errno == EEXIST) &&
                            (mkdir(dalvik_cache->c_str(), 0700) == 0 || errno == EEXIST));
  }
}

static std::string GetDalvikCacheImpl(const char* subdir,
                                      const bool create_if_absent,
                                      const bool abort_on_error) {
  CHECK(subdir != nullptr);
  const char* android_data = GetAndroidData();
  const std::string dalvik_cache_root(StringPrintf("%s/dalvik-cache/", android_data));
  const std::string dalvik_cache = dalvik_cache_root + subdir;
  if (!OS::DirectoryExists(dalvik_cache.c_str())) {
    if (!create_if_absent) {
      // TODO: Check callers. Traditional behavior is to not to abort, even when abort_on_error.
      return "";
    }

    // Don't create the system's /data/dalvik-cache/... because it needs special permissions.
    if (strcmp(android_data, "/data") == 0) {
      if (abort_on_error) {
        LOG(FATAL) << "Failed to find dalvik-cache directory " << dalvik_cache
                   << ", cannot create /data dalvik-cache.";
        UNREACHABLE();
      }
      return "";
    }

    int result = mkdir(dalvik_cache_root.c_str(), 0700);
    if (result != 0 && errno != EEXIST) {
      if (abort_on_error) {
        PLOG(FATAL) << "Failed to create dalvik-cache root directory " << dalvik_cache_root;
        UNREACHABLE();
      }
      return "";
    }

    result = mkdir(dalvik_cache.c_str(), 0700);
    if (result != 0) {
      if (abort_on_error) {
        PLOG(FATAL) << "Failed to create dalvik-cache directory " << dalvik_cache;
        UNREACHABLE();
      }
      return "";
    }
  }
  return dalvik_cache;
}

std::string GetDalvikCache(const char* subdir, const bool create_if_absent) {
  return GetDalvikCacheImpl(subdir, create_if_absent, false);
}

std::string GetDalvikCacheOrDie(const char* subdir, const bool create_if_absent) {
  return GetDalvikCacheImpl(subdir, create_if_absent, true);
}

bool GetDalvikCacheFilename(const char* location, const char* cache_location,
                            std::string* filename, std::string* error_msg) {
  if (location[0] != '/') {
    *error_msg = StringPrintf("Expected path in location to be absolute: %s", location);
    return false;
  }
  std::string cache_file(&location[1]);  // skip leading slash
  if (!EndsWith(location, ".dex") && !EndsWith(location, ".art") && !EndsWith(location, ".oat")) {
    cache_file += "/";
    cache_file += DexFile::kClassesDex;
  }
  std::replace(cache_file.begin(), cache_file.end(), '/', '@');
  *filename = StringPrintf("%s/%s", cache_location, cache_file.c_str());
  return true;
}

std::string GetDalvikCacheFilenameOrDie(const char* location, const char* cache_location) {
  std::string ret;
  std::string error_msg;
  if (!GetDalvikCacheFilename(location, cache_location, &ret, &error_msg)) {
    LOG(FATAL) << error_msg;
  }
  return ret;
}

static void InsertIsaDirectory(const InstructionSet isa, std::string* filename) {
  // in = /foo/bar/baz
  // out = /foo/bar/<isa>/baz
  size_t pos = filename->rfind('/');
  CHECK_NE(pos, std::string::npos) << *filename << " " << isa;
  filename->insert(pos, "/", 1);
  filename->insert(pos + 1, GetInstructionSetString(isa));
}

std::string GetSystemImageFilename(const char* location, const InstructionSet isa) {
  // location = /system/framework/boot.art
  // filename = /system/framework/<isa>/boot.art
  std::string filename(location);
  InsertIsaDirectory(isa, &filename);
  return filename;
}

int ExecAndReturnCode(std::vector<std::string>& arg_vector, std::string* error_msg) {
  const std::string command_line(Join(arg_vector, ' '));
  CHECK_GE(arg_vector.size(), 1U) << command_line;

  // Convert the args to char pointers.
  const char* program = arg_vector[0].c_str();
  std::vector<char*> args;
  for (size_t i = 0; i < arg_vector.size(); ++i) {
    const std::string& arg = arg_vector[i];
    char* arg_str = const_cast<char*>(arg.c_str());
    CHECK(arg_str != nullptr) << i;
    args.push_back(arg_str);
  }
  args.push_back(nullptr);

  // fork and exec
  pid_t pid = fork();
  if (pid == 0) {
    // no allocation allowed between fork and exec

    // change process groups, so we don't get reaped by ProcessManager
    setpgid(0, 0);

    // (b/30160149): protect subprocesses from modifications to LD_LIBRARY_PATH, etc.
    // Use the snapshot of the environment from the time the runtime was created.
    char** envp = (Runtime::Current() == nullptr) ? nullptr : Runtime::Current()->GetEnvSnapshot();
    if (envp == nullptr) {
      execv(program, &args[0]);
    } else {
      execve(program, &args[0], envp);
    }
    PLOG(ERROR) << "Failed to execve(" << command_line << ")";
    // _exit to avoid atexit handlers in child.
    _exit(1);
  } else {
    if (pid == -1) {
      *error_msg = StringPrintf("Failed to execv(%s) because fork failed: %s",
                                command_line.c_str(), strerror(errno));
      return -1;
    }

    // wait for subprocess to finish
    int status = -1;
    pid_t got_pid = TEMP_FAILURE_RETRY(waitpid(pid, &status, 0));
    if (got_pid != pid) {
      *error_msg = StringPrintf("Failed after fork for execv(%s) because waitpid failed: "
                                "wanted %d, got %d: %s",
                                command_line.c_str(), pid, got_pid, strerror(errno));
      return -1;
    }
    if (WIFEXITED(status)) {
      return WEXITSTATUS(status);
    }
    return -1;
  }
}

bool Exec(std::vector<std::string>& arg_vector, std::string* error_msg) {
  int status = ExecAndReturnCode(arg_vector, error_msg);
  if (status != 0) {
    const std::string command_line(Join(arg_vector, ' '));
    *error_msg = StringPrintf("Failed execv(%s) because non-0 exit status",
                              command_line.c_str());
    return false;
  }
  return true;
}

bool FileExists(const std::string& filename) {
  struct stat buffer;
  return stat(filename.c_str(), &buffer) == 0;
}

bool FileExistsAndNotEmpty(const std::string& filename) {
  struct stat buffer;
  if (stat(filename.c_str(), &buffer) != 0) {
    return false;
  }
  return buffer.st_size > 0;
}

std::string PrettyDescriptor(Primitive::Type type) {
  return PrettyDescriptor(Primitive::Descriptor(type));
}

static void DumpMethodCFGImpl(const DexFile* dex_file,
                              uint32_t dex_method_idx,
                              const DexFile::CodeItem* code_item,
                              std::ostream& os) {
  os << "digraph {\n";
  os << "  # /* " << PrettyMethod(dex_method_idx, *dex_file, true) << " */\n";

  std::set<uint32_t> dex_pc_is_branch_target;
  {
    // Go and populate.
    const Instruction* inst = Instruction::At(code_item->insns_);
    for (uint32_t dex_pc = 0;
         dex_pc < code_item->insns_size_in_code_units_;
         dex_pc += inst->SizeInCodeUnits(), inst = inst->Next()) {
      if (inst->IsBranch()) {
        dex_pc_is_branch_target.insert(dex_pc + inst->GetTargetOffset());
      } else if (inst->IsSwitch()) {
        const uint16_t* insns = code_item->insns_ + dex_pc;
        int32_t switch_offset = insns[1] | (static_cast<int32_t>(insns[2]) << 16);
        const uint16_t* switch_insns = insns + switch_offset;
        uint32_t switch_count = switch_insns[1];
        int32_t targets_offset;
        if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
          /* 0=sig, 1=count, 2/3=firstKey */
          targets_offset = 4;
        } else {
          /* 0=sig, 1=count, 2..count*2 = keys */
          targets_offset = 2 + 2 * switch_count;
        }
        for (uint32_t targ = 0; targ < switch_count; targ++) {
          int32_t offset =
              static_cast<int32_t>(switch_insns[targets_offset + targ * 2]) |
              static_cast<int32_t>(switch_insns[targets_offset + targ * 2 + 1] << 16);
          dex_pc_is_branch_target.insert(dex_pc + offset);
        }
      }
    }
  }

  // Create nodes for "basic blocks."
  std::map<uint32_t, uint32_t> dex_pc_to_node_id;  // This only has entries for block starts.
  std::map<uint32_t, uint32_t> dex_pc_to_incl_id;  // This has entries for all dex pcs.

  {
    const Instruction* inst = Instruction::At(code_item->insns_);
    bool first_in_block = true;
    bool force_new_block = false;
    for (uint32_t dex_pc = 0;
         dex_pc < code_item->insns_size_in_code_units_;
         dex_pc += inst->SizeInCodeUnits(), inst = inst->Next()) {
      if (dex_pc == 0 ||
          (dex_pc_is_branch_target.find(dex_pc) != dex_pc_is_branch_target.end()) ||
          force_new_block) {
        uint32_t id = dex_pc_to_node_id.size();
        if (id > 0) {
          // End last node.
          os << "}\"];\n";
        }
        // Start next node.
        os << "  node" << id << " [shape=record,label=\"{";
        dex_pc_to_node_id.insert(std::make_pair(dex_pc, id));
        first_in_block = true;
        force_new_block = false;
      }

      // Register instruction.
      dex_pc_to_incl_id.insert(std::make_pair(dex_pc, dex_pc_to_node_id.size() - 1));

      // Print instruction.
      if (!first_in_block) {
        os << " | ";
      } else {
        first_in_block = false;
      }

      // Dump the instruction. Need to escape '"', '<', '>', '{' and '}'.
      os << "<" << "p" << dex_pc << ">";
      os << " 0x" << std::hex << dex_pc << std::dec << ": ";
      std::string inst_str = inst->DumpString(dex_file);
      size_t cur_start = 0;  // It's OK to start at zero, instruction dumps don't start with chars
                             // we need to escape.
      while (cur_start != std::string::npos) {
        size_t next_escape = inst_str.find_first_of("\"{}<>", cur_start + 1);
        if (next_escape == std::string::npos) {
          os << inst_str.substr(cur_start, inst_str.size() - cur_start);
          break;
        } else {
          os << inst_str.substr(cur_start, next_escape - cur_start);
          // Escape all necessary characters.
          while (next_escape < inst_str.size()) {
            char c = inst_str.at(next_escape);
            if (c == '"' || c == '{' || c == '}' || c == '<' || c == '>') {
              os << '\\' << c;
            } else {
              break;
            }
            next_escape++;
          }
          if (next_escape >= inst_str.size()) {
            next_escape = std::string::npos;
          }
          cur_start = next_escape;
        }
      }

      // Force a new block for some fall-throughs and some instructions that terminate the "local"
      // control flow.
      force_new_block = inst->IsSwitch() || inst->IsBasicBlockEnd();
    }
    // Close last node.
    if (dex_pc_to_node_id.size() > 0) {
      os << "}\"];\n";
    }
  }

  // Create edges between them.
  {
    std::ostringstream regular_edges;
    std::ostringstream taken_edges;
    std::ostringstream exception_edges;

    // Common set of exception edges.
    std::set<uint32_t> exception_targets;

    // These blocks (given by the first dex pc) need exception per dex-pc handling in a second
    // pass. In the first pass we try and see whether we can use a common set of edges.
    std::set<uint32_t> blocks_with_detailed_exceptions;

    {
      uint32_t last_node_id = std::numeric_limits<uint32_t>::max();
      uint32_t old_dex_pc = 0;
      uint32_t block_start_dex_pc = std::numeric_limits<uint32_t>::max();
      const Instruction* inst = Instruction::At(code_item->insns_);
      for (uint32_t dex_pc = 0;
          dex_pc < code_item->insns_size_in_code_units_;
          old_dex_pc = dex_pc, dex_pc += inst->SizeInCodeUnits(), inst = inst->Next()) {
        {
          auto it = dex_pc_to_node_id.find(dex_pc);
          if (it != dex_pc_to_node_id.end()) {
            if (!exception_targets.empty()) {
              // It seems the last block had common exception handlers. Add the exception edges now.
              uint32_t node_id = dex_pc_to_node_id.find(block_start_dex_pc)->second;
              for (uint32_t handler_pc : exception_targets) {
                auto node_id_it = dex_pc_to_incl_id.find(handler_pc);
                if (node_id_it != dex_pc_to_incl_id.end()) {
                  exception_edges << "  node" << node_id
                      << " -> node" << node_id_it->second << ":p" << handler_pc
                      << ";\n";
                }
              }
              exception_targets.clear();
            }

            block_start_dex_pc = dex_pc;

            // Seems to be a fall-through, connect to last_node_id. May be spurious edges for things
            // like switch data.
            uint32_t old_last = last_node_id;
            last_node_id = it->second;
            if (old_last != std::numeric_limits<uint32_t>::max()) {
              regular_edges << "  node" << old_last << ":p" << old_dex_pc
                  << " -> node" << last_node_id << ":p" << dex_pc
                  << ";\n";
            }
          }

          // Look at the exceptions of the first entry.
          CatchHandlerIterator catch_it(*code_item, dex_pc);
          for (; catch_it.HasNext(); catch_it.Next()) {
            exception_targets.insert(catch_it.GetHandlerAddress());
          }
        }

        // Handle instruction.

        // Branch: something with at most two targets.
        if (inst->IsBranch()) {
          const int32_t offset = inst->GetTargetOffset();
          const bool conditional = !inst->IsUnconditional();

          auto target_it = dex_pc_to_node_id.find(dex_pc + offset);
          if (target_it != dex_pc_to_node_id.end()) {
            taken_edges << "  node" << last_node_id << ":p" << dex_pc
                << " -> node" << target_it->second << ":p" << (dex_pc + offset)
                << ";\n";
          }
          if (!conditional) {
            // No fall-through.
            last_node_id = std::numeric_limits<uint32_t>::max();
          }
        } else if (inst->IsSwitch()) {
          // TODO: Iterate through all switch targets.
          const uint16_t* insns = code_item->insns_ + dex_pc;
          /* make sure the start of the switch is in range */
          int32_t switch_offset = insns[1] | (static_cast<int32_t>(insns[2]) << 16);
          /* offset to switch table is a relative branch-style offset */
          const uint16_t* switch_insns = insns + switch_offset;
          uint32_t switch_count = switch_insns[1];
          int32_t targets_offset;
          if ((*insns & 0xff) == Instruction::PACKED_SWITCH) {
            /* 0=sig, 1=count, 2/3=firstKey */
            targets_offset = 4;
          } else {
            /* 0=sig, 1=count, 2..count*2 = keys */
            targets_offset = 2 + 2 * switch_count;
          }
          /* make sure the end of the switch is in range */
          /* verify each switch target */
          for (uint32_t targ = 0; targ < switch_count; targ++) {
            int32_t offset =
                static_cast<int32_t>(switch_insns[targets_offset + targ * 2]) |
                static_cast<int32_t>(switch_insns[targets_offset + targ * 2 + 1] << 16);
            int32_t abs_offset = dex_pc + offset;
            auto target_it = dex_pc_to_node_id.find(abs_offset);
            if (target_it != dex_pc_to_node_id.end()) {
              // TODO: value label.
              taken_edges << "  node" << last_node_id << ":p" << dex_pc
                  << " -> node" << target_it->second << ":p" << (abs_offset)
                  << ";\n";
            }
          }
        }

        // Exception edges. If this is not the first instruction in the block
        if (block_start_dex_pc != dex_pc) {
          std::set<uint32_t> current_handler_pcs;
          CatchHandlerIterator catch_it(*code_item, dex_pc);
          for (; catch_it.HasNext(); catch_it.Next()) {
            current_handler_pcs.insert(catch_it.GetHandlerAddress());
          }
          if (current_handler_pcs != exception_targets) {
            exception_targets.clear();  // Clear so we don't do something at the end.
            blocks_with_detailed_exceptions.insert(block_start_dex_pc);
          }
        }

        if (inst->IsReturn() ||
            (inst->Opcode() == Instruction::THROW) ||
            (inst->IsBranch() && inst->IsUnconditional())) {
          // No fall-through.
          last_node_id = std::numeric_limits<uint32_t>::max();
        }
      }
      // Finish up the last block, if it had common exceptions.
      if (!exception_targets.empty()) {
        // It seems the last block had common exception handlers. Add the exception edges now.
        uint32_t node_id = dex_pc_to_node_id.find(block_start_dex_pc)->second;
        for (uint32_t handler_pc : exception_targets) {
          auto node_id_it = dex_pc_to_incl_id.find(handler_pc);
          if (node_id_it != dex_pc_to_incl_id.end()) {
            exception_edges << "  node" << node_id
                << " -> node" << node_id_it->second << ":p" << handler_pc
                << ";\n";
          }
        }
        exception_targets.clear();
      }
    }

    // Second pass for detailed exception blocks.
    // TODO
    // Exception edges. If this is not the first instruction in the block
    for (uint32_t dex_pc : blocks_with_detailed_exceptions) {
      const Instruction* inst = Instruction::At(&code_item->insns_[dex_pc]);
      uint32_t this_node_id = dex_pc_to_incl_id.find(dex_pc)->second;
      while (true) {
        CatchHandlerIterator catch_it(*code_item, dex_pc);
        if (catch_it.HasNext()) {
          std::set<uint32_t> handled_targets;
          for (; catch_it.HasNext(); catch_it.Next()) {
            uint32_t handler_pc = catch_it.GetHandlerAddress();
            auto it = handled_targets.find(handler_pc);
            if (it == handled_targets.end()) {
              auto node_id_it = dex_pc_to_incl_id.find(handler_pc);
              if (node_id_it != dex_pc_to_incl_id.end()) {
                exception_edges << "  node" << this_node_id << ":p" << dex_pc
                    << " -> node" << node_id_it->second << ":p" << handler_pc
                    << ";\n";
              }

              // Mark as done.
              handled_targets.insert(handler_pc);
            }
          }
        }
        if (inst->IsBasicBlockEnd()) {
          break;
        }

        // Loop update. Have a break-out if the next instruction is a branch target and thus in
        // another block.
        dex_pc += inst->SizeInCodeUnits();
        if (dex_pc >= code_item->insns_size_in_code_units_) {
          break;
        }
        if (dex_pc_to_node_id.find(dex_pc) != dex_pc_to_node_id.end()) {
          break;
        }
        inst = inst->Next();
      }
    }

    // Write out the sub-graphs to make edges styled.
    os << "\n";
    os << "  subgraph regular_edges {\n";
    os << "    edge [color=\"#000000\",weight=.3,len=3];\n\n";
    os << "    " << regular_edges.str() << "\n";
    os << "  }\n\n";

    os << "  subgraph taken_edges {\n";
    os << "    edge [color=\"#00FF00\",weight=.3,len=3];\n\n";
    os << "    " << taken_edges.str() << "\n";
    os << "  }\n\n";

    os << "  subgraph exception_edges {\n";
    os << "    edge [color=\"#FF0000\",weight=.3,len=3];\n\n";
    os << "    " << exception_edges.str() << "\n";
    os << "  }\n\n";
  }

  os << "}\n";
}

void DumpMethodCFG(ArtMethod* method, std::ostream& os) {
  const DexFile* dex_file = method->GetDexFile();
  const DexFile::CodeItem* code_item = dex_file->GetCodeItem(method->GetCodeItemOffset());

  DumpMethodCFGImpl(dex_file, method->GetDexMethodIndex(), code_item, os);
}

void DumpMethodCFG(const DexFile* dex_file, uint32_t dex_method_idx, std::ostream& os) {
  // This is painful, we need to find the code item. That means finding the class, and then
  // iterating the table.
  if (dex_method_idx >= dex_file->NumMethodIds()) {
    os << "Could not find method-idx.";
    return;
  }
  const DexFile::MethodId& method_id = dex_file->GetMethodId(dex_method_idx);

  const DexFile::ClassDef* class_def = dex_file->FindClassDef(method_id.class_idx_);
  if (class_def == nullptr) {
    os << "Could not find class-def.";
    return;
  }

  const uint8_t* class_data = dex_file->GetClassData(*class_def);
  if (class_data == nullptr) {
    os << "No class data.";
    return;
  }

  ClassDataItemIterator it(*dex_file, class_data);
  // Skip fields
  while (it.HasNextStaticField() || it.HasNextInstanceField()) {
    it.Next();
  }

  // Find method, and dump it.
  while (it.HasNextDirectMethod() || it.HasNextVirtualMethod()) {
    uint32_t method_idx = it.GetMemberIndex();
    if (method_idx == dex_method_idx) {
      DumpMethodCFGImpl(dex_file, dex_method_idx, it.GetMethodCodeItem(), os);
      return;
    }
    it.Next();
  }

  // Otherwise complain.
  os << "Something went wrong, didn't find the method in the class data.";
}

static void ParseStringAfterChar(const std::string& s,
                                 char c,
                                 std::string* parsed_value,
                                 UsageFn Usage) {
  std::string::size_type colon = s.find(c);
  if (colon == std::string::npos) {
    Usage("Missing char %c in option %s\n", c, s.c_str());
  }
  // Add one to remove the char we were trimming until.
  *parsed_value = s.substr(colon + 1);
}

void ParseDouble(const std::string& option,
                 char after_char,
                 double min,
                 double max,
                 double* parsed_value,
                 UsageFn Usage) {
  std::string substring;
  ParseStringAfterChar(option, after_char, &substring, Usage);
  bool sane_val = true;
  double value;
  if ((false)) {
    // TODO: this doesn't seem to work on the emulator.  b/15114595
    std::stringstream iss(substring);
    iss >> value;
    // Ensure that we have a value, there was no cruft after it and it satisfies a sensible range.
    sane_val = iss.eof() && (value >= min) && (value <= max);
  } else {
    char* end = nullptr;
    value = strtod(substring.c_str(), &end);
    sane_val = *end == '\0' && value >= min && value <= max;
  }
  if (!sane_val) {
    Usage("Invalid double value %s for option %s\n", substring.c_str(), option.c_str());
  }
  *parsed_value = value;
}

int64_t GetFileSizeBytes(const std::string& filename) {
  struct stat stat_buf;
  int rc = stat(filename.c_str(), &stat_buf);
  return rc == 0 ? stat_buf.st_size : -1;
}

void SleepForever() {
  while (true) {
    usleep(1000000);
  }
}

}  // namespace art
