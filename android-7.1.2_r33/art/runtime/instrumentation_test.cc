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

#include "instrumentation.h"

#include "common_runtime_test.h"
#include "common_throws.h"
#include "class_linker-inl.h"
#include "dex_file.h"
#include "gc/scoped_gc_critical_section.h"
#include "handle_scope-inl.h"
#include "jvalue.h"
#include "runtime.h"
#include "scoped_thread_state_change.h"
#include "thread_list.h"
#include "thread-inl.h"

namespace art {
namespace instrumentation {

class TestInstrumentationListener FINAL : public instrumentation::InstrumentationListener {
 public:
  TestInstrumentationListener()
    : received_method_enter_event(false), received_method_exit_event(false),
      received_method_unwind_event(false), received_dex_pc_moved_event(false),
      received_field_read_event(false), received_field_written_event(false),
      received_exception_caught_event(false), received_branch_event(false),
      received_invoke_virtual_or_interface_event(false) {}

  virtual ~TestInstrumentationListener() {}

  void MethodEntered(Thread* thread ATTRIBUTE_UNUSED,
                     mirror::Object* this_object ATTRIBUTE_UNUSED,
                     ArtMethod* method ATTRIBUTE_UNUSED,
                     uint32_t dex_pc ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_method_enter_event = true;
  }

  void MethodExited(Thread* thread ATTRIBUTE_UNUSED,
                    mirror::Object* this_object ATTRIBUTE_UNUSED,
                    ArtMethod* method ATTRIBUTE_UNUSED,
                    uint32_t dex_pc ATTRIBUTE_UNUSED,
                    const JValue& return_value ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_method_exit_event = true;
  }

  void MethodUnwind(Thread* thread ATTRIBUTE_UNUSED,
                    mirror::Object* this_object ATTRIBUTE_UNUSED,
                    ArtMethod* method ATTRIBUTE_UNUSED,
                    uint32_t dex_pc ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_method_unwind_event = true;
  }

  void DexPcMoved(Thread* thread ATTRIBUTE_UNUSED,
                  mirror::Object* this_object ATTRIBUTE_UNUSED,
                  ArtMethod* method ATTRIBUTE_UNUSED,
                  uint32_t new_dex_pc ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_dex_pc_moved_event = true;
  }

  void FieldRead(Thread* thread ATTRIBUTE_UNUSED,
                 mirror::Object* this_object ATTRIBUTE_UNUSED,
                 ArtMethod* method ATTRIBUTE_UNUSED,
                 uint32_t dex_pc ATTRIBUTE_UNUSED,
                 ArtField* field ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_field_read_event = true;
  }

  void FieldWritten(Thread* thread ATTRIBUTE_UNUSED,
                    mirror::Object* this_object ATTRIBUTE_UNUSED,
                    ArtMethod* method ATTRIBUTE_UNUSED,
                    uint32_t dex_pc ATTRIBUTE_UNUSED,
                    ArtField* field ATTRIBUTE_UNUSED,
                    const JValue& field_value ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_field_written_event = true;
  }

  void ExceptionCaught(Thread* thread ATTRIBUTE_UNUSED,
                       mirror::Throwable* exception_object ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_exception_caught_event = true;
  }

  void Branch(Thread* thread ATTRIBUTE_UNUSED,
              ArtMethod* method ATTRIBUTE_UNUSED,
              uint32_t dex_pc ATTRIBUTE_UNUSED,
              int32_t dex_pc_offset ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_branch_event = true;
  }

  void InvokeVirtualOrInterface(Thread* thread ATTRIBUTE_UNUSED,
                                mirror::Object* this_object ATTRIBUTE_UNUSED,
                                ArtMethod* caller ATTRIBUTE_UNUSED,
                                uint32_t dex_pc ATTRIBUTE_UNUSED,
                                ArtMethod* callee ATTRIBUTE_UNUSED)
      OVERRIDE SHARED_REQUIRES(Locks::mutator_lock_) {
    received_invoke_virtual_or_interface_event = true;
  }

  void Reset() {
    received_method_enter_event = false;
    received_method_exit_event = false;
    received_method_unwind_event = false;
    received_dex_pc_moved_event = false;
    received_field_read_event = false;
    received_field_written_event = false;
    received_exception_caught_event = false;
    received_branch_event = false;
    received_invoke_virtual_or_interface_event = false;
  }

  bool received_method_enter_event;
  bool received_method_exit_event;
  bool received_method_unwind_event;
  bool received_dex_pc_moved_event;
  bool received_field_read_event;
  bool received_field_written_event;
  bool received_exception_caught_event;
  bool received_branch_event;
  bool received_invoke_virtual_or_interface_event;

 private:
  DISALLOW_COPY_AND_ASSIGN(TestInstrumentationListener);
};

class InstrumentationTest : public CommonRuntimeTest {
 public:
  // Unique keys used to test Instrumentation::ConfigureStubs.
  static constexpr const char* kClientOneKey = "TestClient1";
  static constexpr const char* kClientTwoKey = "TestClient2";

  void CheckConfigureStubs(const char* key, Instrumentation::InstrumentationLevel level) {
    ScopedObjectAccess soa(Thread::Current());
    instrumentation::Instrumentation* instr = Runtime::Current()->GetInstrumentation();
    ScopedThreadSuspension sts(soa.Self(), kSuspended);
    gc::ScopedGCCriticalSection gcs(soa.Self(),
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Instrumentation::ConfigureStubs");
    instr->ConfigureStubs(key, level);
  }

  Instrumentation::InstrumentationLevel GetCurrentInstrumentationLevel() {
    return Runtime::Current()->GetInstrumentation()->GetCurrentInstrumentationLevel();
  }

  size_t GetInstrumentationUserCount() {
    ScopedObjectAccess soa(Thread::Current());
    return Runtime::Current()->GetInstrumentation()->requested_instrumentation_levels_.size();
  }

  void TestEvent(uint32_t instrumentation_event) {
    ScopedObjectAccess soa(Thread::Current());
    instrumentation::Instrumentation* instr = Runtime::Current()->GetInstrumentation();
    TestInstrumentationListener listener;
    {
      ScopedThreadSuspension sts(soa.Self(), kSuspended);
      ScopedSuspendAll ssa("Add instrumentation listener");
      instr->AddListener(&listener, instrumentation_event);
    }

    ArtMethod* const event_method = nullptr;
    mirror::Object* const event_obj = nullptr;
    const uint32_t event_dex_pc = 0;

    // Check the listener is registered and is notified of the event.
    EXPECT_TRUE(HasEventListener(instr, instrumentation_event));
    EXPECT_FALSE(DidListenerReceiveEvent(listener, instrumentation_event));
    ReportEvent(instr, instrumentation_event, soa.Self(), event_method, event_obj, event_dex_pc);
    EXPECT_TRUE(DidListenerReceiveEvent(listener, instrumentation_event));

    listener.Reset();
    {
      ScopedThreadSuspension sts(soa.Self(), kSuspended);
      ScopedSuspendAll ssa("Remove instrumentation listener");
      instr->RemoveListener(&listener, instrumentation_event);
    }

    // Check the listener is not registered and is not notified of the event.
    EXPECT_FALSE(HasEventListener(instr, instrumentation_event));
    EXPECT_FALSE(DidListenerReceiveEvent(listener, instrumentation_event));
    ReportEvent(instr, instrumentation_event, soa.Self(), event_method, event_obj, event_dex_pc);
    EXPECT_FALSE(DidListenerReceiveEvent(listener, instrumentation_event));
  }

  void DeoptimizeMethod(Thread* self, ArtMethod* method, bool enable_deoptimization)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Single method deoptimization");
    if (enable_deoptimization) {
      instrumentation->EnableDeoptimization();
    }
    instrumentation->Deoptimize(method);
  }

  void UndeoptimizeMethod(Thread* self, ArtMethod* method,
                          const char* key, bool disable_deoptimization)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Single method undeoptimization");
    instrumentation->Undeoptimize(method);
    if (disable_deoptimization) {
      instrumentation->DisableDeoptimization(key);
    }
  }

  void DeoptimizeEverything(Thread* self, const char* key, bool enable_deoptimization)
        SHARED_REQUIRES(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Full deoptimization");
    if (enable_deoptimization) {
      instrumentation->EnableDeoptimization();
    }
    instrumentation->DeoptimizeEverything(key);
  }

  void UndeoptimizeEverything(Thread* self, const char* key, bool disable_deoptimization)
        SHARED_REQUIRES(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("Full undeoptimization");
    instrumentation->UndeoptimizeEverything(key);
    if (disable_deoptimization) {
      instrumentation->DisableDeoptimization(key);
    }
  }

  void EnableMethodTracing(Thread* self, const char* key, bool needs_interpreter)
        SHARED_REQUIRES(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("EnableMethodTracing");
    instrumentation->EnableMethodTracing(key, needs_interpreter);
  }

  void DisableMethodTracing(Thread* self, const char* key)
        SHARED_REQUIRES(Locks::mutator_lock_) {
    Runtime* runtime = Runtime::Current();
    instrumentation::Instrumentation* instrumentation = runtime->GetInstrumentation();
    ScopedThreadSuspension sts(self, kSuspended);
    gc::ScopedGCCriticalSection gcs(self,
                                    gc::kGcCauseInstrumentation,
                                    gc::kCollectorTypeInstrumentation);
    ScopedSuspendAll ssa("EnableMethodTracing");
    instrumentation->DisableMethodTracing(key);
  }

 private:
  static bool HasEventListener(const instrumentation::Instrumentation* instr, uint32_t event_type)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    switch (event_type) {
      case instrumentation::Instrumentation::kMethodEntered:
        return instr->HasMethodEntryListeners();
      case instrumentation::Instrumentation::kMethodExited:
        return instr->HasMethodExitListeners();
      case instrumentation::Instrumentation::kMethodUnwind:
        return instr->HasMethodUnwindListeners();
      case instrumentation::Instrumentation::kDexPcMoved:
        return instr->HasDexPcListeners();
      case instrumentation::Instrumentation::kFieldRead:
        return instr->HasFieldReadListeners();
      case instrumentation::Instrumentation::kFieldWritten:
        return instr->HasFieldWriteListeners();
      case instrumentation::Instrumentation::kExceptionCaught:
        return instr->HasExceptionCaughtListeners();
      case instrumentation::Instrumentation::kBranch:
        return instr->HasBranchListeners();
      case instrumentation::Instrumentation::kInvokeVirtualOrInterface:
        return instr->HasInvokeVirtualOrInterfaceListeners();
      default:
        LOG(FATAL) << "Unknown instrumentation event " << event_type;
        UNREACHABLE();
    }
  }

  static void ReportEvent(const instrumentation::Instrumentation* instr, uint32_t event_type,
                          Thread* self, ArtMethod* method, mirror::Object* obj,
                          uint32_t dex_pc)
      SHARED_REQUIRES(Locks::mutator_lock_) {
    switch (event_type) {
      case instrumentation::Instrumentation::kMethodEntered:
        instr->MethodEnterEvent(self, obj, method, dex_pc);
        break;
      case instrumentation::Instrumentation::kMethodExited: {
        JValue value;
        instr->MethodExitEvent(self, obj, method, dex_pc, value);
        break;
      }
      case instrumentation::Instrumentation::kMethodUnwind:
        instr->MethodUnwindEvent(self, obj, method, dex_pc);
        break;
      case instrumentation::Instrumentation::kDexPcMoved:
        instr->DexPcMovedEvent(self, obj, method, dex_pc);
        break;
      case instrumentation::Instrumentation::kFieldRead:
        instr->FieldReadEvent(self, obj, method, dex_pc, nullptr);
        break;
      case instrumentation::Instrumentation::kFieldWritten: {
        JValue value;
        instr->FieldWriteEvent(self, obj, method, dex_pc, nullptr, value);
        break;
      }
      case instrumentation::Instrumentation::kExceptionCaught: {
        ThrowArithmeticExceptionDivideByZero();
        mirror::Throwable* event_exception = self->GetException();
        instr->ExceptionCaughtEvent(self, event_exception);
        self->ClearException();
        break;
      }
      case instrumentation::Instrumentation::kBranch:
        instr->Branch(self, method, dex_pc, -1);
        break;
      case instrumentation::Instrumentation::kInvokeVirtualOrInterface:
        instr->InvokeVirtualOrInterface(self, obj, method, dex_pc, method);
        break;
      default:
        LOG(FATAL) << "Unknown instrumentation event " << event_type;
        UNREACHABLE();
    }
  }

  static bool DidListenerReceiveEvent(const TestInstrumentationListener& listener,
                                      uint32_t event_type) {
    switch (event_type) {
      case instrumentation::Instrumentation::kMethodEntered:
        return listener.received_method_enter_event;
      case instrumentation::Instrumentation::kMethodExited:
        return listener.received_method_exit_event;
      case instrumentation::Instrumentation::kMethodUnwind:
        return listener.received_method_unwind_event;
      case instrumentation::Instrumentation::kDexPcMoved:
        return listener.received_dex_pc_moved_event;
      case instrumentation::Instrumentation::kFieldRead:
        return listener.received_field_read_event;
      case instrumentation::Instrumentation::kFieldWritten:
        return listener.received_field_written_event;
      case instrumentation::Instrumentation::kExceptionCaught:
        return listener.received_exception_caught_event;
      case instrumentation::Instrumentation::kBranch:
        return listener.received_branch_event;
      case instrumentation::Instrumentation::kInvokeVirtualOrInterface:
        return listener.received_invoke_virtual_or_interface_event;
      default:
        LOG(FATAL) << "Unknown instrumentation event " << event_type;
        UNREACHABLE();
    }
  }
};

TEST_F(InstrumentationTest, NoInstrumentation) {
  ScopedObjectAccess soa(Thread::Current());
  instrumentation::Instrumentation* instr = Runtime::Current()->GetInstrumentation();
  ASSERT_NE(instr, nullptr);

  EXPECT_FALSE(instr->AreExitStubsInstalled());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsActive());
  EXPECT_FALSE(instr->ShouldNotifyMethodEnterExitEvents());

  // Test interpreter table is the default one.
  EXPECT_EQ(instrumentation::kMainHandlerTable, instr->GetInterpreterHandlerTable());

  // Check there is no registered listener.
  EXPECT_FALSE(instr->HasDexPcListeners());
  EXPECT_FALSE(instr->HasExceptionCaughtListeners());
  EXPECT_FALSE(instr->HasFieldReadListeners());
  EXPECT_FALSE(instr->HasFieldWriteListeners());
  EXPECT_FALSE(instr->HasMethodEntryListeners());
  EXPECT_FALSE(instr->HasMethodExitListeners());
  EXPECT_FALSE(instr->IsActive());
}

// Test instrumentation listeners for each event.
TEST_F(InstrumentationTest, MethodEntryEvent) {
  TestEvent(instrumentation::Instrumentation::kMethodEntered);
}

TEST_F(InstrumentationTest, MethodExitEvent) {
  TestEvent(instrumentation::Instrumentation::kMethodExited);
}

TEST_F(InstrumentationTest, MethodUnwindEvent) {
  TestEvent(instrumentation::Instrumentation::kMethodUnwind);
}

TEST_F(InstrumentationTest, DexPcMovedEvent) {
  TestEvent(instrumentation::Instrumentation::kDexPcMoved);
}

TEST_F(InstrumentationTest, FieldReadEvent) {
  TestEvent(instrumentation::Instrumentation::kFieldRead);
}

TEST_F(InstrumentationTest, FieldWriteEvent) {
  TestEvent(instrumentation::Instrumentation::kFieldWritten);
}

TEST_F(InstrumentationTest, ExceptionCaughtEvent) {
  TestEvent(instrumentation::Instrumentation::kExceptionCaught);
}

TEST_F(InstrumentationTest, BranchEvent) {
  TestEvent(instrumentation::Instrumentation::kBranch);
}

TEST_F(InstrumentationTest, InvokeVirtualOrInterfaceEvent) {
  TestEvent(instrumentation::Instrumentation::kInvokeVirtualOrInterface);
}

TEST_F(InstrumentationTest, DeoptimizeDirectMethod) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader)));
  mirror::Class* klass = class_linker->FindClass(soa.Self(), "LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtMethod* method_to_deoptimize = klass->FindDeclaredDirectMethod("instanceMethod", "()V",
                                                                    sizeof(void*));
  ASSERT_TRUE(method_to_deoptimize != nullptr);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));

  DeoptimizeMethod(soa.Self(), method_to_deoptimize, true);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->AreExitStubsInstalled());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  constexpr const char* instrumentation_key = "DeoptimizeDirectMethod";
  UndeoptimizeMethod(soa.Self(), method_to_deoptimize, instrumentation_key, true);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));
}

TEST_F(InstrumentationTest, FullDeoptimization) {
  ScopedObjectAccess soa(Thread::Current());
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());

  constexpr const char* instrumentation_key = "FullDeoptimization";
  DeoptimizeEverything(soa.Self(), instrumentation_key, true);

  EXPECT_TRUE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->AreExitStubsInstalled());

  UndeoptimizeEverything(soa.Self(), instrumentation_key, true);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
}

TEST_F(InstrumentationTest, MixedDeoptimization) {
  ScopedObjectAccess soa(Thread::Current());
  jobject class_loader = LoadDex("Instrumentation");
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  ClassLinker* class_linker = runtime->GetClassLinker();
  StackHandleScope<1> hs(soa.Self());
  Handle<mirror::ClassLoader> loader(hs.NewHandle(soa.Decode<mirror::ClassLoader*>(class_loader)));
  mirror::Class* klass = class_linker->FindClass(soa.Self(), "LInstrumentation;", loader);
  ASSERT_TRUE(klass != nullptr);
  ArtMethod* method_to_deoptimize = klass->FindDeclaredDirectMethod("instanceMethod", "()V",
                                                                    sizeof(void*));
  ASSERT_TRUE(method_to_deoptimize != nullptr);

  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));

  DeoptimizeMethod(soa.Self(), method_to_deoptimize, true);
  // Deoptimizing a method does not change instrumentation level.
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->AreExitStubsInstalled());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  constexpr const char* instrumentation_key = "MixedDeoptimization";
  DeoptimizeEverything(soa.Self(), instrumentation_key, false);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter,
            GetCurrentInstrumentationLevel());
  EXPECT_TRUE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->AreExitStubsInstalled());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  UndeoptimizeEverything(soa.Self(), instrumentation_key, false);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->AreExitStubsInstalled());
  EXPECT_TRUE(instr->IsDeoptimized(method_to_deoptimize));

  UndeoptimizeMethod(soa.Self(), method_to_deoptimize, instrumentation_key, true);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_FALSE(instr->IsDeoptimized(method_to_deoptimize));
}

TEST_F(InstrumentationTest, MethodTracing_Interpreter) {
  ScopedObjectAccess soa(Thread::Current());
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());

  constexpr const char* instrumentation_key = "MethodTracing";
  EnableMethodTracing(soa.Self(), instrumentation_key, true);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter,
            GetCurrentInstrumentationLevel());
  EXPECT_TRUE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->AreExitStubsInstalled());

  DisableMethodTracing(soa.Self(), instrumentation_key);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
}

TEST_F(InstrumentationTest, MethodTracing_InstrumentationEntryExitStubs) {
  ScopedObjectAccess soa(Thread::Current());
  Runtime* const runtime = Runtime::Current();
  instrumentation::Instrumentation* instr = runtime->GetInstrumentation();
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());

  constexpr const char* instrumentation_key = "MethodTracing";
  EnableMethodTracing(soa.Self(), instrumentation_key, false);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
  EXPECT_TRUE(instr->AreExitStubsInstalled());

  DisableMethodTracing(soa.Self(), instrumentation_key);
  EXPECT_EQ(Instrumentation::InstrumentationLevel::kInstrumentNothing,
            GetCurrentInstrumentationLevel());
  EXPECT_FALSE(instr->AreAllMethodsDeoptimized());
}

// We use a macro to print the line number where the test is failing.
#define CHECK_INSTRUMENTATION(_level, _user_count)                                      \
  do {                                                                                  \
    Instrumentation* const instr = Runtime::Current()->GetInstrumentation();            \
    bool interpreter =                                                                  \
      (_level == Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);    \
    EXPECT_EQ(_level, GetCurrentInstrumentationLevel());                                \
    EXPECT_EQ(_user_count, GetInstrumentationUserCount());                              \
    if (instr->IsForcedInterpretOnly()) {                                               \
      EXPECT_TRUE(instr->InterpretOnly());                                              \
    } else if (interpreter) {                                                           \
      EXPECT_TRUE(instr->InterpretOnly());                                              \
    } else {                                                                            \
      EXPECT_FALSE(instr->InterpretOnly());                                             \
    }                                                                                   \
    if (interpreter) {                                                                  \
      EXPECT_TRUE(instr->AreAllMethodsDeoptimized());                                   \
    } else {                                                                            \
      EXPECT_FALSE(instr->AreAllMethodsDeoptimized());                                  \
    }                                                                                   \
  } while (false)

TEST_F(InstrumentationTest, ConfigureStubs_Nothing) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check no-op.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_InstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check we can switch to instrumentation stubs
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_Interpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check we can switch to interpreter
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_InstrumentationStubsToInterpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // Configure stubs with interpreter.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, ConfigureStubs_InterpreterToInstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with interpreter.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with instrumentation stubs.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest,
       ConfigureStubs_InstrumentationStubsToInterpreterToInstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // Configure stubs with interpreter.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with instrumentation stubs again.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // Check we can disable instrumentation.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_Nothing) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Check kInstrumentNothing with two clients.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_InstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // Configure stubs with instrumentation stubs for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs
  // instrumentation stubs.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_Interpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with interpreter for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with interpreter for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs interpreter.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_InstrumentationStubsThenInterpreter) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with instrumentation stubs for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // Configure stubs with interpreter for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs interpreter.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

TEST_F(InstrumentationTest, MultiConfigureStubs_InterpreterThenInstrumentationStubs) {
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);

  // Configure stubs with interpreter for 1st client.
  CheckConfigureStubs(kClientOneKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 1U);

  // Configure stubs with instrumentation stubs for 2nd client.
  CheckConfigureStubs(kClientTwoKey,
                      Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInterpreter, 2U);

  // 1st client requests instrumentation deactivation but 2nd client still needs
  // instrumentation stubs.
  CheckConfigureStubs(kClientOneKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentWithInstrumentationStubs,
                        1U);

  // 2nd client requests instrumentation deactivation
  CheckConfigureStubs(kClientTwoKey, Instrumentation::InstrumentationLevel::kInstrumentNothing);
  CHECK_INSTRUMENTATION(Instrumentation::InstrumentationLevel::kInstrumentNothing, 0U);
}

}  // namespace instrumentation
}  // namespace art
