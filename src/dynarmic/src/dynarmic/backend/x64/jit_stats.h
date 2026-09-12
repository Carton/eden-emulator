// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 MerryMage
// SPDX-License-Identifier: 0BSD

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>

// Lightweight global JIT activity counters (local profiling instrumentation).
// Incremented with relaxed ordering from hot paths; read at shutdown.
namespace Dynarmic::Backend::X64::JitStats {

// Set before process startup for an instrumentation-off A/B run. Live cache
// size gauges and the independently enabled block dump are not disabled.
inline const bool enabled = [] {
    const char* value = std::getenv("EDEN_JIT_STATS");
    if (value) {
        return value[0] != '0' || value[1] != '\0';
    }
    // Local profiling is opt-in outside the measured Windows environment.
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}();

inline bool Enabled() {
    return enabled;
}

inline void Count(std::atomic<uint64_t>& counter, uint64_t amount = 1) {
    if (Enabled()) {
        counter.fetch_add(amount, std::memory_order_relaxed);
    }
}

inline std::chrono::steady_clock::time_point Now() {
    return Enabled() ? std::chrono::steady_clock::now()
                     : std::chrono::steady_clock::time_point{};
}

inline std::atomic<uint64_t> block_lookups{0};   // dispatcher tiers missed, C++ lookup entered
inline std::atomic<uint64_t> block_compiles{0};  // emitted blocks, including IR cache hits
inline std::atomic<uint64_t> range_invalidations{0};  // guest icache/munmap range invalidations
inline std::atomic<uint64_t> full_clears{0};     // entire code cache rebuilds
inline std::atomic<uint64_t> fastmem_faults{0};  // fastmem SEH fault fallbacks

// Per-phase cumulative compile time (nanoseconds), GetBlock breakdown.
inline std::atomic<uint64_t> translate_ns{0};  // guest decode -> IR build
inline std::atomic<uint64_t> optimize_ns{0};   // IR optimization passes
inline std::atomic<uint64_t> emit_ns{0};       // register allocation + x64 codegen
inline std::atomic<uint64_t> compile_ns{0};    // full GetBlock miss, including cache overhead

// Shared IR cache (EDEN_JIT_IRCACHE=1), see ir_cache.h.
inline std::atomic<uint64_t> ir_hits{0};           // entries served from cache
inline std::atomic<uint64_t> ir_stores{0};         // entries inserted or replaced
inline std::atomic<uint64_t> ir_hash_mismatch{0};  // lookups failed content check
inline std::atomic<uint64_t> ir_cache_entries{0};  // live entries
inline std::atomic<uint64_t> ir_cache_bytes{0};    // entry and vector allocations (not total RSS)

// Static guest instruction mix (counted at translate time; steady-state compiles
// are near-zero so these converge to the executed-code distribution).
inline std::atomic<uint64_t> inst_total{0};
inline std::atomic<uint64_t> inst_bl{0};       // BL imm
inline std::atomic<uint64_t> inst_blr{0};      // BLR (indirect call)
inline std::atomic<uint64_t> inst_br{0};       // BR (indirect jump / tail call)
inline std::atomic<uint64_t> inst_ret{0};      // RET
inline std::atomic<uint64_t> inst_bcond{0};    // B.cond
inline std::atomic<uint64_t> inst_buncond{0};  // B imm
inline std::atomic<uint64_t> inst_cbz{0};      // CBZ/CBNZ
inline std::atomic<uint64_t> inst_tbz{0};      // TBZ/TBNZ

}  // namespace Dynarmic::Backend::X64::JitStats
