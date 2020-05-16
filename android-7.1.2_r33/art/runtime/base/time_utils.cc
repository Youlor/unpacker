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

#include <inttypes.h>
#include <limits>
#include <sstream>

#include "time_utils.h"

#include "base/logging.h"
#include "base/stringprintf.h"

#if defined(__APPLE__)
#include <sys/time.h>
#endif

namespace art {

std::string PrettyDuration(uint64_t nano_duration, size_t max_fraction_digits) {
  if (nano_duration == 0) {
    return "0";
  } else {
    return FormatDuration(nano_duration, GetAppropriateTimeUnit(nano_duration),
                          max_fraction_digits);
  }
}

TimeUnit GetAppropriateTimeUnit(uint64_t nano_duration) {
  const uint64_t one_sec = 1000 * 1000 * 1000;
  const uint64_t one_ms  = 1000 * 1000;
  const uint64_t one_us  = 1000;
  if (nano_duration >= one_sec) {
    return kTimeUnitSecond;
  } else if (nano_duration >= one_ms) {
    return kTimeUnitMillisecond;
  } else if (nano_duration >= one_us) {
    return kTimeUnitMicrosecond;
  } else {
    return kTimeUnitNanosecond;
  }
}

uint64_t GetNsToTimeUnitDivisor(TimeUnit time_unit) {
  const uint64_t one_sec = 1000 * 1000 * 1000;
  const uint64_t one_ms  = 1000 * 1000;
  const uint64_t one_us  = 1000;

  switch (time_unit) {
    case kTimeUnitSecond:
      return one_sec;
    case kTimeUnitMillisecond:
      return one_ms;
    case kTimeUnitMicrosecond:
      return one_us;
    case kTimeUnitNanosecond:
      return 1;
  }
  return 0;
}

std::string FormatDuration(uint64_t nano_duration, TimeUnit time_unit,
                           size_t max_fraction_digits) {
  const char* unit = nullptr;
  uint64_t divisor = GetNsToTimeUnitDivisor(time_unit);
  switch (time_unit) {
    case kTimeUnitSecond:
      unit = "s";
      break;
    case kTimeUnitMillisecond:
      unit = "ms";
      break;
    case kTimeUnitMicrosecond:
      unit = "us";
      break;
    case kTimeUnitNanosecond:
      unit = "ns";
      break;
  }
  const uint64_t whole_part = nano_duration / divisor;
  uint64_t fractional_part = nano_duration % divisor;
  if (fractional_part == 0) {
    return StringPrintf("%" PRIu64 "%s", whole_part, unit);
  } else {
    static constexpr size_t kMaxDigits = 30;
    size_t avail_digits = kMaxDigits;
    char fraction_buffer[kMaxDigits];
    char* ptr = fraction_buffer;
    uint64_t multiplier = 10;
    // This infinite loops if fractional part is 0.
    while (avail_digits > 1 && fractional_part * multiplier < divisor) {
      multiplier *= 10;
      *ptr++ = '0';
      avail_digits--;
    }
    snprintf(ptr, avail_digits, "%" PRIu64, fractional_part);
    fraction_buffer[std::min(kMaxDigits - 1, max_fraction_digits)] = '\0';
    return StringPrintf("%" PRIu64 ".%s%s", whole_part, fraction_buffer, unit);
  }
}

std::string GetIsoDate() {
  time_t now = time(nullptr);
  tm tmbuf;
  tm* ptm = localtime_r(&now, &tmbuf);
  return StringPrintf("%04d-%02d-%02d %02d:%02d:%02d",
      ptm->tm_year + 1900, ptm->tm_mon+1, ptm->tm_mday,
      ptm->tm_hour, ptm->tm_min, ptm->tm_sec);
}

uint64_t MilliTime() {
#if defined(__linux__)
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000) + now.tv_nsec / UINT64_C(1000000);
#else  // __APPLE__
  timeval now;
  gettimeofday(&now, nullptr);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000) + now.tv_usec / UINT64_C(1000);
#endif
}

uint64_t MicroTime() {
#if defined(__linux__)
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000) + now.tv_nsec / UINT64_C(1000);
#else  // __APPLE__
  timeval now;
  gettimeofday(&now, nullptr);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000) + now.tv_usec;
#endif
}

uint64_t NanoTime() {
#if defined(__linux__)
  timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000000) + now.tv_nsec;
#else  // __APPLE__
  timeval now;
  gettimeofday(&now, nullptr);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000000) + now.tv_usec * UINT64_C(1000);
#endif
}

uint64_t ThreadCpuNanoTime() {
#if defined(__linux__)
  timespec now;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &now);
  return static_cast<uint64_t>(now.tv_sec) * UINT64_C(1000000000) + now.tv_nsec;
#else  // __APPLE__
  UNIMPLEMENTED(WARNING);
  return -1;
#endif
}

void NanoSleep(uint64_t ns) {
  timespec tm;
  tm.tv_sec = ns / MsToNs(1000);
  tm.tv_nsec = ns - static_cast<uint64_t>(tm.tv_sec) * MsToNs(1000);
  nanosleep(&tm, nullptr);
}

void InitTimeSpec(bool absolute, int clock, int64_t ms, int32_t ns, timespec* ts) {
  if (absolute) {
#if !defined(__APPLE__)
    clock_gettime(clock, ts);
#else
    UNUSED(clock);
    timeval tv;
    gettimeofday(&tv, nullptr);
    ts->tv_sec = tv.tv_sec;
    ts->tv_nsec = tv.tv_usec * 1000;
#endif
  } else {
    ts->tv_sec = 0;
    ts->tv_nsec = 0;
  }

  int64_t end_sec = ts->tv_sec + ms / 1000;
  constexpr int32_t int32_max = std::numeric_limits<int32_t>::max();
  if (UNLIKELY(end_sec >= int32_max)) {
    // Either ms was intended to denote an infinite timeout, or we have a
    // problem. The former generally uses the largest possible millisecond
    // or nanosecond value.  Log only in the latter case.
    constexpr int64_t int64_max = std::numeric_limits<int64_t>::max();
    if (ms != int64_max && ms != int64_max / (1000 * 1000)) {
      LOG(INFO) << "Note: end time exceeds INT32_MAX: " << end_sec;
    }
    end_sec = int32_max - 1;  // Allow for increment below.
  }
  ts->tv_sec = end_sec;
  ts->tv_nsec = (ts->tv_nsec + (ms % 1000) * 1000000) + ns;

  // Catch rollover.
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_sec++;
    ts->tv_nsec -= 1000000000L;
  }
}

}  // namespace art
