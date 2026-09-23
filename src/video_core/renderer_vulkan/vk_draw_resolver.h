// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <functional>
#include <memory>

#include "video_core/control/engine_override.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Tegra {
class MemoryManager;
}

namespace Vulkan {

class Scheduler;
class StateTracker;

// (local-only) P2 Step 2: draw tokens.
//
// The GPU thread snapshots a draw into the shadow engine and executes the
// resolve phase inline, or on its own worker with an immediate GPU rendezvous
// (stage 1B). Stage 2A queues private snapshots and overlaps parser work.
// The commit phase (uploads + scheduler records) runs on the GPU thread,
// in draw order, at the
// next rasterizer rendezvous. Only the FIFO head may resolve; its tail must
// finish before the next head is armed (shared cache bindings).
// Stage 3 instead lends the whole resolve + tail interval to the private
// worker, immediately waits, then retires on the GPU. It does not use the FIFO.
//
// Snapshot modes:
//  - FullCopy: copy regs/state every draw (depth-1 behaviour, known-good).
//  - Journal:  maintain the shadow incrementally by replaying the engine's
//              register-write journal; full copy only on (re)sync events
//              (first use, channel switch, journal overflow).
// Stage 2A retains per-slot register baselines and bounded catch-up journals.
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
        // Stage 3: published by the immediate worker rendezvous, consumed once
        // by the GPU retirement path. Never authorizes replay after failure.
        bool tail_complete{};
        std::chrono::nanoseconds tail_ns{};
        std::array<u32, 6> expected_draw_inputs{}; // stage 4: LIVE enqueue oracle
    };

    DrawResolver(Tegra::MemoryManager& gpu_memory_, BufferCache& buffer_cache_,
                 TextureCache& texture_cache_, StateTracker& state_tracker_);
    ~DrawResolver();

    DrawResolver(const DrawResolver&) = delete;
    DrawResolver& operator=(const DrawResolver&) = delete;

    bool ResolveInFlight() const;
    bool PipelineFull() const;
    void PollResolveSync();
    // Wait only an already armed head before GPU cache/scheduler preparation.
    void PreparePipelineEnqueue();
    // Rasterizer destruction only, after the ordinary GPU producer has stopped.
    void BeginPipelineTeardown();
    bool TailFrontReady();
    void ReturnTailOwnership(Tegra::Engines::Maxwell3D& engine);
    void LogTailPipelineDiag();
    void AbortTailPipeline(std::exception_ptr error) noexcept;

    // GPU thread: block until the current job finished resolving (returns
    // immediately when none is running).
    void WaitResolved();

    // GPU thread: snapshot the draw into the shadow engine and enqueue it.
    // Returns false (doing nothing) when the draw must run synchronously.
    bool SnapshotAndEnqueue(Tegra::Engines::Maxwell3D& engine, GraphicsPipeline* pipeline,
                            bool is_indexed, u32 instance_count);

    // GPU entry. Stage 1B signals the private worker and immediately waits,
    // servicing synchronization requests. Stage 2A only kicks; WaitResolved
    // services requests and splices at consumption.
    void ExecuteResolve(Scheduler& scheduler);

    // GPU thread: after WaitResolved(), the finished job.
    Job& TakeJob();

    Tegra::Engines::Maxwell3D& SnapshotEngine() const;

    // GPU thread: release the job slot after the tail. Legacy modes merge
    // dirty flags; 2A passes residual flags to the next slot or back to live.
    void FinishJob(Tegra::Engines::Maxwell3D& engine);

    // GPU thread, debug: verify the shadow converged to the live registers.
    // Applies the not-yet-consumed journal entries to a scratch copy first,
    // so writes parsed after the snapshot do not count as mismatches.
    // Returns false and logs on the first divergent register.
    bool VerifySnapshot(const Tegra::Engines::Maxwell3D& engine);

    // Called once by EnsureResolver after check_enabled is configured; seeds
    // bind-time inputs from the current channel before parser interception.
    void EnableJobBindings(const Tegra::Engines::Maxwell3D& engine);
    void BindUniformInput(size_t stage, u32 index, GPUVAddr addr, u32 size);
    void DisableUniformInput(size_t stage, u32 index);
    // GPU-only, caller drained all jobs and holds B; for fallback/channel handoff.
    void ApplyPendingUniformInputs();

    SnapshotMode snapshot_mode{SnapshotMode::Journal};
    bool check_enabled{false};
    bool job_bindings_enabled{false}; // strict EDEN_TOKEN_JOB_BINDINGS=1
    bool batch_enabled{false}; // EDEN_TOKEN_BATCH=1; never enables worker resolve
    bool worker_enabled{false}; // EDEN_TOKEN_WORKER=1 alone: batch + immediate wait
    bool tail_worker_enabled{false}; // stage 3: exclusive producer loan, immediate wait
    bool tail_pipeline_enabled{false}; // stage 4, independent strict opt-in
    std::function<void(Tegra::Engines::Maxwell3D&, Job&)> worker_tail;
    std::function<void(const Tegra::Engines::Maxwell3D&, Job&)> capture_tail_inputs;
    bool pipeline_enabled{false}; // EDEN_TOKEN_PIPELINE=1; queue + consumer-side wait
    static constexpr u32 MaxPipelineDepth = 4;
    u32 pipeline_depth{1};
    u32 spin_us{20}; // pipeline only; zero retains pure parking
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
    u64 diag_capture_batches{}; // successful capture scopes, including empty ones
    u64 diag_captured_bytes{};  // command arena bytes, including successful alignment
    u64 diag_splice_count{};    // nonempty prefix publications (Finish may split a batch)
    u64 diag_worker_resolves{};
    u64 diag_worker_sync_requests{};
    u64 diag_tail_epoch_hits{}, diag_tail_epoch_misses{}, diag_tail_epoch_classic{};
    u64 diag_pipeline_resolves{};
    std::chrono::nanoseconds diag_pipeline_snapshot_ns{};
    u64 diag_pipeline_snapshot_count{};
    u64 diag_pipeline_max_inflight{};
    u64 diag_pipeline_spin_wins{};
    u64 diag_pipeline_parks{};
    u64 diag_pipeline_backpressure{};
    std::atomic<u64> diag_pipeline_worker_spin_wins{};
    std::atomic<u64> diag_pipeline_worker_parks{};
    std::chrono::nanoseconds diag_resolve_ns{};
    std::chrono::nanoseconds diag_replay_ns{};

private:
    enum class Phase : u32 {
        Idle,
        Resolving,
        Resolved,
    };

    void CopyDynamicState(Tegra::Engines::Maxwell3D& engine);
    struct WorkerState;
    void ExecuteResolveImpl(Scheduler& scheduler, WorkerState* worker_state);

    Tegra::MemoryManager& gpu_memory;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    StateTracker& state_tracker;
    std::unique_ptr<Tegra::Engines::Maxwell3D> shadow;

    // GPU-owned parser mirror. Keep translated bind-time inputs, not host IDs.
    std::array<std::array<VideoCommon::GraphicsUniformBindingRecord,
                          VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS>, VideoCommon::NUM_STAGES>
        uniform_inputs{};
    std::array<u32, VideoCommon::NUM_STAGES> uniform_pending{};
    VideoCommon::GraphicsUniformBindingVersions uniform_applied_versions{};
    void CaptureUniformInputs(DrawContext& ctx, const std::array<u32, 5>& used);

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
    // Lazily allocated for 1B/stage 3. Joined before any job/cache references die.
    std::unique_ptr<WorkerState> worker;
    std::exception_ptr tail_worker_error; // GPU-owned poison; submitted prefixes cannot replay
    struct PipelineState;
    std::unique_ptr<PipelineState> pipeline_state;
    bool tail_ownership{}; // GPU-only; cache-local routing changes only at handoff
};

} // namespace Vulkan
