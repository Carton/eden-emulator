// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_draw_resolver.h"

#include <mutex>

#ifdef _MSC_VER
#include <immintrin.h>
#endif

#include "video_core/control/engine_override.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"

namespace Vulkan {

using VideoCommon::tls_engine_snapshot;

namespace {

void PauseSpin() {
#ifdef _MSC_VER
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}

} // Anonymous namespace

DrawResolver::DrawResolver(Tegra::MemoryManager& gpu_memory_, BufferCache& buffer_cache_,
                           TextureCache& texture_cache_)
    : gpu_memory{gpu_memory_}, buffer_cache{buffer_cache_}, texture_cache{texture_cache_},
      shadow{std::make_unique<Tegra::Engines::Maxwell3D>(gpu_memory_)} {
    thread = std::jthread([this](std::stop_token stop_token) { Run(stop_token); });
}

DrawResolver::~DrawResolver() {
    WaitResolved();
}

void DrawResolver::WaitResolved() {
    u32 spins{};
    while (job_phase.load(std::memory_order_acquire) == Phase::Resolving) {
        PauseSpin();
        if (++spins == 20000) {
            spins = 0;
            std::this_thread::yield();
        }
    }
}

bool DrawResolver::SnapshotAndKick(Tegra::Engines::Maxwell3D& engine,
                                   GraphicsPipeline* pipeline, bool is_indexed,
                                   u32 instance_count) {
    // Inline-index draws carry their indices in a vector that is not
    // snapshotted; they must run synchronously.
    const auto& src{engine.draw_manager.draw_state};
    if (src.draw_mode == Tegra::Engines::Maxwell3D::DrawManager::DrawMode::InlineIndex ||
        !src.inline_index_draw_indexes.empty()) {
        return false;
    }

    // Snapshot every piece of engine state the resolve and commit phases
    // read. This runs on the GPU thread with the resolver idle.
    shadow->regs = engine.regs;
    shadow->state = engine.state;
    {
        auto& dst{shadow->draw_manager.draw_state};
        dst.topology = src.topology;
        dst.draw_mode = src.draw_mode;
        dst.draw_indexed = src.draw_indexed;
        dst.base_index = src.base_index;
        dst.vertex_buffer = src.vertex_buffer;
        dst.index_buffer = src.index_buffer;
        dst.base_instance = src.base_instance;
        dst.instance_count = src.instance_count;
    }
    dirty_snapshot = engine.dirty.flags;
    shadow->dirty.flags = engine.dirty.flags;
    engine.dirty.flags_since_snapshot = {};
    engine.tracking_since_snapshot = true;

    job.pipeline = pipeline;
    job.is_indexed = is_indexed;
    job.instance_count = instance_count;
    job.ctx.Reset(shadow.get(), &gpu_memory);
    job_phase.store(Phase::Resolving, std::memory_order_release);
    return true;
}

void DrawResolver::Run(std::stop_token stop_token) {
    while (!stop_token.stop_requested()) {
        if (job_phase.load(std::memory_order_acquire) != Phase::Resolving) {
            PauseSpin();
            continue;
        }
        tls_engine_snapshot = shadow.get();
        {
            std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
            job.pipeline->ConfigureResolve(job.ctx, job.is_indexed);
        }
        tls_engine_snapshot = nullptr;
        job_phase.store(Phase::Resolved, std::memory_order_release);
    }
}

void DrawResolver::FinishJob(Tegra::Engines::Maxwell3D& engine) {
    // Merge dirty flags: bits consumed by the resolve/commit phases are
    // dropped unless the parser re-set them after the snapshot; bits newly
    // set by the parser or by commit code are kept.
    auto& shadow_flags{shadow->dirty.flags};
    auto& flags{engine.dirty.flags};
    const auto& since{engine.dirty.flags_since_snapshot};
    const auto consumed{dirty_snapshot & ~shadow_flags};
    flags = shadow_flags | (flags & ~dirty_snapshot) | (consumed & since);
    engine.dirty.flags_since_snapshot = {};
    engine.tracking_since_snapshot = false;
    job_phase.store(Phase::Idle, std::memory_order_release);
}

} // namespace Vulkan
