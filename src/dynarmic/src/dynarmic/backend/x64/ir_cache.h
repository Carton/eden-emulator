// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Local profiling feature (see PROFILE_PROGRESS.md §11.6/§11.8): process-wide
// cache of post-Optimize IR blocks shared across the three per-core Jit
// instances. Measured motivation: 51% of block compiles are cross-core
// duplicates (same guest block compiled by 2-3 cores); this cache lets the
// second and third core skip Translate+Optimize and only run Emit. Entries
// are keyed by the IR location descriptor. Translation settings and the exact
// guest words must also match before reuse. Entries live only while at least
// one x64 A64 Jit exists. Enabled by default; EDEN_JIT_IRCACHE=0 disables it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opt_passes.h"

namespace Dynarmic::Backend::X64 {

class IRCache {
public:
    // Keep this in sync with Translate and Optimize's configuration inputs.
    struct Config {
        bool define_unpredictable_behaviour;
        bool wall_clock_cntpct;
        bool check_halt_on_memory_access;
        bool hook_data_cache_operations;
        bool get_set_elimination;
        bool constant_propagation;
        bool disable_verification;
        std::uint32_t dczid_el0;
        Optimization::PolyfillOptions polyfill;

        bool operator==(const Config&) const = default;
    };

    struct Entry {
        Config config;
        std::vector<std::uint32_t> code;
        std::vector<std::uint8_t> bytes;  // serialized IR::Block payload
        std::uint64_t start_pc;
        std::uint64_t end_pc;
        std::uint64_t content_hash;  // FNV-1a over translated guest words
    };
    using EntryPtr = std::shared_ptr<const Entry>;

    static bool Enabled();
    static void RegisterJit();
    static void UnregisterJit();

    // Returns the entry for this descriptor value, or nullptr. The caller
    // must still verify the configuration and exact current guest code.
    static EntryPtr Lookup(std::uint64_t descriptor_value);

    static void Store(std::uint64_t descriptor_value, const IR::Block& block,
                      std::uint64_t start_pc, std::uint64_t end_pc,
                      std::uint64_t content_hash, const Config& config,
                      std::vector<std::uint32_t> code);

    // Rebuilds `block` (already Reset with the matching descriptor) from the
    // serialized payload. Returns false on malformed data.
    static bool Load(const Entry& entry, IR::Block& block);

private:
    static std::vector<std::uint8_t> Serialize(const IR::Block& block);
};

}  // namespace Dynarmic::Backend::X64
