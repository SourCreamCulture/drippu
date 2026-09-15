// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <utility>

namespace suyu::recomp {

// Process-scoped AOT registration and coverage. Module bases change on every
// boot (ASLR) and every title, so this state must not outlive the guest
// process, and cores that share one process must wait for one registration.
class RecompSession {
public:
    // Binds this session to `process`. Returns true when the bound process
    // changed, including the first bind.
    bool AttachProcess(const void* process) {
        std::lock_guard<std::mutex> lock{mu_};
        if (process_ == process) {
            return false;
        }
        process_ = process;
        return true;
    }

    void DetachProcess(const void* process) {
        std::lock_guard<std::mutex> lock{mu_};
        if (process_ == process) {
            process_ = nullptr;
        }
    }

    const void* process() const {
        std::lock_guard<std::mutex> lock{mu_};
        return process_;
    }

    // Runs `register_all` once for the current process. Other cores that
    // arrive at the same time wait until that call has finished.
    template <typename Fn>
    void EnsureModuleBasesRegistered(Fn&& register_all) {
        if (!bases_registered_) {
            bases_registered_ = true;
            std::forward<Fn>(register_all)();
        }
    }

    void NoteStaticBlock() {
        static_blocks_.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t static_blocks() const {
        return static_blocks_.load(std::memory_order_relaxed);
    }

private:
    mutable std::mutex mu_;
    const void* process_{nullptr};
    bool bases_registered_{false};
    std::atomic<std::uint64_t> static_blocks_{0};
};

} // namespace suyu::recomp
