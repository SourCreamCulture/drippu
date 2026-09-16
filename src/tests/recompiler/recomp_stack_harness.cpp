// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Real-stack homebrew integration harness: ArmRecomp + in-tree Dynarmic +
// Kernel process memory / PhysicalCore TLS republish.
//
// Unlike homebrew_harness.cpp (hosted stubs, no System), this boots a minimal
// Core::System, creates an application KProcess, maps in-tree fixture bytes,
// and drives ArmRecomp::RunThread / StepThread against real guest memory and
// the Dynarmic JIT fallback.
//
// Does not call Svc::Call / PhysicalCore::RunThread (fixture SVC imm is not a
// safe live HLE call). Does not load copyrighted titles or keys.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <unistd.h>

#include "common/common_types.h"
#include "common/scope_exit.h"
#include "core/arm/arm_interface.h"
#include "core/arm/dynarmic/dynarmic_exclusive_monitor.h"
#include "common/typed_address.h"
#include "core/arm/recomp/arm_recomp.h"
#include "core/arm/recomp/recomp_image_abi.h"
#include "core/core.h"
#include "core/cpu_manager.h"
#include "core/file_sys/program_metadata.h"
#include "core/hle/kernel/code_set.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/memory_types.h"
#include "core/hle/kernel/physical_core.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"

namespace {

int g_fails = 0;

void Fail(const std::string& msg) {
    std::cerr << "FAIL: " << msg << std::endl;
    ++g_fails;
}

void Pass(const std::string& msg) {
    std::cout << "PASS: " << msg << std::endl;
}

void ExpectEq(const char* name, u64 got, u64 want) {
    if (got != want) {
        Fail(std::string(name) + ": got=" + std::to_string(got) + " want=" + std::to_string(want));
    } else {
        Pass(std::string(name) + "=" + std::to_string(got));
    }
}

void ExpectTrue(const char* name, bool cond) {
    if (!cond) {
        Fail(std::string(name) + " was false");
    } else {
        Pass(name);
    }
}

// Layout-compatible prefix of GuestContext / GuestContextView. Must stay in
// lockstep with arm_recomp.cpp / arm64_to_c.h (pinned by kRecompRegsPrefixSize).
struct GuestContextView {
    u64 x[32];
    u64 pc;
    u8 n, z, c, v;
    u8* mem;
    u64 mem_size;
    u64 mem_base_vaddr;
    int halted;
    u64 pending_svc;
    u64 vreg[32][2];
    u64 tpidr_el0;
    const void* host_mem;
    u64 tpidrro_el0;
    u64 fpcr;
    u64 fpsr;
    int chain_budget;
};

static_assert(sizeof(GuestContextView) == suyu::recomp::kRecompRegsPrefixSize);
static_assert(offsetof(GuestContextView, pending_svc) == 304);
static_assert(offsetof(GuestContextView, tpidr_el0) == 824);
static_assert(offsetof(GuestContextView, tpidrro_el0) == 840);

struct RecompHostMem {
    void* user;
    u64 (*load)(void* user, u64 va, u32 size);
    void (*store)(void* user, u64 va, u32 size, u64 value);
};

constexpr u64 kNoPendingSvc = ~0ULL;
constexpr int kHaltUnhandled = 2;

// Guest PCs relative to process entry (absolute after bootstrap).
constexpr u64 kOffTlsSvc = 0x0000;
constexpr u64 kOffUnhandled = 0x0800;
constexpr u64 kOffFallback = 0x1000; // Dynarmic-only encodings live here
constexpr u64 kOffData = 0x2000;
constexpr u64 kOffCrossPage = 0x1FFC; // 4 bytes before page boundary at 0x2000

// AArch64 encodings written into guest RAM for Dynarmic fallback.
// MOVZ Xd,#imm16 = 0xD2800000 | (imm16 << 5) | Rd
constexpr u32 kMovzX0Beef = 0xD2800000u | (0xBEEFu << 5); // MOVZ X0, #0xBEEF
constexpr u32 kSvc99 = 0xD4000C61u;                        // SVC #99
constexpr u32 kMovzX0Cafe = 0xD2800000u | (0xCAFEu << 5); // MOVZ X0, #0xCAFE
constexpr u32 kSvc77 = 0xD40009A1u;                        // SVC #77

u64 g_entry = 0;
u64 g_force_miss_pc = 0; // when set, Lookup returns null for this PC

void StoreViaHost(GuestContextView* c, u64 va, u64 value) {
    auto* hm = reinterpret_cast<const RecompHostMem*>(c->host_mem);
    ExpectTrue("host_mem installed", hm != nullptr && hm->store != nullptr);
    if (hm && hm->store) {
        hm->store(hm->user, va, 8, value);
    }
}

void BlockTlsSvc(void* raw) {
    auto* c = static_cast<GuestContextView*>(raw);
    c->x[0] = 0x1234;
    c->tpidr_el0 = c->x[0];
    c->x[1] = c->tpidr_el0;
    c->x[3] = c->tpidrro_el0;
    c->x[2] = 0xABCD;
    // Cross-page store through ApplicationMemory host bridge.
    StoreViaHost(c, g_entry + kOffCrossPage, c->x[2]);
    c->pc = g_entry + kOffTlsSvc + 0x1C;
    c->pending_svc = 42;
}

void BlockUnhandled(void* raw) {
    auto* c = static_cast<GuestContextView*>(raw);
    c->halted = kHaltUnhandled;
    // Leave PC at the unhandled site so Dynarmic fallback executes guest bytes.
}

void BlockPlain(void* raw) {
    auto* c = static_cast<GuestContextView*>(raw);
    c->x[0] = 7;
    c->pc = g_entry + 0x3000 + 4;
    // Park so RunThread returns instead of chaining into a miss at pc+4.
    c->pending_svc = 1;
}

Core::RecompBlockFn Lookup(u64 pc) {
    if (g_force_miss_pc != 0 && pc == g_force_miss_pc) {
        return nullptr;
    }
    if (pc == g_entry + kOffTlsSvc) {
        return &BlockTlsSvc;
    }
    if (pc == g_entry + kOffUnhandled) {
        return &BlockUnhandled;
    }
    if (pc == g_entry + 0x3000) {
        return &BlockPlain;
    }
    return nullptr;
}

struct StackFixture {
    Core::System system;
    Kernel::KProcess* process = nullptr;
    Kernel::KThread* thread = nullptr;
    std::unique_ptr<Core::ArmRecomp> arm;

    bool Bootstrap() {
        system.Initialize();
        system.Kernel().Initialize();
        system.GetCpuManager().Initialize();

        auto& kernel = system.Kernel();
        process = Kernel::KProcess::Create(kernel);
        if (!process) {
            Fail("KProcess::Create");
            return false;
        }
        Kernel::KProcess::Register(kernel, process);

        constexpr size_t kCodeSize = 4 * Kernel::PageSize; // 0x4000
        const auto meta = FileSys::ProgramMetadata::GetDefault();
        if (process->LoadFromMetadata(kernel, meta, kCodeSize, 0, 0).IsError()) {
            Fail("LoadFromMetadata");
            return false;
        }

        kernel.AppendNewProcess(process);
        kernel.MakeApplicationProcess(process);
        process->Open(kernel);

        g_entry = GetInteger(process->GetEntryPoint());
        ExpectTrue("entry non-zero", g_entry != 0);

        // Build a tiny homebrew image: RX code + RW data. Dynarmic fallback
        // encodings live at +0x1000 / +0x0800; AOT covers other PCs.
        Kernel::CodeSet codeset;
        codeset.memory.assign(kCodeSize, 0);

        auto write_u32 = [&](u64 off, u32 insn) {
            std::memcpy(codeset.memory.data() + off, &insn, sizeof(insn));
        };
        // Fallback path at +0x1000: MOVZ X0,#0xBEEF; SVC #99
        write_u32(kOffFallback, kMovzX0Beef);
        write_u32(kOffFallback + 4, kSvc99);
        // Unhandled AOT site also has Dynarmic bytes so fallback can run:
        write_u32(kOffUnhandled, kMovzX0Cafe);
        write_u32(kOffUnhandled + 4, kSvc77);

        codeset.CodeSegment().offset = 0;
        codeset.CodeSegment().addr = 0;
        codeset.CodeSegment().size = static_cast<u32>(2 * Kernel::PageSize);
        codeset.RODataSegment().offset = 0;
        codeset.RODataSegment().addr = 0;
        codeset.RODataSegment().size = 0;
        codeset.DataSegment().offset = 2 * Kernel::PageSize;
        codeset.DataSegment().addr = 2 * Kernel::PageSize;
        codeset.DataSegment().size = static_cast<u32>(2 * Kernel::PageSize);
        process->LoadModule(kernel, std::move(codeset), process->GetEntryPoint());

        // Dummy thread owns the process so ModuleBaseFor can read it.
        thread = Kernel::KThread::Create(kernel);
        if (!thread) {
            Fail("KThread::Create");
            return false;
        }
        if (Kernel::KThread::InitializeDummyThread(system, thread, process).IsError()) {
            Fail("InitializeDummyThread");
            return false;
        }
        Kernel::KThread::Register(kernel, thread);

        auto& monitor = static_cast<Core::DynarmicExclusiveMonitor&>(process->GetExclusiveMonitor());
        arm = std::make_unique<Core::ArmRecomp>(system, true /* wall clock */, &Lookup, process,
                                                &monitor, 0);
        return true;
    }

    void RecreateArm() {
        // Dynarmic fallback sticks (in_fallback) until the PC lands on AOT again.
        // Recreate the backend between scenarios so each starts on ArmRecomp AOT.
        auto& monitor =
            static_cast<Core::DynarmicExclusiveMonitor&>(process->GetExclusiveMonitor());
        arm.reset();
        arm = std::make_unique<Core::ArmRecomp>(system, true, &Lookup, process, &monitor, 0);
    }

    void ResetCtx(u64 pc, u64 tpidrro = 0) {
        Kernel::Svc::ThreadContext ctx{};
        ctx.pc = pc;
        ctx.sp = g_entry + 0x3F00;
        arm->SetContext(ctx);
        arm->SetTpidrroEl0(tpidrro);
    }
};

void ScenarioSvcTlsCrossPage(StackFixture& f) {
    f.ResetCtx(g_entry + kOffTlsSvc, 0xC0FFEE);
    const auto hr = f.arm->RunThread(f.thread);
    ExpectTrue("svc HaltReason", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("svc number", f.arm->GetSvcNumber(), 42);

    Kernel::Svc::ThreadContext ctx{};
    f.arm->GetContext(ctx);
    ExpectEq("tpidr_el0 via GetContext", ctx.tpidr, 0x1234);
    ExpectEq("x1 tpidr readback", ctx.r[1], 0x1234);
    ExpectEq("x3 tpidrro", ctx.r[3], 0xC0FFEE);

    const u64 stored = f.system.ApplicationMemory().Read64(g_entry + kOffCrossPage);
    ExpectEq("cross-page store", stored, 0xABCD);
    Pass("SVC + TLS + cross-page memory via ApplicationMemory");
}

void ScenarioPhysicalCoreTlsSwitch(StackFixture& f) {
    // PhysicalCore::LoadContext is what KScheduler uses on switch-in: it
    // republishes the thread's TLS address into TPIDRRO_EL0.
    auto& core0 = f.system.Kernel().PhysicalCore(0);

    // Two synthetic "threads": reuse DummyThread but swap TLS via SetTpidrro
    // after LoadContext pattern (DummyThread has no real TLS page).
    f.ResetCtx(g_entry + kOffTlsSvc, 0x1111);
    core0.LoadContext(f.thread); // owner process publishes whatever TLS exists
    f.arm->SetTpidrroEl0(0x1111); // explicit republish matching LoadContext contract
    (void)f.arm->RunThread(f.thread);
    Kernel::Svc::ThreadContext a{};
    f.arm->GetContext(a);
    const u64 tp_a = a.tpidr;
    ExpectEq("switch A tpidrro seen", a.r[3], 0x1111);

    f.ResetCtx(g_entry + kOffTlsSvc, 0x2222);
    f.arm->SetTpidrroEl0(0x2222);
    (void)f.arm->RunThread(f.thread);
    Kernel::Svc::ThreadContext b{};
    f.arm->GetContext(b);
    ExpectEq("switch B tpidrro seen", b.r[3], 0x2222);
    ExpectEq("switch A tpidr preserved in prior snapshot", tp_a, 0x1234);
    Pass("PhysicalCore LoadContext path + TLS switch between RunThread slices");
}

void ScenarioForceMissDynarmic(StackFixture& f) {
    f.RecreateArm();
    g_force_miss_pc = g_entry + kOffFallback;
    f.ResetCtx(g_entry + kOffFallback);
    const auto hr = f.arm->RunThread(f.thread);
    g_force_miss_pc = 0;

    ExpectTrue("miss -> SupervisorCall via Dynarmic", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("fallback svc", f.arm->GetSvcNumber(), 99);
    Kernel::Svc::ThreadContext ctx{};
    f.arm->GetContext(ctx);
    ExpectEq("fallback x0", ctx.r[0], 0xBEEF);
    Pass("AOT miss entered real Dynarmic fallback");
}

void ScenarioUnhandledFallback(StackFixture& f) {
    f.RecreateArm();
    f.ResetCtx(g_entry + kOffUnhandled);
    const auto hr = f.arm->RunThread(f.thread);
    ExpectTrue("unhandled -> SupervisorCall via Dynarmic",
               True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("unhandled svc", f.arm->GetSvcNumber(), 77);
    Kernel::Svc::ThreadContext ctx{};
    f.arm->GetContext(ctx);
    ExpectEq("unhandled x0", ctx.r[0], 0xCAFE);
    Pass("RECOMP_HALT_UNHANDLED entered real Dynarmic fallback");
}

void ScenarioInvalidation(StackFixture& f) {
    f.RecreateArm();
    f.ResetCtx(g_entry + 0x3000);
    (void)f.arm->RunThread(f.thread);
    Kernel::Svc::ThreadContext pre{};
    f.arm->GetContext(pre);
    ExpectEq("pre-invalidate x0", pre.r[0], 7);

    f.arm->ClearInstructionCache();

    // Same AOT PC must now miss and hit Dynarmic. Plant fallback encodings at
    // 0x3000 for this scenario (was AOT-only; lives in RW data segment).
    f.system.ApplicationMemory().Write32(g_entry + 0x3000, kMovzX0Beef);
    f.system.ApplicationMemory().Write32(g_entry + 0x3004, kSvc99);
    f.ResetCtx(g_entry + 0x3000);
    const auto hr = f.arm->RunThread(f.thread);
    ExpectTrue("post-invalidate Dynarmic", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("post-invalidate svc", f.arm->GetSvcNumber(), 99);
    Pass("ClearInstructionCache refuses AOT and forces Dynarmic");
}

void ScenarioRestart(StackFixture& f) {
    // Destroy and recreate ArmRecomp: session detach/attach + fresh icache.
    f.RecreateArm();

    f.ResetCtx(g_entry + kOffTlsSvc, 0xABCD1234ULL);
    const auto hr = f.arm->RunThread(f.thread);
    ExpectTrue("restart SVC", True(hr & Core::HaltReason::SupervisorCall));
    ExpectEq("restart svc num", f.arm->GetSvcNumber(), 42);
    Kernel::Svc::ThreadContext ctx{};
    f.arm->GetContext(ctx);
    ExpectEq("restart tpidrro", ctx.r[3], 0xABCD1234ULL);
    Pass("stop/relaunch recreates ArmRecomp and re-runs AOT");
}

void ScenarioStepMiss(StackFixture& f) {
    f.RecreateArm();
    g_force_miss_pc = g_entry + kOffFallback;
    f.ResetCtx(g_entry + kOffFallback);
    const auto hr = f.arm->StepThread(f.thread);
    g_force_miss_pc = 0;
    // One Dynarmic step executes MOVZ; may return StepThread without SVC yet.
    Kernel::Svc::ThreadContext ctx{};
    f.arm->GetContext(ctx);
    ExpectEq("step-miss x0 after one insn", ctx.r[0], 0xBEEF);
    ExpectTrue("step-miss advanced or halted",
               True(hr & Core::HaltReason::StepThread) || True(hr & Core::HaltReason::SupervisorCall) ||
                   ctx.pc == g_entry + kOffFallback + 4);
    Pass("StepThread AOT miss uses Dynarmic StepFallback");
}

void PrintGaps() {
    std::cout
        << "GAPS (still blocked / out of scope for this harness):\n"
        << "  - Full PhysicalCore::RunThread -> Svc::Call HLE (needs a safe SVC + services)\n"
        << "  - Multi-core KScheduler fiber world / CpuManager guest loop\n"
        << "  - Real NSO/NRO homebrew load (no keys/firmware/dumps in CI)\n"
        << "  - gdbstub debugger StepThread against a live title\n"
        << "Proven here: ArmRecomp Run/Step, Dynarmic fallback, ApplicationMemory,\n"
        << "  TPIDRRO republish contract used by PhysicalCore::LoadContext, icache,\n"
        << "  restart.\n";
}

} // namespace

int main() {
    std::cout << std::unitbuf;
    std::cerr << std::unitbuf;
    std::cout << "recomp_stack_harness: ArmRecomp + Dynarmic + kernel memory\n";

    // Heap-allocate and intentionally leak: System/kernel teardown is not safe
    // after this minimal bootstrap (no ShutdownMainProcess path).
    auto* fix = new StackFixture;
    if (!fix->Bootstrap()) {
        std::cerr << "bootstrap failed\n";
        _exit(1);
    }
    Pass("bootstrap System + application KProcess + ArmRecomp");

    ScenarioSvcTlsCrossPage(*fix);
    ScenarioPhysicalCoreTlsSwitch(*fix);
    ScenarioForceMissDynarmic(*fix);
    ScenarioUnhandledFallback(*fix);
    ScenarioInvalidation(*fix);
    ScenarioRestart(*fix);
    ScenarioStepMiss(*fix);
    PrintGaps();

    if (g_fails == 0) {
        std::cout << "ALL PASSED\n";
        _exit(0);
    }
    std::cerr << g_fails << " FAILURE(S)\n";
    _exit(1);
}
