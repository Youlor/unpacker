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

#include "logging.h"

#include <iostream>
#include <limits>
#include <sstream>

#include "base/mutex.h"
#include "runtime.h"
#include "thread-inl.h"
#include "utils.h"

// Headers for LogMessage::LogLine.
#ifdef __ANDROID__
#include "cutils/log.h"
#else
#include <sys/types.h>
#include <unistd.h>
#endif

namespace art {

LogVerbosity gLogVerbosity;

unsigned int gAborting = 0;

static LogSeverity gMinimumLogSeverity = INFO;
static std::unique_ptr<std::string> gCmdLine;
static std::unique_ptr<std::string> gProgramInvocationName;
static std::unique_ptr<std::string> gProgramInvocationShortName;

// Print INTERNAL_FATAL messages directly instead of at destruction time. This only works on the
// host right now: for the device, a stream buf collating output into lines and calling LogLine or
// lower-level logging is necessary.
#ifdef __ANDROID__
static constexpr bool kPrintInternalFatalDirectly = false;
#else
static constexpr bool kPrintInternalFatalDirectly = !kIsTargetBuild;
#endif

static bool PrintDirectly(LogSeverity severity) {
  return kPrintInternalFatalDirectly && severity == INTERNAL_FATAL;
}

const char* GetCmdLine() {
  return (gCmdLine.get() != nullptr) ? gCmdLine->c_str() : nullptr;
}

const char* ProgramInvocationName() {
  return (gProgramInvocationName.get() != nullptr) ? gProgramInvocationName->c_str() : "art";
}

const char* ProgramInvocationShortName() {
  return (gProgramInvocationShortName.get() != nullptr) ? gProgramInvocationShortName->c_str()
                                                        : "art";
}

void InitLogging(char* argv[]) {
  if (gCmdLine.get() != nullptr) {
    return;
  }
  // TODO: Move this to a more obvious InitART...
  Locks::Init();

  // Stash the command line for later use. We can use /proc/self/cmdline on Linux to recover this,
  // but we don't have that luxury on the Mac, and there are a couple of argv[0] variants that are
  // commonly used.
  if (argv != nullptr) {
    gCmdLine.reset(new std::string(argv[0]));
    for (size_t i = 1; argv[i] != nullptr; ++i) {
      gCmdLine->append(" ");
      gCmdLine->append(argv[i]);
    }
    gProgramInvocationName.reset(new std::string(argv[0]));
    const char* last_slash = strrchr(argv[0], '/');
    gProgramInvocationShortName.reset(new std::string((last_slash != nullptr) ? last_slash + 1
                                                                           : argv[0]));
  } else {
    // TODO: fall back to /proc/self/cmdline when argv is null on Linux.
    gCmdLine.reset(new std::string("<unset>"));
  }
  const char* tags = getenv("ANDROID_LOG_TAGS");
  if (tags == nullptr) {
    return;
  }

  std::vector<std::string> specs;
  Split(tags, ' ', &specs);
  for (size_t i = 0; i < specs.size(); ++i) {
    // "tag-pattern:[vdiwefs]"
    std::string spec(specs[i]);
    if (spec.size() == 3 && StartsWith(spec, "*:")) {
      switch (spec[2]) {
        case 'v':
          gMinimumLogSeverity = VERBOSE;
          continue;
        case 'd':
          gMinimumLogSeverity = DEBUG;
          continue;
        case 'i':
          gMinimumLogSeverity = INFO;
          continue;
        case 'w':
          gMinimumLogSeverity = WARNING;
          continue;
        case 'e':
          gMinimumLogSeverity = ERROR;
          continue;
        case 'f':
          gMinimumLogSeverity = FATAL;
          continue;
        // liblog will even suppress FATAL if you say 's' for silent, but that's crazy!
        case 's':
          gMinimumLogSeverity = FATAL;
          continue;
      }
    }
    LOG(FATAL) << "unsupported '" << spec << "' in ANDROID_LOG_TAGS (" << tags << ")";
  }
}

// This indirection greatly reduces the stack impact of having
// lots of checks/logging in a function.
class LogMessageData {
 public:
  LogMessageData(const char* file, unsigned int line, LogSeverity severity, int error)
      : file_(file),
        line_number_(line),
        severity_(severity),
        error_(error) {
    const char* last_slash = strrchr(file, '/');
    file = (last_slash == nullptr) ? file : last_slash + 1;
  }

  const char * GetFile() const {
    return file_;
  }

  unsigned int GetLineNumber() const {
    return line_number_;
  }

  LogSeverity GetSeverity() const {
    return severity_;
  }

  int GetError() const {
    return error_;
  }

  std::ostream& GetBuffer() {
    return buffer_;
  }

  std::string ToString() const {
    return buffer_.str();
  }

 private:
  std::ostringstream buffer_;
  const char* const file_;
  const unsigned int line_number_;
  const LogSeverity severity_;
  const int error_;

  DISALLOW_COPY_AND_ASSIGN(LogMessageData);
};


LogMessage::LogMessage(const char* file, unsigned int line, LogSeverity severity, int error)
  : data_(new LogMessageData(file, line, severity, error)) {
  if (PrintDirectly(severity)) {
    static constexpr char kLogCharacters[] = { 'N', 'V', 'D', 'I', 'W', 'E', 'F', 'F' };
    static_assert(arraysize(kLogCharacters) == static_cast<size_t>(INTERNAL_FATAL) + 1,
                  "Wrong character array size");
    stream() << ProgramInvocationShortName() << " " << kLogCharacters[static_cast<size_t>(severity)]
             << " " << getpid() << " " << ::art::GetTid() << " " << file << ":" <<  line << "]";
  }
}
LogMessage::~LogMessage() {
  std::string msg;

  if (!PrintDirectly(data_->GetSeverity()) && data_->GetSeverity() != LogSeverity::NONE) {
    if (data_->GetSeverity() < gMinimumLogSeverity) {
      return;  // No need to format something we're not going to output.
    }

    // Finish constructing the message.
    if (data_->GetError() != -1) {
      data_->GetBuffer() << ": " << strerror(data_->GetError());
    }
    msg = data_->ToString();

    // Do the actual logging with the lock held.
    {
      MutexLock mu(Thread::Current(), *Locks::logging_lock_);
      if (msg.find('\n') == std::string::npos) {
        LogLine(data_->GetFile(), data_->GetLineNumber(), data_->GetSeverity(), msg.c_str());
      } else {
        msg += '\n';
        size_t i = 0;
        while (i < msg.size()) {
          size_t nl = msg.find('\n', i);
          msg[nl] = '\0';
          LogLine(data_->GetFile(), data_->GetLineNumber(), data_->GetSeverity(), &msg[i]);
          // Undo zero-termination, so we retain the complete message.
          msg[nl] = '\n';
          i = nl + 1;
        }
      }
    }
  }

  // Abort if necessary.
  if (data_->GetSeverity() == FATAL) {
    Runtime::Abort(msg.c_str());
  }
}

std::ostream& LogMessage::stream() {
  if (PrintDirectly(data_->GetSeverity())) {
    return std::cerr;
  }
  return data_->GetBuffer();
}

#ifdef __ANDROID__
static const android_LogPriority kLogSeverityToAndroidLogPriority[] = {
  ANDROID_LOG_VERBOSE,  // NONE, use verbose as stand-in, will never be printed.
  ANDROID_LOG_VERBOSE, ANDROID_LOG_DEBUG, ANDROID_LOG_INFO, ANDROID_LOG_WARN,
  ANDROID_LOG_ERROR, ANDROID_LOG_FATAL, ANDROID_LOG_FATAL
};
static_assert(arraysize(kLogSeverityToAndroidLogPriority) == INTERNAL_FATAL + 1,
              "Mismatch in size of kLogSeverityToAndroidLogPriority and values in LogSeverity");
#endif

void LogMessage::LogLine(const char* file, unsigned int line, LogSeverity log_severity,
                         const char* message) {
  if (log_severity == LogSeverity::NONE) {
    return;
  }

#ifdef __ANDROID__
  const char* tag = ProgramInvocationShortName();
  int priority = kLogSeverityToAndroidLogPriority[static_cast<size_t>(log_severity)];
  if (priority == ANDROID_LOG_FATAL) {
    LOG_PRI(priority, tag, "%s:%u] %s", file, line, message);
  } else {
    LOG_PRI(priority, tag, "%s", message);
  }
#else
  static const char* log_characters = "NVDIWEFF";
  CHECK_EQ(strlen(log_characters), INTERNAL_FATAL + 1U);
  char severity = log_characters[log_severity];
  fprintf(stderr, "%s %c %5d %5d %s:%u] %s\n",
          ProgramInvocationShortName(), severity, getpid(), ::art::GetTid(), file, line, message);
#endif
}

void LogMessage::LogLineLowStack(const char* file, unsigned int line, LogSeverity log_severity,
                                 const char* message) {
  if (log_severity == LogSeverity::NONE) {
    return;
  }

#ifdef __ANDROID__
  // Use android_writeLog() to avoid stack-based buffers used by android_printLog().
  const char* tag = ProgramInvocationShortName();
  int priority = kLogSeverityToAndroidLogPriority[static_cast<size_t>(log_severity)];
  char* buf = nullptr;
  size_t buf_size = 0u;
  if (priority == ANDROID_LOG_FATAL) {
    // Allocate buffer for snprintf(buf, buf_size, "%s:%u] %s", file, line, message) below.
    // If allocation fails, fall back to printing only the message.
    buf_size = strlen(file) + 1 /* ':' */ + std::numeric_limits<typeof(line)>::max_digits10 +
        2 /* "] " */ + strlen(message) + 1 /* terminating 0 */;
    buf = reinterpret_cast<char*>(malloc(buf_size));
  }
  if (buf != nullptr) {
    snprintf(buf, buf_size, "%s:%u] %s", file, line, message);
    android_writeLog(priority, tag, buf);
    free(buf);
  } else {
    android_writeLog(priority, tag, message);
  }
#else
  static constexpr char kLogCharacters[] = { 'N', 'V', 'D', 'I', 'W', 'E', 'F', 'F' };
  static_assert(arraysize(kLogCharacters) == static_cast<size_t>(INTERNAL_FATAL) + 1,
                "Wrong character array size");

  const char* program_name = ProgramInvocationShortName();
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, program_name, strlen(program_name)));
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, " ", 1));
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, &kLogCharacters[static_cast<size_t>(log_severity)], 1));
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, " ", 1));
  // TODO: pid and tid.
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, file, strlen(file)));
  // TODO: line.
  UNUSED(line);
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, "] ", 2));
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, message, strlen(message)));
  TEMP_FAILURE_RETRY(write(STDERR_FILENO, "\n", 1));
#endif
}

ScopedLogSeverity::ScopedLogSeverity(LogSeverity level) {
  old_ = gMinimumLogSeverity;
  gMinimumLogSeverity = level;
}

ScopedLogSeverity::~ScopedLogSeverity() {
  gMinimumLogSeverity = old_;
}

}  // namespace art
