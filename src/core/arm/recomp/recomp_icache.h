// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>

namespace suyu::recomp {

// Tracks instruction-cache invalidation for hybrid AOT plus JIT fallback.
// Matches ArmRecomp::ClearInstructionCache / InvalidateCacheRange today:
// both are empty, so AOT stays selected and the JIT is not notified.
class RecompICache {
public:
    void InvalidateRange(std::uint64_t addr, std::size_t size) {
        (void)addr;
        (void)size;
    }

    void Clear() {}

    bool AllowsAot() const {
        return true;
    }

    bool AllowsAotChain() const {
        return true;
    }

    bool NeedsJitForward() const {
        return false;
    }
};

} // namespace suyu::recomp
