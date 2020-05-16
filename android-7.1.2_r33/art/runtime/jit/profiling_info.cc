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

#include "profiling_info.h"

#include "art_method-inl.h"
#include "dex_instruction.h"
#include "jit/jit.h"
#include "jit/jit_code_cache.h"
#include "scoped_thread_state_change.h"
#include "thread.h"

namespace art {

ProfilingInfo::ProfilingInfo(ArtMethod* method, const std::vector<uint32_t>& entries)
      : number_of_inline_caches_(entries.size()),
        method_(method),
        is_method_being_compiled_(false),
        is_osr_method_being_compiled_(false),
        current_inline_uses_(0),
        saved_entry_point_(nullptr) {
  memset(&cache_, 0, number_of_inline_caches_ * sizeof(InlineCache));
  for (size_t i = 0; i < number_of_inline_caches_; ++i) {
    cache_[i].dex_pc_ = entries[i];
  }
  if (method->IsCopied()) {
    // GetHoldingClassOfCopiedMethod is expensive, but creating a profiling info for a copied method
    // appears to happen very rarely in practice.
    holding_class_ = GcRoot<mirror::Class>(
        Runtime::Current()->GetClassLinker()->GetHoldingClassOfCopiedMethod(method));
  } else {
    holding_class_ = GcRoot<mirror::Class>(method->GetDeclaringClass());
  }
  DCHECK(!holding_class_.IsNull());
}

bool ProfilingInfo::Create(Thread* self, ArtMethod* method, bool retry_allocation) {
  // Walk over the dex instructions of the method and keep track of
  // instructions we are interested in profiling.
  DCHECK(!method->IsNative());

  const DexFile::CodeItem& code_item = *method->GetCodeItem();
  const uint16_t* code_ptr = code_item.insns_;
  const uint16_t* code_end = code_item.insns_ + code_item.insns_size_in_code_units_;

  uint32_t dex_pc = 0;
  std::vector<uint32_t> entries;
  while (code_ptr < code_end) {
    const Instruction& instruction = *Instruction::At(code_ptr);
    switch (instruction.Opcode()) {
      case Instruction::INVOKE_VIRTUAL:
      case Instruction::INVOKE_VIRTUAL_RANGE:
      case Instruction::INVOKE_VIRTUAL_QUICK:
      case Instruction::INVOKE_VIRTUAL_RANGE_QUICK:
      case Instruction::INVOKE_INTERFACE:
      case Instruction::INVOKE_INTERFACE_RANGE:
        entries.push_back(dex_pc);
        break;

      default:
        break;
    }
    dex_pc += instruction.SizeInCodeUnits();
    code_ptr += instruction.SizeInCodeUnits();
  }

  // We always create a `ProfilingInfo` object, even if there is no instruction we are
  // interested in. The JIT code cache internally uses it.

  // Allocate the `ProfilingInfo` object int the JIT's data space.
  jit::JitCodeCache* code_cache = Runtime::Current()->GetJit()->GetCodeCache();
  return code_cache->AddProfilingInfo(self, method, entries, retry_allocation) != nullptr;
}

InlineCache* ProfilingInfo::GetInlineCache(uint32_t dex_pc) {
  InlineCache* cache = nullptr;
  // TODO: binary search if array is too long.
  for (size_t i = 0; i < number_of_inline_caches_; ++i) {
    if (cache_[i].dex_pc_ == dex_pc) {
      cache = &cache_[i];
      break;
    }
  }
  return cache;
}

void ProfilingInfo::AddInvokeInfo(uint32_t dex_pc, mirror::Class* cls) {
  InlineCache* cache = GetInlineCache(dex_pc);
  CHECK(cache != nullptr) << PrettyMethod(method_) << "@" << dex_pc;
  for (size_t i = 0; i < InlineCache::kIndividualCacheSize; ++i) {
    mirror::Class* existing = cache->classes_[i].Read();
    if (existing == cls) {
      // Receiver type is already in the cache, nothing else to do.
      return;
    } else if (existing == nullptr) {
      // Cache entry is empty, try to put `cls` in it.
      GcRoot<mirror::Class> expected_root(nullptr);
      GcRoot<mirror::Class> desired_root(cls);
      if (!reinterpret_cast<Atomic<GcRoot<mirror::Class>>*>(&cache->classes_[i])->
              CompareExchangeStrongSequentiallyConsistent(expected_root, desired_root)) {
        // Some other thread put a class in the cache, continue iteration starting at this
        // entry in case the entry contains `cls`.
        --i;
      } else {
        // We successfully set `cls`, just return.
        // Since the instrumentation is marked from the declaring class we need to mark the card so
        // that mod-union tables and card rescanning know about the update.
        // Note that the declaring class is not necessarily the holding class if the method is
        // copied. We need the card mark to be in the holding class since that is from where we
        // will visit the profiling info.
        if (!holding_class_.IsNull()) {
          Runtime::Current()->GetHeap()->WriteBarrierEveryFieldOf(holding_class_.Read());
        }
        return;
      }
    }
  }
  // Unsuccessfull - cache is full, making it megamorphic. We do not DCHECK it though,
  // as the garbage collector might clear the entries concurrently.
}

}  // namespace art
