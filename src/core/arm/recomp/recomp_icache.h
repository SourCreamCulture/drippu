// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace suyu::recomp {

// Instruction-cache invalidation for hybrid AOT plus JIT fallback.
// AOT translations cannot be rewritten, so any invalidate rejects further AOT
// selection and direct chains until this core is rebuilt. The JIT is notified
// separately by the backend so it can drop its own translations.
class RecompICache {
public:
    void InvalidateRange(std::uint64_t addr, std::size_t size) {
        (void)addr;
        (void)size;
        RejectAot();
    }

    void Clear() {
        RejectAot();
    }

    bool AllowsAot() const {
        return !aot_rejected_.load(std::memory_order_acquire);
    }

    bool AllowsAotChain() const {
        return AllowsAot();
    }

    bool NeedsJitForward() const {
        return jit_forward_.load(std::memory_order_acquire);
    }

private:
    void RejectAot() {
        aot_rejected_.store(true, std::memory_order_release);
        jit_forward_.store(true, std::memory_order_release);
    }

    std::atomic<bool> aot_rejected_{false};
    std::atomic<bool> jit_forward_{false};
};

} // namespace suyu::recomp
