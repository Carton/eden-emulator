// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <memory>

#include "video_core/control/engine_override.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Tegra {
class MemoryManager;
}

namespace Vulkan {

// (local-only) P2 Step 2: draw tokens.
//
// The GPU thread snapshots a draw into the shadow engine and hands the
// resolve phase (binding lookups) to the VulkanWorker through a scheduler
// command, or runs it inline for validation. The commit phase (uploads +
// scheduler records) always runs on the GPU thread, in draw order, at the
// next rasterizer rendezvous. Exactly one job is in flight at any time.
//
// Snapshot modes:
//  - FullCopy: copy regs/state every draw (depth-1 behaviour, known-good).
//  - Journal:  maintain the shadow incrementally by replaying the engine's
//              register-write journal; full copy only on (re)sync events
//              (first use, channel switch, journal overflow).
class DrawResolver {
public:
    enum class SnapshotMode : u8 {
        FullCopy,
        Journal,
    };

    struct Job {
        GraphicsPipeline* pipeline{};
        bool is_indexed{};
        u32 instance_count{};
        DrawContext ctx;

        // (local-only) uniform epoch: guest bytes of the enabled uniform
        // bindings captured at resolve time; the delayed tail upload reads
        // them instead of guest memory that the game CPU may have rewritten
        // inside the tail-delay window (right-edge HUD bug). Contents live
        // in the resolver's persistent UniformEpochTable (fixed slots, so a
        // recapture with unchanged bytes skips the copy); the job only
        // carries the per-draw entry list pointing into it.
        static constexpr size_t EpochEntries = 16;
        std::array<VideoCommon::UniformEpochEntry, EpochEntries> epoch_entries{};
        size_t epoch_entry_count{};
        VideoCommon::UniformEpochSnapshot epoch_snapshot{};
        bool epoch_valid{}; // capture succeeded; the tail installs the snapshot
    };

    DrawResolver(Tegra::MemoryManager& gpu_memory_, BufferCache& buffer_cache_,
                 TextureCache& texture_cache_);
    ~DrawResolver();

    DrawResolver(const DrawResolver&) = delete;
    DrawResolver& operator=(const DrawResolver&) = delete;

    bool ResolveInFlight() const {
        return job_phase.load(std::memory_order_relaxed) != Phase::Idle;
    }

    // GPU thread: block until the current job finished resolving (returns
    // immediately when none is running).
    void WaitResolved();

    // GPU thread: snapshot the draw into the shadow engine and enqueue it.
    // Returns false (doing nothing) when the draw must run synchronously.
    bool SnapshotAndEnqueue(Tegra::Engines::Maxwell3D& engine, GraphicsPipeline* pipeline,
                            bool is_indexed, u32 instance_count);

    // Execute the resolve phase. Runs on the VulkanWorker (token mode) or
    // inline on the GPU thread (validation mode); the caller guarantees the
    // job was enqueued and is not concurrently executed.
    void ExecuteResolve();

    // GPU thread: after WaitResolved(), the finished job.
    Job& TakeJob() {
        return job;
    }

    Tegra::Engines::Maxwell3D& SnapshotEngine() const {
        return *shadow;
    }

    // GPU thread: merge snapshot dirty flags back into the live engine and
    // release the job slot.
    void FinishJob(Tegra::Engines::Maxwell3D& engine);

    // GPU thread, debug: verify the shadow converged to the live registers.
    // Applies the not-yet-consumed journal entries to a scratch copy first,
    // so writes parsed after the snapshot do not count as mismatches.
    // Returns false and logs on the first divergent register.
    bool VerifySnapshot(const Tegra::Engines::Maxwell3D& engine);

    SnapshotMode snapshot_mode{SnapshotMode::Journal};
    bool check_enabled{false};
    // (local-only) EDEN_TOKEN_EPOCH=0 disables the uniform epoch capture
    // (A/B switch; the tail then keeps the old direct-guest fast path).
    bool epoch_enabled{true};
    // (local-only) persistent fixed-slot backing store for the epoch capture.
    // short_circuit=false (EDEN_TOKEN_EPOCH_MEMCMP=0) forces a full copy on
    // every recapture -- the A/B arm isolating the memcmp skip's value.
    VideoCommon::UniformEpochTable epoch_table{};

    // (local-only) P2 diagnostics (aggregate since startup)
    u64 diag_kicks{};
    u64 diag_fallbacks{};      // set by the rasterizer on sync fallback
    u64 diag_resyncs{};        // journal-mode full copies
    u64 diag_journal_entries{};// total journal entries replayed
    u64 diag_resolve_calls{};
    u64 diag_snapshot_mismatches{};
    std::chrono::nanoseconds diag_resolve_ns{};
    std::chrono::nanoseconds diag_replay_ns{};

private:
    enum class Phase : u32 {
        Idle,
        Resolving,
        Resolved,
    };

    void CopyDynamicState(Tegra::Engines::Maxwell3D& engine);

    Tegra::MemoryManager& gpu_memory;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    std::unique_ptr<Tegra::Engines::Maxwell3D> shadow;

    Job job;
    std::atomic<Phase> job_phase{Phase::Idle};
    Tegra::Engines::Maxwell3D::DirtyState::Flags dirty_snapshot{};

    // Journal mode: entries copied from the engine at snapshot time, replayed
    // into the shadow at execute time.
    Tegra::Engines::Maxwell3D::JournalEntry
        slot_journal[Tegra::Engines::Maxwell3D::JournalCapacity];
    size_t slot_journal_size{};
    // Engine instance the shadow is currently synchronized with (channel
    // switches swap Maxwell3D objects; a different source forces a resync).
    const Tegra::Engines::Maxwell3D* shadow_source{};
    bool shadow_in_sync{};
};

} // namespace Vulkan
