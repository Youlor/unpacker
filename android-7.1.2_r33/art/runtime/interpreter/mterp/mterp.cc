/*
 * Copyright (C) 2016 The Android Open Source Project
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

/*
 * Mterp entry point and support functions.
 */
#include "interpreter/interpreter_common.h"
#include "entrypoints/entrypoint_utils-inl.h"
#include "mterp.h"
#include "debugger.h"

namespace art {
namespace interpreter {
/*
 * Verify some constants used by the mterp interpreter.
 */
void CheckMterpAsmConstants() {
  /*
   * If we're using computed goto instruction transitions, make sure
   * none of the handlers overflows the 128-byte limit.  This won't tell
   * which one did, but if any one is too big the total size will
   * overflow.
   */
  const int width = 128;
  int interp_size = (uintptr_t) artMterpAsmInstructionEnd -
                    (uintptr_t) artMterpAsmInstructionStart;
  if ((interp_size == 0) || (interp_size != (art::kNumPackedOpcodes * width))) {
      LOG(art::FATAL) << "ERROR: unexpected asm interp size " << interp_size
                      << "(did an instruction handler exceed " << width << " bytes?)";
  }
}

void InitMterpTls(Thread* self) {
  self->SetMterpDefaultIBase(artMterpAsmInstructionStart);
  self->SetMterpAltIBase(artMterpAsmAltInstructionStart);
  self->SetMterpCurrentIBase(TraceExecutionEnabled() ?
                             artMterpAsmAltInstructionStart :
                             artMterpAsmInstructionStart);
}

/*
 * Find the matching case.  Returns the offset to the handler instructions.
 *
 * Returns 3 if we don't find a match (it's the size of the sparse-switch
 * instruction).
 */
extern "C" int32_t MterpDoSparseSwitch(const uint16_t* switchData, int32_t testVal) {
  const int kInstrLen = 3;
  uint16_t size;
  const int32_t* keys;
  const int32_t* entries;

  /*
   * Sparse switch data format:
   *  ushort ident = 0x0200   magic value
   *  ushort size             number of entries in the table; > 0
   *  int keys[size]          keys, sorted low-to-high; 32-bit aligned
   *  int targets[size]       branch targets, relative to switch opcode
   *
   * Total size is (2+size*4) 16-bit code units.
   */

  uint16_t signature = *switchData++;
  DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kSparseSwitchSignature));

  size = *switchData++;

  /* The keys are guaranteed to be aligned on a 32-bit boundary;
   * we can treat them as a native int array.
   */
  keys = reinterpret_cast<const int32_t*>(switchData);

  /* The entries are guaranteed to be aligned on a 32-bit boundary;
   * we can treat them as a native int array.
   */
  entries = keys + size;

  /*
   * Binary-search through the array of keys, which are guaranteed to
   * be sorted low-to-high.
   */
  int lo = 0;
  int hi = size - 1;
  while (lo <= hi) {
    int mid = (lo + hi) >> 1;

    int32_t foundVal = keys[mid];
    if (testVal < foundVal) {
      hi = mid - 1;
    } else if (testVal > foundVal) {
      lo = mid + 1;
    } else {
      return entries[mid];
    }
  }
  return kInstrLen;
}

extern "C" int32_t MterpDoPackedSwitch(const uint16_t* switchData, int32_t testVal) {
  const int kInstrLen = 3;

  /*
   * Packed switch data format:
   *  ushort ident = 0x0100   magic value
   *  ushort size             number of entries in the table
   *  int first_key           first (and lowest) switch case value
   *  int targets[size]       branch targets, relative to switch opcode
   *
   * Total size is (4+size*2) 16-bit code units.
   */
  uint16_t signature = *switchData++;
  DCHECK_EQ(signature, static_cast<uint16_t>(art::Instruction::kPackedSwitchSignature));

  uint16_t size = *switchData++;

  int32_t firstKey = *switchData++;
  firstKey |= (*switchData++) << 16;

  int index = testVal - firstKey;
  if (index < 0 || index >= size) {
    return kInstrLen;
  }

  /*
   * The entries are guaranteed to be aligned on a 32-bit boundary;
   * we can treat them as a native int array.
   */
  const int32_t* entries = reinterpret_cast<const int32_t*>(switchData);
  return entries[index];
}

extern "C" bool MterpShouldSwitchInterpreters()
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  return instrumentation->NonJitProfilingActive() || Dbg::IsDebuggerActive();
}


extern "C" bool MterpInvokeVirtual(Thread* self, ShadowFrame* shadow_frame,
                                   uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kVirtual, false, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeSuper(Thread* self, ShadowFrame* shadow_frame,
                                 uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kSuper, false, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeInterface(Thread* self, ShadowFrame* shadow_frame,
                                     uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kInterface, false, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeDirect(Thread* self, ShadowFrame* shadow_frame,
                                  uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kDirect, false, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeStatic(Thread* self, ShadowFrame* shadow_frame,
                                  uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kStatic, false, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeVirtualRange(Thread* self, ShadowFrame* shadow_frame,
                                        uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kVirtual, true, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeSuperRange(Thread* self, ShadowFrame* shadow_frame,
                                      uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kSuper, true, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeInterfaceRange(Thread* self, ShadowFrame* shadow_frame,
                                          uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kInterface, true, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeDirectRange(Thread* self, ShadowFrame* shadow_frame,
                                       uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kDirect, true, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeStaticRange(Thread* self, ShadowFrame* shadow_frame,
                                       uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvoke<kStatic, true, false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeVirtualQuick(Thread* self, ShadowFrame* shadow_frame,
                                        uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvokeVirtualQuick<false>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" bool MterpInvokeVirtualQuickRange(Thread* self, ShadowFrame* shadow_frame,
                                             uint16_t* dex_pc_ptr,  uint16_t inst_data )
    SHARED_REQUIRES(Locks::mutator_lock_) {
  JValue* result_register = shadow_frame->GetResultRegister();
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoInvokeVirtualQuick<true>(
      self, *shadow_frame, inst, inst_data, result_register);
}

extern "C" void MterpThreadFenceForConstructor() {
  QuasiAtomic::ThreadFenceForConstructor();
}

extern "C" bool MterpConstString(uint32_t index, uint32_t tgt_vreg, ShadowFrame* shadow_frame,
                                 Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  String* s = ResolveString(self, *shadow_frame,  index);
  if (UNLIKELY(s == nullptr)) {
    return true;
  }
  shadow_frame->SetVRegReference(tgt_vreg, s);
  return false;
}

extern "C" bool MterpConstClass(uint32_t index, uint32_t tgt_vreg, ShadowFrame* shadow_frame,
                                Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Class* c = ResolveVerifyAndClinit(index, shadow_frame->GetMethod(), self, false, false);
  if (UNLIKELY(c == nullptr)) {
    return true;
  }
  shadow_frame->SetVRegReference(tgt_vreg, c);
  return false;
}

extern "C" bool MterpCheckCast(uint32_t index, StackReference<mirror::Object>* vreg_addr,
                               art::ArtMethod* method, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Class* c = ResolveVerifyAndClinit(index, method, self, false, false);
  if (UNLIKELY(c == nullptr)) {
    return true;
  }
  // Must load obj from vreg following ResolveVerifyAndClinit due to moving gc.
  Object* obj = vreg_addr->AsMirrorPtr();
  if (UNLIKELY(obj != nullptr && !obj->InstanceOf(c))) {
    ThrowClassCastException(c, obj->GetClass());
    return true;
  }
  return false;
}

extern "C" bool MterpInstanceOf(uint32_t index, StackReference<mirror::Object>* vreg_addr,
                                art::ArtMethod* method, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  Class* c = ResolveVerifyAndClinit(index, method, self, false, false);
  if (UNLIKELY(c == nullptr)) {
    return false;  // Caller will check for pending exception.  Return value unimportant.
  }
  // Must load obj from vreg following ResolveVerifyAndClinit due to moving gc.
  Object* obj = vreg_addr->AsMirrorPtr();
  return (obj != nullptr) && obj->InstanceOf(c);
}

extern "C" bool MterpFillArrayData(Object* obj, const Instruction::ArrayDataPayload* payload)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  return FillArrayData(obj, payload);
}

extern "C" bool MterpNewInstance(ShadowFrame* shadow_frame, Thread* self, uint32_t inst_data)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  Object* obj = nullptr;
  Class* c = ResolveVerifyAndClinit(inst->VRegB_21c(), shadow_frame->GetMethod(),
                                    self, false, false);
  if (LIKELY(c != nullptr)) {
    if (UNLIKELY(c->IsStringClass())) {
      gc::AllocatorType allocator_type = Runtime::Current()->GetHeap()->GetCurrentAllocator();
      mirror::SetStringCountVisitor visitor(0);
      obj = String::Alloc<true>(self, 0, allocator_type, visitor);
    } else {
      obj = AllocObjectFromCode<false, true>(
        inst->VRegB_21c(), shadow_frame->GetMethod(), self,
        Runtime::Current()->GetHeap()->GetCurrentAllocator());
    }
  }
  if (UNLIKELY(obj == nullptr)) {
    return false;
  }
  obj->GetClass()->AssertInitializedOrInitializingInThread(self);
  shadow_frame->SetVRegReference(inst->VRegA_21c(inst_data), obj);
  return true;
}

extern "C" bool MterpSputObject(ShadowFrame* shadow_frame, uint16_t* dex_pc_ptr,
                                uint32_t inst_data, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoFieldPut<StaticObjectWrite, Primitive::kPrimNot, false, false>
      (self, *shadow_frame, inst, inst_data);
}

extern "C" bool MterpIputObject(ShadowFrame* shadow_frame, uint16_t* dex_pc_ptr,
                                uint32_t inst_data, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoFieldPut<InstanceObjectWrite, Primitive::kPrimNot, false, false>
      (self, *shadow_frame, inst, inst_data);
}

extern "C" bool MterpIputObjectQuick(ShadowFrame* shadow_frame, uint16_t* dex_pc_ptr,
                                     uint32_t inst_data)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoIPutQuick<Primitive::kPrimNot, false>(*shadow_frame, inst, inst_data);
}

extern "C" bool MterpAputObject(ShadowFrame* shadow_frame, uint16_t* dex_pc_ptr,
                                uint32_t inst_data)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  Object* a = shadow_frame->GetVRegReference(inst->VRegB_23x());
  if (UNLIKELY(a == nullptr)) {
    return false;
  }
  int32_t index = shadow_frame->GetVReg(inst->VRegC_23x());
  Object* val = shadow_frame->GetVRegReference(inst->VRegA_23x(inst_data));
  ObjectArray<Object>* array = a->AsObjectArray<Object>();
  if (array->CheckIsValidIndex(index) && array->CheckAssignable(val)) {
    array->SetWithoutChecks<false>(index, val);
    return true;
  }
  return false;
}

extern "C" bool MterpFilledNewArray(ShadowFrame* shadow_frame, uint16_t* dex_pc_ptr,
                                    Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoFilledNewArray<false, false, false>(inst, *shadow_frame, self,
                                               shadow_frame->GetResultRegister());
}

extern "C" bool MterpFilledNewArrayRange(ShadowFrame* shadow_frame, uint16_t* dex_pc_ptr,
                                         Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  return DoFilledNewArray<true, false, false>(inst, *shadow_frame, self,
                                              shadow_frame->GetResultRegister());
}

extern "C" bool MterpNewArray(ShadowFrame* shadow_frame, uint16_t* dex_pc_ptr,
                              uint32_t inst_data, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(dex_pc_ptr);
  int32_t length = shadow_frame->GetVReg(inst->VRegB_22c(inst_data));
  Object* obj = AllocArrayFromCode<false, true>(
      inst->VRegC_22c(), length, shadow_frame->GetMethod(), self,
      Runtime::Current()->GetHeap()->GetCurrentAllocator());
  if (UNLIKELY(obj == nullptr)) {
      return false;
  }
  shadow_frame->SetVRegReference(inst->VRegA_22c(inst_data), obj);
  return true;
}

extern "C" bool MterpHandleException(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  DCHECK(self->IsExceptionPending());
  const instrumentation::Instrumentation* const instrumentation =
      Runtime::Current()->GetInstrumentation();
  uint32_t found_dex_pc = FindNextInstructionFollowingException(self, *shadow_frame,
                                                                shadow_frame->GetDexPC(),
                                                                instrumentation);
  if (found_dex_pc == DexFile::kDexNoIndex) {
    return false;
  }
  // OK - we can deal with it.  Update and continue.
  shadow_frame->SetDexPC(found_dex_pc);
  return true;
}

extern "C" void MterpCheckBefore(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  if (inst->Opcode(inst_data) == Instruction::MOVE_EXCEPTION) {
    self->AssertPendingException();
  } else {
    self->AssertNoPendingException();
  }
  TraceExecution(*shadow_frame, inst, shadow_frame->GetDexPC());
}

extern "C" void MterpLogDivideByZeroException(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "DivideByZero: " << inst->Opcode(inst_data);
}

extern "C" void MterpLogArrayIndexException(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "ArrayIndex: " << inst->Opcode(inst_data);
}

extern "C" void MterpLogNegativeArraySizeException(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "NegativeArraySize: " << inst->Opcode(inst_data);
}

extern "C" void MterpLogNoSuchMethodException(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "NoSuchMethod: " << inst->Opcode(inst_data);
}

extern "C" void MterpLogExceptionThrownException(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "ExceptionThrown: " << inst->Opcode(inst_data);
}

extern "C" void MterpLogNullObjectException(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "NullObject: " << inst->Opcode(inst_data);
}

extern "C" void MterpLogFallback(Thread* self, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "Fallback: " << inst->Opcode(inst_data) << ", Suspend Pending?: "
            << self->IsExceptionPending();
}

extern "C" void MterpLogOSR(Thread* self, ShadowFrame* shadow_frame, int32_t offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  LOG(INFO) << "OSR: " << inst->Opcode(inst_data) << ", offset = " << offset;
}

extern "C" void MterpLogSuspendFallback(Thread* self, ShadowFrame* shadow_frame, uint32_t flags)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  UNUSED(self);
  const Instruction* inst = Instruction::At(shadow_frame->GetDexPCPtr());
  uint16_t inst_data = inst->Fetch16(0);
  if (flags & kCheckpointRequest) {
    LOG(INFO) << "Checkpoint fallback: " << inst->Opcode(inst_data);
  } else if (flags & kSuspendRequest) {
    LOG(INFO) << "Suspend fallback: " << inst->Opcode(inst_data);
  }
}

extern "C" bool MterpSuspendCheck(Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  self->AllowThreadSuspension();
  return MterpShouldSwitchInterpreters();
}

extern "C" int artSet64IndirectStaticFromMterp(uint32_t field_idx, ArtMethod* referrer,
                                               uint64_t* new_value, Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ScopedQuickEntrypointChecks sqec(self);
  ArtField* field = FindFieldFast(field_idx, referrer, StaticPrimitiveWrite, sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(field->GetDeclaringClass(), *new_value);
    return 0;  // success
  }
  field = FindFieldFromCode<StaticPrimitiveWrite, true>(field_idx, referrer, self, sizeof(int64_t));
  if (LIKELY(field != nullptr)) {
    // Compiled code can't use transactional mode.
    field->Set64<false>(field->GetDeclaringClass(), *new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet8InstanceFromMterp(uint32_t field_idx, mirror::Object* obj, uint8_t new_value,
                                        ArtMethod* referrer)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite, sizeof(int8_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    if (type == Primitive::kPrimBoolean) {
      field->SetBoolean<false>(obj, new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimByte, type);
      field->SetByte<false>(obj, new_value);
    }
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet16InstanceFromMterp(uint32_t field_idx, mirror::Object* obj, uint16_t new_value,
                                        ArtMethod* referrer)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int16_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    Primitive::Type type = field->GetTypeAsPrimitiveType();
    if (type == Primitive::kPrimChar) {
      field->SetChar<false>(obj, new_value);
    } else {
      DCHECK_EQ(Primitive::kPrimShort, type);
      field->SetShort<false>(obj, new_value);
    }
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet32InstanceFromMterp(uint32_t field_idx, mirror::Object* obj,
                                         uint32_t new_value, ArtMethod* referrer)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int32_t));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    field->Set32<false>(obj, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSet64InstanceFromMterp(uint32_t field_idx, mirror::Object* obj,
                                         uint64_t* new_value, ArtMethod* referrer)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtField* field = FindFieldFast(field_idx, referrer, InstancePrimitiveWrite,
                                          sizeof(int64_t));
  if (LIKELY(field != nullptr  && obj != nullptr)) {
    field->Set64<false>(obj, *new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" int artSetObjInstanceFromMterp(uint32_t field_idx, mirror::Object* obj,
                                          mirror::Object* new_value, ArtMethod* referrer)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtField* field = FindFieldFast(field_idx, referrer, InstanceObjectWrite,
                                          sizeof(mirror::HeapReference<mirror::Object>));
  if (LIKELY(field != nullptr && obj != nullptr)) {
    field->SetObj<false>(obj, new_value);
    return 0;  // success
  }
  return -1;  // failure
}

extern "C" mirror::Object* artAGetObjectFromMterp(mirror::Object* arr, int32_t index)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (UNLIKELY(arr == nullptr)) {
    ThrowNullPointerExceptionFromInterpreter();
    return nullptr;
  }
  ObjectArray<Object>* array = arr->AsObjectArray<Object>();
  if (LIKELY(array->CheckIsValidIndex(index))) {
    return array->GetWithoutChecks(index);
  } else {
    return nullptr;
  }
}

extern "C" mirror::Object* artIGetObjectFromMterp(mirror::Object* obj, uint32_t field_offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  if (UNLIKELY(obj == nullptr)) {
    ThrowNullPointerExceptionFromInterpreter();
    return nullptr;
  }
  return obj->GetFieldObject<mirror::Object>(MemberOffset(field_offset));
}

/*
 * Create a hotness_countdown based on the current method hotness_count and profiling
 * mode.  In short, determine how many hotness events we hit before reporting back
 * to the full instrumentation via MterpAddHotnessBatch.  Called once on entry to the method,
 * and regenerated following batch updates.
 */
extern "C" int MterpSetUpHotnessCountdown(ArtMethod* method, ShadowFrame* shadow_frame)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  uint16_t hotness_count = method->GetCounter();
  int32_t countdown_value = jit::kJitHotnessDisabled;
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    int32_t warm_threshold = jit->WarmMethodThreshold();
    int32_t hot_threshold = jit->HotMethodThreshold();
    int32_t osr_threshold = jit->OSRMethodThreshold();
    if (hotness_count < warm_threshold) {
      countdown_value = warm_threshold - hotness_count;
    } else if (hotness_count < hot_threshold) {
      countdown_value = hot_threshold - hotness_count;
    } else if (hotness_count < osr_threshold) {
      countdown_value = osr_threshold - hotness_count;
    } else {
      countdown_value = jit::kJitCheckForOSR;
    }
    if (jit::Jit::ShouldUsePriorityThreadWeight()) {
      int32_t priority_thread_weight = jit->PriorityThreadWeight();
      countdown_value = std::min(countdown_value, countdown_value / priority_thread_weight);
    }
  }
  /*
   * The actual hotness threshold may exceed the range of our int16_t countdown value.  This is
   * not a problem, though.  We can just break it down into smaller chunks.
   */
  countdown_value = std::min(countdown_value,
                             static_cast<int32_t>(std::numeric_limits<int16_t>::max()));
  shadow_frame->SetCachedHotnessCountdown(countdown_value);
  shadow_frame->SetHotnessCountdown(countdown_value);
  return countdown_value;
}

/*
 * Report a batch of hotness events to the instrumentation and then return the new
 * countdown value to the next time we should report.
 */
extern "C" int16_t MterpAddHotnessBatch(ArtMethod* method,
                                        ShadowFrame* shadow_frame,
                                        Thread* self)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (jit != nullptr) {
    int16_t count = shadow_frame->GetCachedHotnessCountdown() - shadow_frame->GetHotnessCountdown();
    jit->AddSamples(self, method, count, /*with_backedges*/ true);
  }
  return MterpSetUpHotnessCountdown(method, shadow_frame);
}

// TUNING: Unused by arm/arm64/x86/x86_64.  Remove when mips/mips64 mterps support batch updates.
extern "C" bool  MterpProfileBranch(Thread* self, ShadowFrame* shadow_frame, int32_t offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtMethod* method = shadow_frame->GetMethod();
  JValue* result = shadow_frame->GetResultRegister();
  uint32_t dex_pc = shadow_frame->GetDexPC();
  jit::Jit* jit = Runtime::Current()->GetJit();
  if ((jit != nullptr) && (offset <= 0)) {
    jit->AddSamples(self, method, 1, /*with_backedges*/ true);
  }
  int16_t countdown_value = MterpSetUpHotnessCountdown(method, shadow_frame);
  if (countdown_value == jit::kJitCheckForOSR) {
    return jit::Jit::MaybeDoOnStackReplacement(self, method, dex_pc, offset, result);
  } else {
    return false;
  }
}

extern "C" bool MterpMaybeDoOnStackReplacement(Thread* self,
                                               ShadowFrame* shadow_frame,
                                               int32_t offset)
    SHARED_REQUIRES(Locks::mutator_lock_) {
  ArtMethod* method = shadow_frame->GetMethod();
  JValue* result = shadow_frame->GetResultRegister();
  uint32_t dex_pc = shadow_frame->GetDexPC();
  jit::Jit* jit = Runtime::Current()->GetJit();
  if (offset <= 0) {
    // Keep updating hotness in case a compilation request was dropped.  Eventually it will retry.
    jit->AddSamples(self, method, 1, /*with_backedges*/ true);
  }
  // Assumes caller has already determined that an OSR check is appropriate.
  return jit::Jit::MaybeDoOnStackReplacement(self, method, dex_pc, offset, result);
}

}  // namespace interpreter
}  // namespace art
