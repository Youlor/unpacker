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

#include "thread.h"

#include <sys/syscall.h>
#include <sys/types.h>

#include "asm_support_x86.h"
#include "base/macros.h"
#include "thread-inl.h"
#include "thread_list.h"

#if defined(__APPLE__)
#include <architecture/i386/table.h>
#include <i386/user_ldt.h>
struct descriptor_table_entry_t {
  uint16_t limit0;
  uint16_t base0;
  unsigned base1: 8, type: 4, s: 1, dpl: 2, p: 1;
  unsigned limit: 4, avl: 1, l: 1, d: 1, g: 1, base2: 8;
} __attribute__((packed));
#define MODIFY_LDT_CONTENTS_DATA 0
#else
#include <asm/ldt.h>
#endif

namespace art {

void Thread::InitCpu() {
  // Take the ldt lock, Thread::Current isn't yet established.
  MutexLock mu(nullptr, *Locks::modify_ldt_lock_);

  const uintptr_t base = reinterpret_cast<uintptr_t>(this);
  const size_t limit = sizeof(Thread);

  const int contents = MODIFY_LDT_CONTENTS_DATA;
  const int seg_32bit = 1;
  const int read_exec_only = 0;
  const int limit_in_pages = 1;
  const int seg_not_present = 0;
  const int useable = 1;

  int entry_number;
  uint16_t table_indicator;

#if defined(__APPLE__)
  descriptor_table_entry_t entry;
  memset(&entry, 0, sizeof(entry));
  entry.limit0 = (limit & 0x0ffff);
  entry.limit  = (limit & 0xf0000) >> 16;
  entry.base0 = (base & 0x0000ffff);
  entry.base1 = (base & 0x00ff0000) >> 16;
  entry.base2 = (base & 0xff000000) >> 24;
  entry.type = ((read_exec_only ^ 1) << 1) | (contents << 2);
  entry.s = 1;
  entry.dpl = 0x3;
  entry.p = seg_not_present ^ 1;
  entry.avl = useable;
  entry.l = 0;
  entry.d = seg_32bit;
  entry.g = limit_in_pages;

  entry_number = i386_set_ldt(LDT_AUTO_ALLOC, reinterpret_cast<ldt_entry*>(&entry), 1);
  if (entry_number == -1) {
    PLOG(FATAL) << "i386_set_ldt failed";
  }

  table_indicator = 1 << 2;  // LDT
#else
  // We use a GDT entry on Linux.
  user_desc gdt_entry;
  memset(&gdt_entry, 0, sizeof(gdt_entry));

  // On Linux, there are 3 TLS GDT entries. We use one of those to to store our segment descriptor
  // data.
  //
  // This entry must be shared, as the kernel only guarantees three TLS entries. For simplicity
  // (and locality), use this local global, which practically becomes readonly after the first
  // (startup) thread of the runtime has been initialized (during Runtime::Start()).
  //
  // We also share this between all runtimes in the process. This is both for simplicity (one
  // well-known slot) as well as to avoid the three-slot limitation. Downside is that we cannot
  // free the slot when it is known that a runtime stops.
  static unsigned int gdt_entry_number = -1;

  if (gdt_entry_number == static_cast<unsigned int>(-1)) {
    gdt_entry.entry_number = -1;  // Let the kernel choose.
  } else {
    gdt_entry.entry_number = gdt_entry_number;
  }
  gdt_entry.base_addr = base;
  gdt_entry.limit = limit;
  gdt_entry.seg_32bit = seg_32bit;
  gdt_entry.contents = contents;
  gdt_entry.read_exec_only = read_exec_only;
  gdt_entry.limit_in_pages = limit_in_pages;
  gdt_entry.seg_not_present = seg_not_present;
  gdt_entry.useable = useable;
  int rc = syscall(__NR_set_thread_area, &gdt_entry);
  if (rc != -1) {
    entry_number = gdt_entry.entry_number;
    if (gdt_entry_number == static_cast<unsigned int>(-1)) {
      gdt_entry_number = entry_number;  // Save the kernel-assigned entry number.
    }
  } else {
    PLOG(FATAL) << "set_thread_area failed";
    UNREACHABLE();
  }
  table_indicator = 0;  // GDT
#endif

  // Change %fs to be new DT entry.
  uint16_t rpl = 3;  // Requested privilege level
  uint16_t selector = (entry_number << 3) | table_indicator | rpl;
  __asm__ __volatile__("movw %w0, %%fs"
      :    // output
      : "q"(selector)  // input
      :);  // clobber

  // Allow easy indirection back to Thread*.
  tlsPtr_.self = this;

  // Sanity check that reads from %fs point to this Thread*.
  Thread* self_check;
  CHECK_EQ(THREAD_SELF_OFFSET, SelfOffset<4>().Int32Value());
  __asm__ __volatile__("movl %%fs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);

  // Sanity check other offsets.
  CHECK_EQ(THREAD_EXCEPTION_OFFSET, ExceptionOffset<4>().Int32Value());
  CHECK_EQ(THREAD_CARD_TABLE_OFFSET, CardTableOffset<4>().Int32Value());
  CHECK_EQ(THREAD_ID_OFFSET, ThinLockIdOffset<4>().Int32Value());
}

void Thread::CleanupCpu() {
  MutexLock mu(this, *Locks::modify_ldt_lock_);

  // Sanity check that reads from %fs point to this Thread*.
  Thread* self_check;
  __asm__ __volatile__("movl %%fs:(%1), %0"
      : "=r"(self_check)  // output
      : "r"(THREAD_SELF_OFFSET)  // input
      :);  // clobber
  CHECK_EQ(self_check, this);

  // Extract the LDT entry number from the FS register.
  uint16_t selector;
  __asm__ __volatile__("movw %%fs, %w0"
      : "=q"(selector)  // output
      :  // input
      :);  // clobber

  // Free LDT entry.
#if defined(__APPLE__)
  // TODO: release selectors on OS/X this is a leak which will cause ldt entries to be exhausted
  // after enough threads are created. However, the following code results in kernel panics in OS/X
  // 10.9.
  UNUSED(selector);
  // i386_set_ldt(selector >> 3, 0, 1);
#else
  // Note if we wanted to clean up the GDT entry, we would do that here, when the *last* thread
  // is being deleted. But see the comment on gdt_entry_number. Code would look like this:
  //
  // user_desc gdt_entry;
  // memset(&gdt_entry, 0, sizeof(gdt_entry));
  // gdt_entry.entry_number = selector >> 3;
  // gdt_entry.contents = MODIFY_LDT_CONTENTS_DATA;
  // // "Empty" = Delete = seg_not_present==1 && read_exec_only==1.
  // gdt_entry.seg_not_present = 1;
  // gdt_entry.read_exec_only = 1;
  // syscall(__NR_set_thread_area, &gdt_entry);
  UNUSED(selector);
#endif
}

}  // namespace art
