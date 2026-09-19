// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <memory>
#include <thread>

#include "video_core/engines/maxwell_3d.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

namespace Tegra {
class MemoryManager;
}

namespace Vulkan {

// (local-only) P2 depth-1 parallel draw resolver.
//
// The GPU thread snapshots draw N's engine state into the shadow engine and
// kicks the resolver to run the binding-resolution phase; parsing of draw
// N+1's command stream proceeds meanwhile. The commit phase (uploads +
// scheduler records) always runs on the GPU thread, in draw order, at the
// next rasterizer rendezvous. Exactly one job is in flight at any time.
class DrawResolver {
public:
    struct Job {
        GraphicsPipeline* pipeline{};
        bool is_indexed{};
        u32 instance_count{};
        DrawContext ctx;
    };

    DrawResolver(Tegra::MemoryManager& gpu_memory_, BufferCache& buffer_cache_,
                 TextureCache& texture_cache_);
    ~DrawResolver();

    DrawResolver(const DrawResolver&) = delete;
    DrawResolver& operator=(const DrawResolver&) = delete;

    bool ResolveInFlight() const {
        return job_phase.load(std::memory_order_relaxed) != Phase::Idle;
    }

    // GPU thread: block until the resolver finished the current job (returns
    // immediately when idle). Bounded by one binding-resolution pass.
    void WaitResolved();

    // GPU thread: snapshot the draw into the shadow engine and kick the
    // resolver. Returns false (doing nothing) when the draw must run
    // synchronously.
    bool SnapshotAndKick(Tegra::Engines::Maxwell3D& engine, GraphicsPipeline* pipeline,
                         bool is_indexed, u32 instance_count);

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

private:
    enum class Phase : u32 {
        Idle,
        Resolving,
        Resolved,
    };

    void Run(std::stop_token stop_token);

    Tegra::MemoryManager& gpu_memory;
    BufferCache& buffer_cache;
    TextureCache& texture_cache;
    std::unique_ptr<Tegra::Engines::Maxwell3D> shadow;

    Job job;
    std::atomic<Phase> job_phase{Phase::Idle};
    Tegra::Engines::Maxwell3D::DirtyState::Flags dirty_snapshot{};

    std::jthread thread;
};

} // namespace Vulkan
