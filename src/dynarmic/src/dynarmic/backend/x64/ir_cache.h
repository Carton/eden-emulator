// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Local profiling feature (see PROFILE_PROGRESS.md §11.6/§11.8): process-wide
// cache of post-Optimize IR blocks shared across the three per-core Jit
// instances. Measured motivation: 51% of block compiles are cross-core
// duplicates (same guest block compiled by 2-3 cores); this cache lets the
// second and third core skip Translate+Optimize and only run Emit. Entries
// are keyed by the IR location descriptor and validated by an FNV-1a hash of
// the guest code words that were actually fed to the translator, so any
// guest code modification (self-modifying code / module reload) naturally
// turns hits into misses without explicit invalidation.
// Enabled via EDEN_JIT_IRCACHE=1.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "dynarmic/ir/basic_block.h"

namespace Dynarmic::Backend::X64 {

class IRCache {
public:
    struct Entry {
        std::vector<std::uint8_t> bytes;  // serialized IR::Block payload
        std::uint64_t start_pc;
        std::uint64_t end_pc;
        std::uint64_t content_hash;  // FNV-1a over translated guest words
    };
    using EntryPtr = std::shared_ptr<const Entry>;

    static bool Enabled();

    // Returns the entry for this descriptor value, or nullptr. The caller
    // must still verify entry->content_hash against current guest code.
    static EntryPtr Lookup(std::uint64_t descriptor_value);

    static void Store(std::uint64_t descriptor_value, const IR::Block& block,
                      std::uint64_t start_pc, std::uint64_t end_pc,
                      std::uint64_t content_hash);

    // Rebuilds `block` (already Reset with the matching descriptor) from the
    // serialized payload. Returns false on malformed data.
    static bool Load(const Entry& entry, IR::Block& block);

private:
    static std::vector<std::uint8_t> Serialize(const IR::Block& block);
};

}  // namespace Dynarmic::Backend::X64
