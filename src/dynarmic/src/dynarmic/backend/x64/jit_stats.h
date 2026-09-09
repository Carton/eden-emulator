// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2026 MerryMage
// SPDX-License-Identifier: 0BSD

#pragma once

#include <atomic>
#include <cstdint>

// Lightweight global JIT activity counters (local profiling instrumentation).
// Incremented with relaxed ordering from hot paths; read at shutdown.
namespace Dynarmic::Backend::X64::JitStats {

inline std::atomic<uint64_t> block_lookups{0};   // dispatcher tiers missed, C++ lookup entered
inline std::atomic<uint64_t> block_compiles{0};  // blocks translated + emitted
inline std::atomic<uint64_t> range_invalidations{0};  // guest icache/munmap range invalidations
inline std::atomic<uint64_t> full_clears{0};     // entire code cache rebuilds
inline std::atomic<uint64_t> fastmem_faults{0};  // fastmem SEH fault fallbacks

}  // namespace Dynarmic::Backend::X64::JitStats
