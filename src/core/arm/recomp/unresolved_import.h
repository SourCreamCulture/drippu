// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
//
// Shared unresolved-import policy for ArmRecomp and the exporter-smoke tests.
// Slot targeting and trap handling live here so the dispatcher cannot silently
// invent a different rule from the reloc writer.

#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace suyu::recomp {

using u32 = uint32_t;
using u64 = uint64_t;

constexpr u32 kA64MovX0Zero = 0xD2800000u;
constexpr u32 kA64Ret = 0xD65F03C0u;

// Recognizable PC the reloc writer stores in an unresolved GOT/IRELATIVE slot
// so the dispatcher can tell a missing import from a real guest address.
constexpr u64 kUnresolvedImportTrap = 0xFFFF'FFFF'0000'0000ULL;

enum class UnresolvedReloc : u32 {
    Abs64 = 0x101,
    GlobDat = 0x401,
    JumpSlot = 0x402,
    Irelative = 0x408,
};

struct UnresolvedImport {
    std::string name;
    u64 module_base = 0;
    u64 offset = 0;
    UnresolvedReloc kind = UnresolvedReloc::JumpSlot;
};

inline const char* UnresolvedRelocName(UnresolvedReloc kind) {
    switch (kind) {
    case UnresolvedReloc::Abs64:
        return "R_AARCH64_ABS64";
    case UnresolvedReloc::GlobDat:
        return "R_AARCH64_GLOB_DAT";
    case UnresolvedReloc::JumpSlot:
        return "R_AARCH64_JUMP_SLOT";
    case UnresolvedReloc::Irelative:
        return "R_AARCH64_IRELATIVE";
    }
    return "R_AARCH64_UNKNOWN";
}

// Scan module text for `mov x0, #0; ret`, else a bare `ret`. Current reloc
// writer prefers this guest VA over kUnresolvedImportTrap so Dynarmic can
// execute a real instruction. That turns a missing import into a silent return.
template <typename Read32>
inline u64 FindGuestReturnStub(u64 mod_base, Read32&& read32, u64 scan_limit = 0x100000) {
    u64 bare_ret = 0;
    for (u64 off = 0; off < scan_limit; off += 4) {
        const u32 insn = read32(mod_base + off);
        if (insn == kA64Ret) {
            if (!bare_ret) {
                bare_ret = mod_base + off;
            }
        } else if (insn == kA64MovX0Zero && read32(mod_base + off + 4) == kA64Ret) {
            return mod_base + off;
        }
    }
    return bare_ret;
}

// Address written into an unresolved or unsupported-IRELATIVE slot.
// trap_va is a FindGuestReturnStub result (0 if none).
inline u64 UnresolvedSlotTarget(u64 trap_va) {
    return trap_va ? trap_va : kUnresolvedImportTrap;
}

inline bool IsUnresolvedImportTrap(u64 pc) {
    return pc == kUnresolvedImportTrap;
}

enum class UnresolvedTrapAction {
    FakeReturnZero,
    Halt,
};

struct UnresolvedTrapResult {
    UnresolvedTrapAction action = UnresolvedTrapAction::FakeReturnZero;
    u64 x0 = 0;
    u64 pc = 0;
    std::string diagnostic;
};

inline std::string FormatUnresolvedImportDiagnostic(
    std::string_view name, u64 module_base, u64 offset, UnresolvedReloc kind) {
    std::string out = "recomp: unresolved import ";
    out += UnresolvedRelocName(kind);
    out += " '";
    out += name.empty() ? "<no name>" : std::string(name);
    out += "' module_base=0x";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(module_base));
    out += buf;
    out += " offset=0x";
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(offset));
    out += buf;
    return out;
}

inline std::string FormatUnresolvedTrapDiagnostic(u64 lr,
                                                 const std::vector<UnresolvedImport>& recorded) {
    std::string out = "recomp: called through unresolved import (lr=0x";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(lr));
    out += buf;
    out += ")";
    if (recorded.empty()) {
        out += " no reloc records";
        return out;
    }
    out += " pending:";
    const size_t n = recorded.size() < 8 ? recorded.size() : 8;
    for (size_t i = 0; i < n; ++i) {
        out += " [";
        out += FormatUnresolvedImportDiagnostic(recorded[i].name, recorded[i].module_base,
                                                recorded[i].offset, recorded[i].kind);
        out += "]";
    }
    if (recorded.size() > n) {
        out += " ...";
    }
    return out;
}

// Current dispatcher. Treats the sentinel as a successful empty function.
inline UnresolvedTrapResult TakeUnresolvedImportTrap(u64 x0, u64 lr,
                                                     const std::vector<UnresolvedImport>& recorded) {
    (void)x0;
    UnresolvedTrapResult r;
    r.action = UnresolvedTrapAction::FakeReturnZero;
    r.x0 = 0;
    r.pc = lr;
    r.diagnostic = FormatUnresolvedTrapDiagnostic(lr, recorded);
    return r;
}

} // namespace suyu::recomp
