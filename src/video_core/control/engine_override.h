// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-FileCopyrightText: Copyright 2014 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

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

inline thread_local const UniformEpochSnapshot* tls_uniform_epoch = nullptr;

} // namespace VideoCommon
