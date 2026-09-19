// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_draw_resolver.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <thread>

#include "common/logging.h"
#include "video_core/control/engine_override.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"

namespace Vulkan {

using VideoCommon::tls_engine_snapshot;

namespace {
using Clock = std::chrono::steady_clock;
}

DrawResolver::DrawResolver(Tegra::MemoryManager& gpu_memory_, BufferCache& buffer_cache_,
                           TextureCache& texture_cache_)
    : gpu_memory{gpu_memory_}, buffer_cache{buffer_cache_}, texture_cache{texture_cache_},
      shadow{std::make_unique<Tegra::Engines::Maxwell3D>(gpu_memory_)} {}

DrawResolver::~DrawResolver() {
    WaitResolved();
}

void DrawResolver::WaitResolved() {
    while (job_phase.load(std::memory_order_acquire) == Phase::Resolving) {
        std::this_thread::yield();
    }
}

void DrawResolver::CopyDynamicState(Tegra::Engines::Maxwell3D& engine) {
    // State outside the flat register array, copied wholesale in every mode:
    // shader-stage constant-buffer bindings (ProcessCBBind), draw parameters
    // and dirty flags. Together a few KiB, not worth journaling.
    shadow->state = engine.state;
    {
        const auto& src{engine.draw_manager.draw_state};
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
}

bool DrawResolver::SnapshotAndEnqueue(Tegra::Engines::Maxwell3D& engine,
                                      GraphicsPipeline* pipeline, bool is_indexed,
                                      u32 instance_count) {
    // Inline-index draws carry their indices in a vector that is not
    // snapshotted; they must run synchronously.
    const auto& src{engine.draw_manager.draw_state};
    if (src.draw_mode == Tegra::Engines::Maxwell3D::DrawManager::DrawMode::InlineIndex ||
        !src.inline_index_draw_indexes.empty()) {
        return false;
    }

    if (snapshot_mode == SnapshotMode::Journal) {
        engine.journal_active = true;
        const bool must_resync =
            !shadow_in_sync || shadow_source != &engine || engine.journal_overflow;
        if (must_resync) {
            shadow->regs = engine.regs;
            engine.reg_journal_consumed = engine.reg_journal_size;
            shadow_in_sync = true;
            shadow_source = &engine;
            engine.journal_overflow = false;
            ++diag_resyncs;
            slot_journal_size = 0;
        } else {
            const size_t pending{engine.reg_journal_size - engine.reg_journal_consumed};
            std::memcpy(slot_journal, engine.reg_journal.data() + engine.reg_journal_consumed,
                        pending * sizeof(engine.reg_journal[0]));
            slot_journal_size = pending;
            engine.reg_journal_consumed = engine.reg_journal_size;
            diag_journal_entries += pending;
        }
        // Compact the engine buffer once fully consumed; capacity then bounds
        // the entries of a single inter-snapshot interval (one draw).
        if (engine.reg_journal_consumed == engine.reg_journal_size) {
            engine.reg_journal_size = 0;
            engine.reg_journal_consumed = 0;
        }
    } else {
        shadow->regs = engine.regs;
        slot_journal_size = 0;
    }
    CopyDynamicState(engine);

    job.pipeline = pipeline;
    job.is_indexed = is_indexed;
    job.instance_count = instance_count;
    job.ctx.Reset(shadow.get(), &gpu_memory);
    job_phase.store(Phase::Resolving, std::memory_order_release);
    if (++diag_kicks % 2000 == 0) {
        LOG_INFO(Render_Vulkan,
                 "DrawToken diag: kicks={} fallbacks={} resyncs={} "
                 "journal_avg={:.1f} resolve_avg_ns={} replay_avg_ns={} mismatches={}",
                 diag_kicks, diag_fallbacks, diag_resyncs,
                 diag_resolve_calls ? static_cast<double>(diag_journal_entries) /
                                          static_cast<double>(diag_resolve_calls)
                                   : 0.0,
                 diag_resolve_calls ? diag_resolve_ns.count() / diag_resolve_calls : 0,
                 diag_resolve_calls ? diag_replay_ns.count() / diag_resolve_calls : 0,
                 diag_snapshot_mismatches);
    }
    return true;
}

void DrawResolver::ExecuteResolve() {
    // Runs on the VulkanWorker (token mode) or inline (validation mode).
    tls_engine_snapshot = shadow.get();
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        const auto replay_start{Clock::now()};
        if (slot_journal_size != 0) {
            shadow->ReplayJournal(slot_journal, slot_journal_size);
        }
        const auto resolve_start{Clock::now()};
        job.pipeline->ConfigureResolve(job.ctx, job.is_indexed);
        diag_replay_ns += resolve_start - replay_start;
        diag_resolve_ns += Clock::now() - resolve_start;
        // (local-only) uniform epoch: snapshot the constant-buffer contents
        // from the shadow engine now -- resolve time matches the serial
        // path's read moment, so the guest still holds this draw's bytes.
        // ConfigureResolve has installed this pipeline's uniform masks and
        // shader-declared sizes; the addresses come from the shadow state.
        // The delayed tail reads this copy instead of guest memory.
        job.epoch_valid = false;
        if (epoch_enabled) {
            job.epoch_entry_count = buffer_cache.CaptureUniformEpoch(
                *shadow, job.epoch_bytes.data(), job.epoch_bytes.size(),
                job.epoch_entries.data(), job.epoch_entries.size());
            job.epoch_snapshot = {job.epoch_entries.data(), job.epoch_entry_count,
                                  job.epoch_bytes.data()};
            job.epoch_valid = true;
        }
    }
    tls_engine_snapshot = nullptr;
    ++diag_resolve_calls;
    job_phase.store(Phase::Resolved, std::memory_order_release);
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

bool DrawResolver::VerifySnapshot(const Tegra::Engines::Maxwell3D& engine) {
    // expected == shadow + journal entries parsed since the snapshot.
    auto expected{shadow->regs};
    for (size_t i = engine.reg_journal_consumed; i < engine.reg_journal_size; ++i) {
        expected.reg_array[engine.reg_journal[i].method] = engine.reg_journal[i].value;
    }
    const u32* lhs{expected.reg_array.data()};
    const u32* live{engine.regs.reg_array.data()};
    for (size_t i = 0; i < expected.reg_array.size(); ++i) {
        if (lhs[i] != live[i]) {
            ++diag_snapshot_mismatches;
            LOG_ERROR(Render_Vulkan,
                      "DrawToken shadow mismatch at reg {:#x}: shadow={} live={} "
                      "(mismatch #{})",
                      i * sizeof(u32), lhs[i], live[i], diag_snapshot_mismatches);
            return false;
        }
    }
    return true;
}

} // namespace Vulkan
