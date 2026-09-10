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

// Per-phase cumulative compile time (nanoseconds), GetBlock breakdown.
inline std::atomic<uint64_t> translate_ns{0};  // guest decode -> IR build
inline std::atomic<uint64_t> optimize_ns{0};   // IR optimization passes
inline std::atomic<uint64_t> emit_ns{0};       // register allocation + x64 codegen

// Shared IR cache (EDEN_JIT_IRCACHE=1), see ir_cache.h.
inline std::atomic<uint64_t> ir_hits{0};           // entries served from cache
inline std::atomic<uint64_t> ir_stores{0};         // entries inserted
inline std::atomic<uint64_t> ir_hash_mismatch{0};  // lookups failed content check
inline std::atomic<uint64_t> ir_cache_entries{0};  // live entries
inline std::atomic<uint64_t> ir_cache_bytes{0};    // serialized bytes held

}  // namespace Dynarmic::Backend::X64::JitStats
