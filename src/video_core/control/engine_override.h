// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2014 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>

#include "common/common_types.h"

namespace Tegra::Engines {
class Maxwell3D;
}

namespace VideoCommon {

// (local-only) P2 parallel draw resolver: a thread with this set reads engine
// state from the draw snapshot instead of the live engine. Only the resolver
// thread (during the resolve phase) and the GPU thread (while committing a
// resolved draw) ever set it; every other thread leaves it null and observes
// the live engine.
inline thread_local Tegra::Engines::Maxwell3D* tls_engine_snapshot = nullptr;

// (local-only) P2 uniform epoch slots. The token pipeline delays a draw's
// tail (uploads) by one draw; uniforms uploaded there used to be memcpy'd
// straight from guest memory, so a game-CPU rewrite inside that window fed
// the draw wrong bytes (right-edge HUD bug). At resolve time every enabled
// uniform binding's bytes are copied into the job's epoch arena; while the
// tail runs, this pointer names that snapshot and the upload reads it
// instead of the guest. Null outside a token tail (serial path unchanged).
struct UniformEpochEntry {
    u64 device_addr; // DAddr of the binding content at resolve time
    u32 size;
    u32 offset;      // into the epoch byte arena
};

struct UniformEpochSnapshot {
    const UniformEpochEntry* entries{};
    size_t entry_count{};
    const u8* bytes{};
};

// (local-only) Fixed-slot backing store for the epoch capture. Each
// (device_addr, size) key owns one 2KB slot that persists across draws, so a
// recapture whose guest bytes still equal the slot content skips the copy
// (70.7% of consecutive recaptures measured content-identical, PROFILE
// 搂28.13). Slots are only written by captures, and with one job in flight
// the tail of job N always runs before the capture of job N+1 (CommitPending
//Draw precedes SnapshotAndEnqueue in every enabled mode), so a tail never
// observes a slot mid-rewrite. NOT safe under a concurrent resolve (async
// mode) without slot versioning -- entries reuse already carries that
// invariant, which is why async is disabled.
struct UniformEpochTable {
    static constexpr size_t kSlots = 64;
    static constexpr size_t kSlotBytes = 2048;

    std::array<u8, kSlots * kSlotBytes> bytes{};
    std::array<u64, kSlots> key_addr{};  // 0 = free (address 0 never maps)
    std::array<u32, kSlots> key_size{};
    u32 next_victim{};
    std::array<bool, kSlots> pinned{};

    // Entries in one snapshot must stay valid until its tail has consumed them.
    void BeginCapture() {
        pinned.fill(false);
    }
    bool short_circuit{true}; // EDEN_TOKEN_EPOCH_MEMCMP=0 forces full copies

    // (local-only) diagnostics (capture side)
    u64 diag_copies{};    // captures that copied into a slot
    u64 diag_skips{};     // captures skipped by identical-content memcmp
    u64 diag_overflows{}; // bindings over slot capacity -> classic path

    // Returns the slot for the key and whether its bytes already hold this
    // key's previous capture (a memcmp candidate); nullptr = caller falls
    // back to the tracked classic path. Probes from a hash of the address,
    // then linearly; inserts into the first free slot, else claims the
    // clock victim.
    u8* Acquire(u64 addr, u32 size, bool& content_valid) {
        content_valid = false;
        if (size == 0 || size > kSlotBytes) {
            ++diag_overflows;
            return nullptr;
        }
        size_t i{static_cast<size_t>((addr >> 8) & (kSlots - 1))};
        for (size_t probe{0}; probe < kSlots; ++probe, i = (i + 1) & (kSlots - 1)) {
            if (key_size[i] == 0) {
                break; // free slot: insert here
            }
            if (key_addr[i] == addr && key_size[i] == size) {
                pinned[i] = true;
                content_valid = true;
                return bytes.data() + i * kSlotBytes;
            }
        }
        if (key_size[i] != 0) {
            // Skip slots already referenced by this capture, including hits.
            size_t probe = 0;
            for (; probe < kSlots; ++probe) {
                i = next_victim;
                next_victim = (next_victim + 1) & (kSlots - 1);
                if (!pinned[i]) {
                    break;
                }
            }
            if (probe == kSlots) {
                ++diag_overflows;
                return nullptr;
            }
        }
        pinned[i] = true;
        key_addr[i] = addr;
        key_size[i] = size;
        content_valid = false;
        return bytes.data() + i * kSlotBytes;
    }
};

inline thread_local const UniformEpochSnapshot* tls_uniform_epoch = nullptr;

} // namespace VideoCommon
