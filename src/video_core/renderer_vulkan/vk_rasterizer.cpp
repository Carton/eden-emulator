// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <stdexcept>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/renderer_vulkan.h"

#include "common/assert.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/control/channel_state.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/renderer_vulkan/blit_image.h"
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_compute_pipeline.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_draw_resolver.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_query_cache.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_tail_pipeline_diag.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/shader_cache.h"
#include "video_core/texture_cache/texture_cache_base.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

using Maxwell = Tegra::Engines::Maxwell3D::Regs;
using VideoCommon::ImageViewId;
using VideoCommon::ImageViewType;


namespace {
thread_local RasterizerVulkan* draw_owner{};

struct DrawParams {
    u32 base_instance;
    u32 num_instances;
    u32 base_vertex;
    u32 num_vertices;
    u32 first_index;
    bool is_indexed;
};

std::array<u32, 6> DrawInputWords(const DrawParams& p) {
    return {p.base_instance, p.num_instances, p.base_vertex, p.num_vertices,
            p.first_index, static_cast<u32>(p.is_indexed)};
}

VkViewport GetViewportState(const Device& device, const Maxwell& regs, size_t index, float scale) {
    const auto& src = regs.viewport_transform[index];
    const auto conv = [scale](float value) {
        float new_value = value * scale;
        if (scale < 1.0f) {
            const bool sign = std::signbit(value);
            new_value = std::round(std::abs(new_value));
            new_value = sign ? -new_value : new_value;
        }
        return new_value;
    };
    const float x = conv(src.translate_x - src.scale_x);
    const float width = conv(src.scale_x * 2.0f);
    float y = conv(src.translate_y - src.scale_y);
    float height = conv(src.scale_y * 2.0f);

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const bool y_negate = !device.IsNvViewportSwizzleSupported() &&
                          src.swizzle.y == Maxwell::ViewportSwizzle::NegativeY;

    if (lower_left) {
        // Flip by surface clip height
        y += conv(static_cast<f32>(regs.surface_clip.height));
        height = -height;
    }

    if (y_negate) {
        // Flip by viewport height
        y += height;
        height = -height;
    }

    const float reduce_z = regs.depth_mode == Maxwell::DepthMode::MinusOneToOne ? 1.0f : 0.0f;
    VkViewport viewport{
        .x = x,
        .y = y,
        .width = width != 0.0f ? width : 1.0f,
        .height = height != 0.0f ? height : 1.0f,
        .minDepth = src.translate_z - src.scale_z * reduce_z,
        .maxDepth = src.translate_z + src.scale_z,
    };
    if (!device.IsExtDepthRangeUnrestrictedSupported()) {
        viewport.minDepth = std::clamp(viewport.minDepth, 0.0f, 1.0f);
        viewport.maxDepth = std::clamp(viewport.maxDepth, 0.0f, 1.0f);
    }
    return viewport;
}

VkRect2D GetScissorState(const Maxwell& regs, size_t index, u32 up_scale = 1, u32 down_shift = 0) {
    const auto& src = regs.scissor_test[index];
    VkRect2D scissor{};
    const auto scale_up = [&](s32 value) -> s32 {
        if (value == 0) {
            return 0U;
        }
        const s32 upset = value * up_scale;
        s32 acumm = 0;
        if ((up_scale >> down_shift) == 0) {
            acumm = upset % 2;
        }
        const s32 converted_value = (value * up_scale) >> down_shift;
        return value < 0 ? std::min<s32>(converted_value - acumm, -1)
                         : std::max<s32>(converted_value + acumm, 1);
    };

    const bool lower_left = regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft;
    const s32 clip_height = regs.surface_clip.height;

    // Flip coordinates if lower left
    s32 min_y = lower_left ? (clip_height - src.max_y) : src.min_y.Value();
    s32 max_y = lower_left ? (clip_height - src.min_y) : src.max_y.Value();

    // Bound to render area
    min_y = (std::max)(min_y, 0);
    max_y = (std::max)(max_y, 0);

    if (src.enable) {
        scissor.offset.x = scale_up(src.min_x);
        scissor.offset.y = scale_up(min_y);
        scissor.extent.width = scale_up(src.max_x - src.min_x);
        scissor.extent.height = scale_up(max_y - min_y);
    } else {
        scissor.offset.x = 0;
        scissor.offset.y = 0;
        scissor.extent.width = (std::numeric_limits<s32>::max)();
        scissor.extent.height = (std::numeric_limits<s32>::max)();
    }
    return scissor;
}

// GPU-thread-only counters. Indirect draws never enter Draw(); without these
// the token diag cannot distinguish "scene has no indirect draws" from "never
// looked" (the fallbacks counter only covers inline-index fallbacks).
u64 diag_draw_indirect_calls{};
u64 diag_draw_indirect_byte_count{};
u64 diag_draw_indirect_count_buffer{};

DrawParams MakeDrawParams(const Tegra::Engines::Maxwell3D::DrawManager::State& draw_state, u32 num_instances, bool is_indexed) {
    DrawParams params{
        .base_instance = draw_state.base_instance,
        .num_instances = num_instances,
        .base_vertex = is_indexed ? draw_state.base_index : draw_state.vertex_buffer.first,
        .num_vertices = is_indexed ? draw_state.index_buffer.count : draw_state.vertex_buffer.count,
        .first_index = is_indexed ? draw_state.index_buffer.first : 0,
        .is_indexed = is_indexed,
    };
    // 6 triangle vertices per quad, base vertex is part of the index
    // See BindQuadIndexBuffer for more details
    if (draw_state.topology == Maxwell::PrimitiveTopology::Quads) {
        params.num_vertices = (params.num_vertices / 4) * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    } else if (draw_state.topology == Maxwell::PrimitiveTopology::QuadStrip) {
        params.num_vertices = (params.num_vertices - 2) / 2 * 6;
        params.base_vertex = 0;
        params.is_indexed = true;
    }
    return params;
}

bool SupportsPrimitiveRestart(VkPrimitiveTopology topology) {
    switch (topology) {
    case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
    case VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY:
    case VK_PRIMITIVE_TOPOLOGY_PATCH_LIST:
        return false;
    default:
        return true;
    }
}

bool IsPrimitiveRestartSupported(const Device& device, VkPrimitiveTopology topology) {
    return ((topology != VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsTopologyListPrimitiveRestartSupported()) ||
            SupportsPrimitiveRestart(topology) ||
            (topology == VK_PRIMITIVE_TOPOLOGY_PATCH_LIST &&
             device.IsPatchListPrimitiveRestartSupported()));
}
} // Anonymous namespace

RasterizerVulkan::RasterizerVulkan(Core::Frontend::EmuWindow& emu_window_, Tegra::GPU& gpu_,
                                   Tegra::MaxwellDeviceMemoryManager& device_memory_,
                                   const Device& device_, MemoryAllocator& memory_allocator_,
                                   StateTracker& state_tracker_, Scheduler& scheduler_)
    : gpu{gpu_}, device_memory{device_memory_}, device{device_},
      memory_allocator{memory_allocator_}, state_tracker{state_tracker_}, scheduler{scheduler_},
      staging_pool(device, memory_allocator, scheduler), descriptor_pool(device, scheduler),
      guest_descriptor_queue(device, UpdateDescriptorQueue::GUEST_FRAME_PAYLOAD_SIZE,
                             device.IsExtDescriptorBufferSupported()),
      compute_pass_descriptor_queue(device, UpdateDescriptorQueue::COMPUTE_FRAME_PAYLOAD_SIZE),
      descriptor_buffer_ring(device, memory_allocator),
      blit_image(device, scheduler, state_tracker, descriptor_pool), render_pass_cache(device),
      texture_cache_runtime{
          device,     scheduler,         memory_allocator, staging_pool,
          blit_image, render_pass_cache, descriptor_pool,  compute_pass_descriptor_queue},
      texture_cache(texture_cache_runtime, device_memory),
      buffer_cache_runtime(device, memory_allocator, scheduler, staging_pool,
                           guest_descriptor_queue, compute_pass_descriptor_queue, descriptor_pool),
      buffer_cache(device_memory, buffer_cache_runtime),
      query_cache_runtime(this, device_memory, buffer_cache, device, memory_allocator, scheduler,
                          staging_pool, compute_pass_descriptor_queue, descriptor_pool, texture_cache),
      query_cache(gpu, *this, device_memory, query_cache_runtime),
      pipeline_cache(device_memory, device, scheduler, descriptor_pool, guest_descriptor_queue,
                     descriptor_buffer_ring, render_pass_cache, buffer_cache, texture_cache,
                     gpu.ShaderNotify()),
      accelerate_dma(buffer_cache, texture_cache, scheduler),
      fence_manager(*this, gpu, texture_cache, buffer_cache, query_cache, device, scheduler),
      wfi_event(device.GetLogical().CreateEvent()) {
    scheduler.SetQueryCache(query_cache);
    // (local-only) P2 Step 2 draw tokens. EDEN_DRAW_TOKEN selects the token
    // pipeline: "1"/"inline" resolves on the GPU thread after the snapshot.
    // Worker modes "sync"/"async" fall back to inline (scheduler re-entry).
    // EDEN_TOKEN_SNAPSHOT=full forces the depth-1 whole-register copy;
    // EDEN_TOKEN_CHECK=1 verifies the shadow register state bit-exactly.
    const char* token{std::getenv("EDEN_DRAW_TOKEN")};
    if (token && *token != '\0' && *token != '0') {
        token_mode = TokenMode::Inline;
        if (*token == 's' || *token == 'S' || *token == 'a' || *token == 'A') {
            // Resolve can record commands, dispatch chunks, and wait for the
            // worker through texture runtime operations. Draining the GPU
            // thread does not make scheduler re-entry from its worker safe.
            LOG_WARNING(Render_Vulkan,
                        "DrawToken worker resolve disabled (scheduler re-entry); using inline");
        }
        const char* snapshot{std::getenv("EDEN_TOKEN_SNAPSHOT")};
        const char* check{std::getenv("EDEN_TOKEN_CHECK")};
        token_check_enabled = check != nullptr && *check != '\0' && *check != '0';
        const char* tail_imm{std::getenv("EDEN_TOKEN_TAIL_IMM")};
        token_tail_immediate = tail_imm != nullptr && *tail_imm != '\0' && *tail_imm != '0';
        LOG_INFO(Render_Vulkan,
                 "Draw tokens: mode={} snapshot={} check={}",
                 "inline",
                 (snapshot && *snapshot == 'f') ? "full" : "journal",
                 token_check_enabled ? "on" : "off");
    }
}

RasterizerVulkan::~RasterizerVulkan() {
    if (resolver && resolver->pipeline_enabled) {
        // Destruction is quiescent with respect to the ordinary GPU producer.
        // Finish pending tails even when teardown runs on a different thread.
        resolver->BeginPipelineTeardown();
        if (resolver->tail_pipeline_enabled) {
            const auto index = static_cast<size_t>(DrawDrain::Teardown);
            ++tail_drain_calls[index];
            tail_drains[index] += pending_commit.load(std::memory_order_acquire);
        }
        if (resolver->tail_pipeline_enabled) {
            const auto teardown_begin = TailPipelineDiag::Clock::now();
            try {
                while (pending_commit.load(std::memory_order_acquire)) {
                    CommitPendingDraw();
                    ++tail_drain_jobs[static_cast<size_t>(DrawDrain::Teardown)];
                }
            } catch (...) {
                resolver->AbortTailPipeline(std::current_exception());
                pending_commit.store(false, std::memory_order_release);
            }
            TailPipelineDiag::Metric("rasterizer_final", resolver.get(), "gpu_teardown_drain",
                                    1, TailPipelineDiag::Ns(TailPipelineDiag::Clock::now() - teardown_begin),
                                    pipelined_draws);
        } else {
            while (pending_commit.load(std::memory_order_acquire)) {
                CommitPendingDraw();
            }
        }
        if (resolver->tail_pipeline_enabled) {
            resolver->ReturnTailOwnership(*maxwell3d);
            LogTokenDiag(true);
        }
    }
    if (resolver) {
        resolver->WaitResolved();
    }
    if (pipelined_draws || fallback_draws || diag_draw_indirect_calls) {
        LOG_INFO(Render_Vulkan,
                 "Draw tokens: {} pipelined, {} synchronous fallback, {} indirect",
                 pipelined_draws, fallback_draws, diag_draw_indirect_calls);
    }
    // Drain queued token commands before member teardown frees the resolver.
    scheduler.WaitWorker();
    scheduler.Finish();
    if (draw_owner == this) {
        draw_owner = nullptr;
    }
}

template <typename Func>
void RasterizerVulkan::PrepareDraw(bool is_indexed, Func&& draw_func) {

    FlushPendingDraw();

    // (local-only) per-draw timing diag: whole serial draw path, averaged
    // over thousands of draws so scene load bands cancel out.
    const auto prepare_start{std::chrono::steady_clock::now()};
    auto phase_start = prepare_start;
    const auto phase_end = [&](size_t index) {
        const auto now = std::chrono::steady_clock::now();
        diag_prepare_phases_ns[index] += static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now - phase_start).count());
        phase_start = now;
    };
    SCOPE_EXIT {
        // Reuse the last phase boundary: seven clocks on a complete draw.
        // Excludes lock destruction and gpu.TickWork(), unlike the old outer timer.
        diag_prepare_ns += phase_start - prepare_start;
        if (++diag_prepare_calls % 5000 == 0) {
            LOG_INFO(Render_Vulkan, "SerialDraw diag: calls={} prepare_avg_ns={}",
                     diag_prepare_calls, diag_prepare_ns.count() / diag_prepare_calls);
            LOG_INFO(Render_Vulkan,
                     "SerialDraw phases diag: calls={} prologue_ns={} lock_setup_ns={} "
                     "resolve_ns={} configure_tail_ns={} dynamic_query_ns={} emit_ns={} "
                     "null_pipeline={} tail_rejected={}",
                     diag_prepare_calls, diag_prepare_phases_ns[0] / diag_prepare_calls,
                     diag_prepare_phases_ns[1] / diag_prepare_calls,
                     diag_prepare_phases_ns[2] / diag_prepare_calls,
                     diag_prepare_phases_ns[3] / diag_prepare_calls,
                     diag_prepare_phases_ns[4] / diag_prepare_calls,
                     diag_prepare_phases_ns[5] / diag_prepare_calls,
                     diag_prepare_null_pipeline, diag_prepare_tail_rejected);
        }
    };
    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();
    gpu_memory->FlushCaching();

    GraphicsPipeline* const pipeline{pipeline_cache.CurrentGraphicsPipeline()};
    phase_end(0);
    if (!pipeline) {
        ++diag_prepare_null_pipeline;
        return;
    }
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    // update engine as channel may be different.
    pipeline->SetEngine(maxwell3d, gpu_memory);
    draw_ctx.Reset(maxwell3d, gpu_memory);
    if (resolver && resolver->job_bindings_enabled) {
        resolver->ApplyPendingUniformInputs();
    }
    // (local-only) P2 phase split: resolve (binding lookups) then tail
    // (uploads + scheduler records), synchronously.
    phase_end(1);
    pipeline->ConfigureResolve(draw_ctx, is_indexed);
    phase_end(2);
    const bool configured = pipeline->ConfigureTail(draw_ctx, is_indexed);
    phase_end(3);
    if (!configured) {
        ++diag_prepare_tail_rejected;
        return;
    }

    UpdateDynamicStates(*maxwell3d, pipeline);

    query_cache.NotifySegment(true);
    HandleTransformFeedback(*maxwell3d);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              maxwell3d->regs.zpass_pixel_count_enable);
    phase_end(4);
    draw_func();
    phase_end(5);
}

void RasterizerVulkan::EnsureResolver() {
    if (resolver) {
        return;
    }
    resolver = std::make_unique<DrawResolver>(*gpu_memory, buffer_cache, texture_cache,
                                               state_tracker);
    const char* snapshot{std::getenv("EDEN_TOKEN_SNAPSHOT")};
    resolver->snapshot_mode = (snapshot && (*snapshot == 'f' || *snapshot == 'F'))
                                  ? DrawResolver::SnapshotMode::FullCopy
                                  : DrawResolver::SnapshotMode::Journal;
    resolver->check_enabled = token_check_enabled;
    const char* tail_pipeline{std::getenv("EDEN_TOKEN_TAIL_PIPELINE")};
    resolver->tail_pipeline_enabled =
        tail_pipeline && tail_pipeline[0] == '1' && tail_pipeline[1] == '\0';
    const char* tail_worker{std::getenv("EDEN_TOKEN_TAIL_WORKER")};
    resolver->tail_worker_enabled =
        resolver->tail_pipeline_enabled ||
        (tail_worker && tail_worker[0] == '1' && tail_worker[1] == '\0');
    const char* job_bindings{std::getenv("EDEN_TOKEN_JOB_BINDINGS")};
    if (resolver->tail_worker_enabled ||
        (job_bindings && job_bindings[0] == '1' && job_bindings[1] == '\0')) {
        resolver->EnableJobBindings(*maxwell3d);
        LOG_INFO(Render_Vulkan, "DrawToken job bindings enabled: check={}", token_check_enabled);
    }
    const char* batch{std::getenv("EDEN_TOKEN_BATCH")};
    resolver->batch_enabled = batch && batch[0] == '1' && batch[1] == '\0';
    const char* worker{std::getenv("EDEN_TOKEN_WORKER")};
    resolver->worker_enabled = worker && worker[0] == '1' && worker[1] == '\0';
    const char* token_pipeline{std::getenv("EDEN_TOKEN_PIPELINE")};
    resolver->pipeline_enabled =
        token_pipeline && token_pipeline[0] == '1' && token_pipeline[1] == '\0';
    if (resolver->tail_worker_enabled) {
        if (resolver->pipeline_enabled && !resolver->tail_pipeline_enabled) {
            LOG_WARNING(Render_Vulkan,
                        "DrawToken tail worker overrides PIPELINE: immediate rendezvous only");
        }
        resolver->pipeline_enabled = resolver->tail_pipeline_enabled;
        resolver->worker_enabled = true;
        if (resolver->tail_pipeline_enabled) {
            resolver->capture_tail_inputs = [](const Tegra::Engines::Maxwell3D& live,
                                               DrawResolver::Job& job) {
                job.expected_draw_inputs = DrawInputWords(MakeDrawParams(
                    live.draw_manager.draw_state, job.instance_count, job.is_indexed));
            };
        }
        resolver->worker_tail = [this](Tegra::Engines::Maxwell3D& shadow, DrawResolver::Job& job) {
            // Stage 3 reads the parked live engine. Stage 4 uses an independent
            // LIVE enqueue reference; it never reads the concurrently parsing engine.
            // Indirect/inline-index draws still take the existing GPU fallback.
            bool equivalent = true;
            if (token_check_enabled) {
                const auto expected = resolver->tail_pipeline_enabled ? job.expected_draw_inputs
                    : DrawInputWords(MakeDrawParams(maxwell3d->draw_manager.draw_state,
                                                   job.instance_count, job.is_indexed));
                equivalent = expected == DrawInputWords(MakeDrawParams(
                    shadow.draw_manager.draw_state, job.instance_count, job.is_indexed));
            }
            const auto tail_start = std::chrono::steady_clock::now();
            FinishDrawLocked(shadow, *job.pipeline, job.ctx, job.is_indexed, job.instance_count);
            job.tail_ns = std::chrono::steady_clock::now() - tail_start;
            RecordWorkerTailDiag(token_check_enabled, equivalent);
        };
        if (resolver->tail_pipeline_enabled) {
            // Separate instrumented callback: Stage 3 retains its exact body.
            resolver->worker_tail = [this](Tegra::Engines::Maxwell3D& shadow, DrawResolver::Job& job) {
                bool equivalent = true;
                if (token_check_enabled) {
                    equivalent = job.expected_draw_inputs == DrawInputWords(MakeDrawParams(
                        shadow.draw_manager.draw_state, job.instance_count, job.is_indexed));
                }
                auto& timing = TailPipelineDiag::worker_job;
                timing.tail_begin = std::chrono::steady_clock::now();
                timing.emitted = false;
                FinishDrawLockedMeasured(shadow, *job.pipeline, job.ctx, job.is_indexed,
                                         job.instance_count);
                timing.tail_end = std::chrono::steady_clock::now();
                job.tail_ns = timing.tail_end - timing.tail_begin;
                RecordWorkerTailDiag(token_check_enabled, equivalent);
            };
        }
        if (!resolver->tail_pipeline_enabled) {
            LOG_INFO(Render_Vulkan,
                 "DrawToken tail worker: immediate rendezvous; implies JOB_BINDINGS, WORKER, BATCH; "
                 "PIPELINE/depth/spin and TAIL_IMM do not alter this mode");
        }
    }
    if (resolver->pipeline_enabled) {
        const auto bounded_env = [](const char* name, u32 fallback, u32 low, u32 high) {
            const char* value = std::getenv(name);
            if (!value) {
                return fallback;
            }
            u32 parsed{};
            const auto end = value + std::strlen(value);
            const auto result = std::from_chars(value, end, parsed);
            if (result.ec != std::errc{} || result.ptr != end || parsed < low || parsed > high) {
                LOG_WARNING(Render_Vulkan, "Invalid {}={}, using {}", name, value, fallback);
                return fallback;
            }
            return parsed;
        };
        resolver->pipeline_depth = bounded_env("EDEN_TOKEN_PIPELINE_DEPTH",
                                               resolver->tail_pipeline_enabled ? 2 : 1, 1,
                                               DrawResolver::MaxPipelineDepth);
        resolver->spin_us = bounded_env("EDEN_TOKEN_SPIN_US", 20, 0, 1000);
        resolver->worker_enabled = true;
        if (resolver->tail_pipeline_enabled) {
            LOG_INFO(Render_Vulkan,
                     "DrawToken tail FIFO enabled: depth={} spin_us={} snapshot={}",
                     resolver->pipeline_depth, resolver->spin_us,
                     resolver->snapshot_mode == DrawResolver::SnapshotMode::FullCopy ? "full"
                                                                                   : "journal");
        } else {
            LOG_INFO(Render_Vulkan,
                     "DrawToken pipeline: depth={} spin_us={} snapshot={} tail-gated resolves",
                     resolver->pipeline_depth, resolver->spin_us,
                     resolver->snapshot_mode == DrawResolver::SnapshotMode::FullCopy ? "full"
                                                                                   : "journal");
        }
    }
    resolver->batch_enabled |= resolver->worker_enabled;
    // (local-only) EDEN_TOKEN_EPOCH=0 turns the uniform epoch capture off
    // (A/B switch: tail keeps reading guest memory directly).
    const char* epoch{std::getenv("EDEN_TOKEN_EPOCH")};
    resolver->epoch_enabled = !(epoch && *epoch != '\0' && *epoch == '0');
    // (local-only) EDEN_TOKEN_EPOCH_MEMCMP=0 forces a full copy on every
    // epoch recapture (A/B arm isolating the memcmp skip's value).
    const char* epoch_memcmp{std::getenv("EDEN_TOKEN_EPOCH_MEMCMP")};
    resolver->epoch_table.short_circuit =
        !(epoch_memcmp && *epoch_memcmp != '\0' && *epoch_memcmp == '0');
    tail_service_enabled.store(resolver->tail_pipeline_enabled, std::memory_order_release);
}

void RasterizerVulkan::CommitPendingDraw() {
    if (!pending_commit.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    resolver->WaitResolved();
    if (resolver->pipeline_enabled && !resolver->ResolveInFlight()) {
        return; // a failed head already poisoned and discarded the queue
    }
    if (token_check_enabled) {
        resolver->VerifySnapshot(*maxwell3d);
    }
    DrawResolver::Job& job{resolver->TakeJob()};
    if (resolver->tail_worker_enabled) {
        ASSERT_MSG(job.tail_complete, "DrawToken worker tail must finish before GPU retirement");
        diag_tail_ns += job.tail_ns;
        ++diag_tail_calls;
        // Stage 3 returns producer ownership. Stage 4 only reclaims this slot;
        // dirty carry and producer state remain worker-owned until a full drain.
        resolver->FinishJob(*maxwell3d);
        if (resolver->tail_pipeline_enabled) {
            pending_commit.store(resolver->ResolveInFlight(), std::memory_order_release);
            ++tail_maintenance_pending;
            return;
        }
        gpu.TickWork();
        return;
    }
    Tegra::Engines::Maxwell3D& shadow{resolver->SnapshotEngine()};
    {
        const auto previous_query_snapshot = VideoCommon::tls_pipeline_engine_snapshot;
        if (resolver->pipeline_enabled) {
            VideoCommon::tls_pipeline_engine_snapshot = &shadow;
        }
        SCOPE_EXIT {
            VideoCommon::tls_pipeline_engine_snapshot = previous_query_snapshot;
        };
        // Cache code inside the commit reads the snapshot engine.
        VideoCommon::tls_engine_snapshot = &shadow;
        // Uniform uploads read the resolve-time epoch copy, never the guest
        // (the game CPU may have rewritten it inside the tail-delay window).
        VideoCommon::tls_uniform_epoch = job.epoch_valid ? &job.epoch_snapshot : nullptr;
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        // Consume dirty flags on the shadow so the dirty merge sees what the
        // Touch* family consumed (they operate on whichever flag set the
        // state tracker points at).
        state_tracker.RetargetFlags(shadow.dirty.flags);
        const auto tail_start{std::chrono::steady_clock::now()};
        FinishDrawLocked(shadow, *job.pipeline, job.ctx, job.is_indexed, job.instance_count);
        diag_tail_ns += std::chrono::steady_clock::now() - tail_start;
        ++diag_tail_calls;
        state_tracker.RetargetFlags(maxwell3d->dirty.flags);
        VideoCommon::tls_engine_snapshot = nullptr;
        VideoCommon::tls_uniform_epoch = nullptr;
    }
    resolver->FinishJob(*maxwell3d);
    if (resolver->pipeline_enabled) {
        pending_commit.store(resolver->ResolveInFlight(), std::memory_order_release);
    }
    gpu.TickWork();
}

void RasterizerVulkan::FlushPendingDraw(DrawDrain reason) {
    if (UsesGPUServiceHandoff() && gpu.IsGPUThread() && resolver &&
        resolver->tail_pipeline_enabled) {
        if (gpu.HasRendererFailure()) {
            throw std::runtime_error("DrawToken producer entry after session cancellation");
        }
        const auto index = static_cast<size_t>(reason);
        static_assert(static_cast<size_t>(DrawDrain::Count) == TailPipelineDiag::DrainNames.size());
        ++tail_drain_calls[index];
        const bool nonempty = pending_commit.load(std::memory_order_acquire);
        const auto drain_begin = nonempty ? TailPipelineDiag::Clock::now()
                                         : TailPipelineDiag::Clock::time_point{};
        if (nonempty) {
            ++tail_drains[index];
        }
        while (pending_commit.load(std::memory_order_acquire)) {
            CommitPendingDraw();
            ++tail_drain_jobs[index];
        }
        resolver->ReturnTailOwnership(*maxwell3d);
        if (nonempty) {
            TailPipelineDiag::gpu.Drain(index,
                TailPipelineDiag::Ns(TailPipelineDiag::Clock::now() - drain_begin));
        }
        // Arbitrary GPU requests may mutate producer state. Service only at
        // an explicit handoff, never between reclaiming two queued tails.
        if (tail_maintenance_pending) {
            tail_maintenance_pending = 0;
            gpu.TickWork();
        }
        return;
    }
    // Only the GPU producer consumes tails (including the entire 2A FIFO). Foreign
    // invalidation and resolver callbacks have no draw_owner TLS: they use
    // cache mutexes and must not read/reset/wait on the GPU-owned pending draw.
    while (draw_owner == this && pending_commit.load(std::memory_order_acquire)) {
        CommitPendingDraw();
    }
}

void RasterizerVulkan::PrepareGPUService(GPUServiceReason reason) {
    if (!UsesGPUServiceHandoff()) {
        return;
    }
    if (!gpu.IsGPUThread()) {
        throw std::logic_error("DrawToken external producer must marshal to GPU service thread");
    }
    FlushPendingDraw(reason == GPUServiceReason::Presentation ? DrawDrain::Presentation :
                     reason == GPUServiceReason::Capture ? DrawDrain::Capture : DrawDrain::SyncRequest);
}

bool RasterizerVulkan::AbortGPUService(std::exception_ptr error) {
    if (!UsesGPUServiceHandoff() || !gpu.IsGPUThread()) {
        return false;
    }
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        LOG_ERROR(Render_Vulkan, "DrawToken GPU service failure: {}", e.what());
    } catch (...) {
        LOG_ERROR(Render_Vulkan, "DrawToken GPU service failure: unknown exception");
    }
    if (resolver) {
        resolver->AbortTailPipeline(error); // stops worker before returning producer state
        pending_commit.store(false, std::memory_order_release);
        tail_maintenance_pending = 0;
        resolver->ReturnTailOwnership(*maxwell3d);
    }
    return true;
}

void RasterizerVulkan::WaitForDrawResolve(DrawResolveReason reason) {
    // Resolve completion alone is insufficient: the tail still translates
    // addresses and uploads vertex/index/storage data from guest memory.
    FlushPendingDraw(reason == DrawResolveReason::Map ? DrawDrain::Map :
                     reason == DrawResolveReason::Unmap ? DrawDrain::Unmap : DrawDrain::GuestWrite);
}

void RasterizerVulkan::FinishDrawLocked(Tegra::Engines::Maxwell3D& engine,
                                        GraphicsPipeline& pipeline, DrawContext& ctx,
                                        bool is_indexed, u32 instance_count) {
    // Caller holds buffer_cache.mutex + texture_cache.mutex.
    if (!pipeline.ConfigureTail(ctx, is_indexed)) {
        return;
    }

    UpdateDynamicStates(engine, &pipeline);

    query_cache.NotifySegment(true);
    HandleTransformFeedback(engine);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              engine.regs.zpass_pixel_count_enable);
    RecordDraw(engine, is_indexed, instance_count);
}

void RasterizerVulkan::FinishDrawLockedMeasured(Tegra::Engines::Maxwell3D& engine,
                                               GraphicsPipeline& pipeline, DrawContext& ctx,
                                               bool is_indexed, u32 instance_count) {
    // Stage 4 only. Keep the legacy helper untouched. This is a coarse seam:
    // ConfigureTail includes upload/binding records, NOT pure state-only work.
    if (!pipeline.ConfigureTail(ctx, is_indexed)) {
        return;
    }
    TailPipelineDiag::worker_job.emit_begin = TailPipelineDiag::Clock::now();
    TailPipelineDiag::worker_job.emitted = true;
    UpdateDynamicStates(engine, &pipeline);
    query_cache.NotifySegment(true);
    HandleTransformFeedback(engine);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64,
                              engine.regs.zpass_pixel_count_enable);
    RecordDraw(engine, is_indexed, instance_count);
}

void RasterizerVulkan::RecordDraw(Tegra::Engines::Maxwell3D& engine, bool is_indexed,
                                  u32 instance_count) {
    const auto& draw_state = engine.draw_manager.draw_state;
    const u32 num_instances{instance_count};
    const DrawParams draw_params{MakeDrawParams(draw_state, num_instances, is_indexed)};

    scheduler.Record([draw_params](vk::CommandBuffer cmdbuf) {
        if (draw_params.is_indexed) {
            cmdbuf.DrawIndexed(draw_params.num_vertices, draw_params.num_instances,
                               draw_params.first_index, draw_params.base_vertex,
                               draw_params.base_instance);
        } else {
            cmdbuf.Draw(draw_params.num_vertices, draw_params.num_instances,
                        draw_params.base_vertex, draw_params.base_instance);
        }
    });

    // Log draw call
    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string params = is_indexed ?
            fmt::format("vertices={}, instances={}, firstIndex={}, baseVertex={}, baseInstance={}",
                draw_params.num_vertices, draw_params.num_instances,
                draw_params.first_index, draw_params.base_vertex, draw_params.base_instance) :
            fmt::format("vertices={}, instances={}, firstVertex={}, firstInstance={}",
                draw_params.num_vertices, draw_params.num_instances,
                draw_params.base_vertex, draw_params.base_instance);
        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            is_indexed ? "vkCmdDrawIndexed" : "vkCmdDraw", params, VK_SUCCESS);
    }
}

void RasterizerVulkan::LogTokenDiag(bool force_tail_diag) {
    if (resolver->job_bindings_enabled) {
        LogJobBindingsDiag();
    }
    LOG_INFO(Render_Vulkan,
             "DrawToken diag: pipelined={} "
             "resolve_avg_ns={} tail_avg_ns={} epoch hit/miss/classic={}/{}/{} "
             "slots copy/skip/overflow={}/{}/{}",
             pipelined_draws,
             resolver->diag_resolve_calls
                 ? resolver->diag_resolve_ns.count() / resolver->diag_resolve_calls
                 : 0,
             diag_tail_calls ? diag_tail_ns.count() / diag_tail_calls : 0,
             resolver->tail_pipeline_enabled ? resolver->diag_tail_epoch_hits : buffer_cache.diag_epoch_hits,
             resolver->tail_pipeline_enabled ? resolver->diag_tail_epoch_misses : buffer_cache.diag_epoch_misses,
             resolver->tail_pipeline_enabled ? resolver->diag_tail_epoch_classic : buffer_cache.diag_epoch_classic,
             resolver->epoch_table.diag_copies,
             resolver->epoch_table.diag_skips, resolver->epoch_table.diag_overflows);
    if (diag_draw_indirect_calls) {
        LOG_INFO(Render_Vulkan,
                 "DrawToken diag: indirect_calls={} (byte_count={} count_buffer={})",
                 diag_draw_indirect_calls, diag_draw_indirect_byte_count,
                 diag_draw_indirect_count_buffer);
    }
    if (resolver->batch_enabled) {
        LOG_INFO(Render_Vulkan,
                 "DrawToken diag: capture_batches={} captured_bytes={} splice_count={}",
                 resolver->diag_capture_batches, resolver->diag_captured_bytes,
                 resolver->diag_splice_count);
    }
    if (resolver->worker_enabled) {
        LOG_INFO(Render_Vulkan,
                 "DrawToken diag: diag_worker_resolves={} diag_worker_sync_requests={}",
                 resolver->diag_worker_resolves, resolver->diag_worker_sync_requests);
    }
    if (resolver->pipeline_enabled) {
        if (resolver->tail_pipeline_enabled &&
            (force_tail_diag || pipelined_draws - tail_diag_last >= 65536)) {
            tail_diag_last = pipelined_draws;
            resolver->LogTailPipelineDiag();
            static constexpr std::array names{"other", "flush_caching", "guest_write", "map",
                "unmap", "cold_pipeline", "submit", "indirect", "fallback", "channel", "teardown",
                "invalidation", "sync_request", "download", "presentation", "capture",
                "draw_texture", "clear", "dispatch_compute", "reset_counter", "query_counter",
                "uniform_bind", "signal_sync", "cond_render", "surface_copy", "inline_to_memory",
                "must_flush", "flush_area", "modify_gpu_mem", "release_fences", "flush_invalidate",
                "wait_for_idle", "fragment_barrier", "tiled_cache_barrier", "flush_commands",
                "tick_frame", "access_dma", "accel_display"};
            for (size_t i = 0; i < names.size(); ++i) {
                LOG_INFO(Render_Vulkan, "DrawToken tail drain: reason={} calls={} drains={} jobs={}",
                         names[i], tail_drain_calls[i], tail_drains[i], tail_drain_jobs[i]);
            }
        }
        LOG_INFO(Render_Vulkan,
                 "DrawToken diag: diag_pipeline_snapshot_ns={} diag_pipeline_snapshot_count={} "
                 "pipeline_snapshot_avg_ns={} pipeline_resyncs={} pipeline_replayed_entries={}",
                 resolver->diag_pipeline_snapshot_ns.count(), resolver->diag_pipeline_snapshot_count,
                 resolver->diag_pipeline_snapshot_count
                     ? resolver->diag_pipeline_snapshot_ns.count() /
                           resolver->diag_pipeline_snapshot_count
                     : 0,
                 resolver->diag_resyncs, resolver->diag_journal_entries);
        LOG_INFO(Render_Vulkan,
                 "DrawToken diag: diag_pipeline_resolves={} diag_pipeline_max_inflight={} "
                 "diag_pipeline_spin_wins={} diag_pipeline_parks={} diag_pipeline_backpressure={} "
                 "worker_spin_wins={} worker_parks={} mismatches={}",
                 resolver->diag_pipeline_resolves, resolver->diag_pipeline_max_inflight,
                 resolver->diag_pipeline_spin_wins, resolver->diag_pipeline_parks,
                 resolver->diag_pipeline_backpressure,
                 resolver->diag_pipeline_worker_spin_wins.load(std::memory_order_relaxed),
                 resolver->diag_pipeline_worker_parks.load(std::memory_order_relaxed),
                 resolver->diag_snapshot_mismatches);
    }
}

void RasterizerVulkan::DrawPipelined(bool is_indexed, u32 instance_count) {
    resolver->PollResolveSync();
    if (resolver->PipelineFull()) {
        ++resolver->diag_pipeline_backpressure;
        CommitPendingDraw(); // retire exactly the oldest tail, before enqueue
    }
    // Pipeline selection can synchronize guest memory/cache state. It cannot
    // run against a resolver holding B/T and waiting for GPU bridge service.
    // Younger queued snapshots are NOT armed until the final Kick below.
    resolver->PreparePipelineEnqueue();
#ifdef __ANDROID__
    constexpr u32 flush_interval = 512;
#else
    constexpr u32 flush_interval = 4096;
#endif
    if (draw_counter >= flush_interval - 1) {
        FlushPendingDraw(); // a submit must follow every older queued tail
    }
    FlushWork(); // preserve ordinary periodic DispatchWork; worker is quiescent
    gpu_memory->FlushCaching(); // invalidation callback drains ALL queued tails
    GraphicsPipeline* const pipeline = pipeline_cache.CurrentGraphicsPipeline();
    if (!pipeline) {
        return;
    }
    if (!resolver->SnapshotAndEnqueue(*maxwell3d, pipeline, is_indexed, instance_count)) {
        ++fallback_draws;
        ++resolver->diag_fallbacks;
        // PrepareDraw drains the queue and reselects after any barrier effects.
        PrepareDraw(is_indexed, [this, is_indexed, instance_count] {
            RecordDraw(*maxwell3d, is_indexed, instance_count);
        });
        return;
    }
    resolver->ExecuteResolve(scheduler); // Kick only; normal consumption is later
    pending_commit.store(true, std::memory_order_release);
    ++pipelined_draws;
    if (token_tail_immediate) {
        FlushPendingDraw(); // diagnostic mode explicitly requests immediate tails
    }
    if (pipelined_draws % 2000 == 0) {
        LogTokenDiag(); // pipeline aggregates are GPU-owned; no worker data race
    }
}

void RasterizerVulkan::DrawTailPipelined(bool is_indexed, u32 instance_count) {
    auto& timing = TailPipelineDiag::gpu;
    ++timing.draws;
    const auto poll_begin = TailPipelineDiag::Clock::now();
    resolver->PollResolveSync();
    while (resolver->TailFrontReady()) {
        CommitPendingDraw(); // publication/reclaim only; worker advances independently
    }
    auto parse_begin = TailPipelineDiag::Clock::now();
    timing.poll_ns += TailPipelineDiag::Ns(parse_begin - poll_begin);
    ++timing.poll_blocks;
    if (resolver->PipelineFull()) {
        ++resolver->diag_pipeline_backpressure;
        CommitPendingDraw();
        const auto capacity_end = TailPipelineDiag::Clock::now();
        timing.Capacity(TailPipelineDiag::Ns(capacity_end - parse_begin));
        parse_begin = capacity_end;
    }
    {
        const u64 drains_before = timing.all_drain_ns;
        SCOPE_EXIT {
            timing.parse_ns += TailPipelineDiag::Ns(TailPipelineDiag::Clock::now() - parse_begin);
            timing.parse_drain_ns += timing.all_drain_ns - drains_before;
        };
#ifdef __ANDROID__
        constexpr u32 flush_interval = 512;
#else
        constexpr u32 flush_interval = 4096;
#endif
        if (draw_counter >= flush_interval - 1) {
            FlushPendingDraw(DrawDrain::Submit);
        }
        FlushWork(); // dispatch is publication-only; submit above requires handoff
        gpu_memory->FlushCaching(); // real invalidations still synchronously drain
        auto* pipeline = pipeline_cache.TryGraphicsPipelineForParser();
        if (!pipeline) {
            FlushPendingDraw(DrawDrain::ColdPipeline);
            pipeline = pipeline_cache.CurrentGraphicsPipeline();
        }
        if (!pipeline) {
            return;
        }
        if (!resolver->SnapshotAndEnqueue(*maxwell3d, pipeline, is_indexed, instance_count)) {
            ++fallback_draws;
            ++resolver->diag_fallbacks;
            FlushPendingDraw(DrawDrain::Fallback);
            PrepareDraw(is_indexed, [this, is_indexed, instance_count] {
                RecordDraw(*maxwell3d, is_indexed, instance_count);
            });
            return;
        }
        resolver->ExecuteResolve(scheduler);
        pending_commit.store(true, std::memory_order_release);
        ++pipelined_draws;
        ++timing.admitted;
    }
    if (token_tail_immediate) {
        FlushPendingDraw();
    }
    if (pipelined_draws % 2000 == 0) {
        LogTokenDiag();
    }
}

void RasterizerVulkan::Draw(bool is_indexed, u32 instance_count) {
    ++diag_frame_draws;
    if (token_mode == TokenMode::Off) {
        PrepareDraw(is_indexed, [this, is_indexed, instance_count] {
            RecordDraw(*maxwell3d, is_indexed, instance_count);
        });
        return;
    }
    draw_owner = this;
    EnsureResolver();
    if (resolver->tail_pipeline_enabled) {
        DrawTailPipelined(is_indexed, instance_count);
        return;
    }
    if (resolver->pipeline_enabled) {
        DrawPipelined(is_indexed, instance_count);
        return;
    }

    // Commit the previous token draw, keeping draw order.
    CommitPendingDraw();

    FlushWork();
    // Commit before flushing invalidations that can change draw resources.
    gpu_memory->FlushCaching();

    GraphicsPipeline* const pipeline{pipeline_cache.CurrentGraphicsPipeline()};
    if (!pipeline) {
        return;
    }
    if (resolver->SnapshotAndEnqueue(*maxwell3d, pipeline, is_indexed, instance_count)) {
        if (resolver->tail_worker_enabled) {
            resolver->ExecuteResolve(scheduler); // resolve + tail, immediate bridge-aware wait
            // Publish only after all worker callbacks returned; never expose a
            // half-completed job to a reentrant guest-memory invalidation.
            pending_commit.store(true, std::memory_order_release);
            CommitPendingDraw(); // GPU retirement only; no second tail
            ++pipelined_draws;
            if (pipelined_draws % 2000 == 0) {
                LogTokenDiag();
            }
            return;
        }
        if (token_tail_immediate) {
            // Bisect mode: snapshot + resolve + commit all inside Draw(),
            // structurally identical to the serial path plus the shadow.
            resolver->ExecuteResolve(scheduler);
            DrawResolver::Job& job{resolver->TakeJob()};
            Tegra::Engines::Maxwell3D& shadow{resolver->SnapshotEngine()};
            {
                VideoCommon::tls_engine_snapshot = &shadow;
                VideoCommon::tls_uniform_epoch =
                    job.epoch_valid ? &job.epoch_snapshot : nullptr;
                std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
                state_tracker.RetargetFlags(shadow.dirty.flags);
                // Mirrors CommitPendingDraw's tail chrono (FinishDrawLocked
                // only) so tail_avg_ns means the same thing in both paths.
                const auto tail_start{std::chrono::steady_clock::now()};
                FinishDrawLocked(shadow, *job.pipeline, job.ctx, job.is_indexed,
                                 job.instance_count);
                diag_tail_ns += std::chrono::steady_clock::now() - tail_start;
                ++diag_tail_calls;
                state_tracker.RetargetFlags(maxwell3d->dirty.flags);
                VideoCommon::tls_engine_snapshot = nullptr;
                VideoCommon::tls_uniform_epoch = nullptr;
            }
            resolver->FinishJob(*maxwell3d);
            gpu.TickWork();
            ++pipelined_draws;
            if (pipelined_draws % 2000 == 0) {
                LogTokenDiag();
            }
            return;
        }
        // Resolve may call back into the rasterizer while synchronizing guest
        // memory. Publish only after it returns (including the 1B rendezvous),
        // otherwise a callback can try to commit a still-resolving job.
        resolver->ExecuteResolve(scheduler);
        pending_commit.store(true, std::memory_order_release);
        ++pipelined_draws;
        if (pipelined_draws % 2000 == 0) {
            LogTokenDiag();
        }
        return;
    }
    ++fallback_draws;
    resolver->diag_fallbacks = fallback_draws;
    // Synchronous draw (e.g. inline index buffer); engine state is live.
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    pipeline->SetEngine(maxwell3d, gpu_memory);
    draw_ctx.Reset(maxwell3d, gpu_memory);
    if (resolver && resolver->job_bindings_enabled) {
        resolver->ApplyPendingUniformInputs();
    }
    pipeline->ConfigureResolve(draw_ctx, is_indexed);
    FinishDrawLocked(*maxwell3d, *pipeline, draw_ctx, is_indexed, instance_count);
    gpu.TickWork();
}

void RasterizerVulkan::DrawIndirect() {
    ++diag_frame_draws;
    ++diag_draw_indirect_calls;
    FlushPendingDraw(DrawDrain::Indirect);
    const auto& params = maxwell3d->draw_manager.indirect_state;
    diag_draw_indirect_byte_count += params.is_byte_count;
    diag_draw_indirect_count_buffer += params.include_count;
    buffer_cache.SetDrawIndirect(&params);
    PrepareDraw(params.is_indexed, [this, &params] {
        const auto indirect_buffer = buffer_cache.GetDrawIndirectBuffer();
        const auto& buffer = indirect_buffer.first;
        const auto& offset = indirect_buffer.second;
        if (params.is_byte_count) {
            scheduler.Record([buffer_obj = buffer->Handle(), offset,
                              stride = params.stride](vk::CommandBuffer cmdbuf) {
                cmdbuf.DrawIndirectByteCountEXT(1, 0, buffer_obj, offset, 0,
                                                static_cast<u32>(stride));
            });
            return;
        }
        if (params.include_count) {
            const auto count = buffer_cache.GetDrawIndirectCount();
            const auto& draw_buffer = count.first;
            const auto& offset_base = count.second;
            scheduler.Record([draw_buffer_obj = draw_buffer->Handle(),
                              buffer_obj = buffer->Handle(), offset_base, offset,
                              params](vk::CommandBuffer cmdbuf) {
                if (params.is_indexed) {
                    cmdbuf.DrawIndexedIndirectCount(
                        buffer_obj, offset, draw_buffer_obj, offset_base,
                        static_cast<u32>(params.max_draw_counts), static_cast<u32>(params.stride));
                } else {
                    cmdbuf.DrawIndirectCount(buffer_obj, offset, draw_buffer_obj, offset_base,
                                             static_cast<u32>(params.max_draw_counts),
                                             static_cast<u32>(params.stride));
                }
            });
            return;
        }
        scheduler.Record([buffer_obj = buffer->Handle(), offset, params](vk::CommandBuffer cmdbuf) {
            if (params.is_indexed) {
                cmdbuf.DrawIndexedIndirect(buffer_obj, offset,
                                           static_cast<u32>(params.max_draw_counts),
                                           static_cast<u32>(params.stride));
            } else {
                cmdbuf.DrawIndirect(buffer_obj, offset, static_cast<u32>(params.max_draw_counts),
                                    static_cast<u32>(params.stride));
            }
        });

        // Log indirect draw call
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            const std::string log_params = fmt::format("drawCount={}, stride={}",
                params.max_draw_counts, params.stride);
            GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                params.is_indexed ? "vkCmdDrawIndexedIndirect" : "vkCmdDrawIndirect",
                log_params, VK_SUCCESS);
        }
    });
    buffer_cache.SetDrawIndirect(nullptr);
}

void RasterizerVulkan::DrawTexture() {
    FlushPendingDraw(DrawDrain::DrawTexture);

    SCOPE_EXIT {
        gpu.TickWork();
    };
    FlushWork();

    std::scoped_lock l{texture_cache.mutex};
    texture_cache.SynchronizeDescriptors(false);
    texture_cache.UpdateRenderTargets(false);

    UpdateDynamicStates(*maxwell3d, pipeline_cache.CurrentGraphicsPipeline());

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    const auto& draw_texture_state = maxwell3d->draw_manager.draw_texture_state;
    const auto& sampler = texture_cache.GetSampler(draw_texture_state.src_sampler, false);
    const auto& texture = texture_cache.GetImageView(draw_texture_state.src_texture);
    const auto* framebuffer = texture_cache.GetFramebuffer();

    const bool src_rescaling = texture_cache.IsRescaling() && texture.IsRescaled();
    const bool dst_rescaling = texture_cache.IsRescaling() && framebuffer->IsRescaled();

    const auto ScaleSrc = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return src_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    const auto ScaleDst = [&](auto dim_f) -> s32 {
        auto dim = static_cast<s32>(dim_f);
        return dst_rescaling ? Settings::values.resolution_info.ScaleUp(dim) : dim;
    };

    Region2D dst_region = {Offset2D{.x = ScaleDst(draw_texture_state.dst_x0),
                                    .y = ScaleDst(draw_texture_state.dst_y0)},
                           Offset2D{.x = ScaleDst(draw_texture_state.dst_x1),
                                    .y = ScaleDst(draw_texture_state.dst_y1)}};
    Region2D src_region = {Offset2D{.x = ScaleSrc(draw_texture_state.src_x0),
                                    .y = ScaleSrc(draw_texture_state.src_y0)},
                           Offset2D{.x = ScaleSrc(draw_texture_state.src_x1),
                                    .y = ScaleSrc(draw_texture_state.src_y1)}};
    Extent3D src_size = {static_cast<u32>(ScaleSrc(texture.size.width)),
                         static_cast<u32>(ScaleSrc(texture.size.height)), texture.size.depth};
    blit_image.BlitColor(framebuffer, texture.RenderTarget(), texture.ImageHandle(),
                         sampler->Handle(), dst_region, src_region, src_size);
}

void RasterizerVulkan::Clear(u32 layer_count) {
    FlushPendingDraw(DrawDrain::Clear);
    FlushWork();
    gpu_memory->FlushCaching();

    auto& regs = maxwell3d->regs;
    const bool use_color = regs.clear_surface.R || regs.clear_surface.G || regs.clear_surface.B ||
                           regs.clear_surface.A;
    const bool use_depth = regs.clear_surface.Z;
    const bool use_stencil = regs.clear_surface.S;
    if (!use_color && !use_depth && !use_stencil) {
        return;
    }

    std::scoped_lock lock{texture_cache.mutex};
    texture_cache.UpdateRenderTargets(true);
    const Framebuffer* const framebuffer = texture_cache.GetFramebuffer();
    const VkExtent2D render_area = framebuffer->RenderArea();

    constexpr bool ENABLE_DEFERRED_CLEAR = true;
    const bool color_full_channels = regs.clear_surface.R && regs.clear_surface.G &&
                                     regs.clear_surface.B && regs.clear_surface.A;
    const bool stencil_partial = use_stencil && framebuffer->HasAspectStencilBit() &&
                                 regs.stencil_front_mask != 0xFF && regs.stencil_front_mask != 0;
    const bool ds_used = use_depth || use_stencil;
    const bool ds_deferrable =
        !ds_used || ((!framebuffer->HasAspectDepthBit() || use_depth) &&
                     (!framebuffer->HasAspectStencilBit() || use_stencil) && !stencil_partial);
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    VkRect2D default_scissor{};
    default_scissor.offset.x = 0;
    default_scissor.offset.y = 0;
    default_scissor.extent.width = (std::numeric_limits<s32>::max)();
    default_scissor.extent.height = (std::numeric_limits<s32>::max)();

    VkClearRect clear_rect{
        .rect = regs.clear_control.use_scissor ? GetScissorState(regs, 0, up_scale, down_shift)
                                               : default_scissor,
        .baseArrayLayer = regs.clear_surface.layer,
        .layerCount = layer_count,
    };
    const auto clamp_rect_to_render_area = [render_area](VkRect2D& rect) -> bool {
        const auto clamp_axis = [](s32& offset, u32& extent, u32 limit) {
            auto clamp_offset = [&offset, limit]() {
                if (limit == 0) {
                    offset = 0;
                    return;
                }
                offset = std::clamp(offset, 0, static_cast<s32>(limit));
            };

            if (extent == 0) {
                clamp_offset();
                return;
            }
            if (offset < 0) {
                const u32 shrink = (std::min)(extent, static_cast<u32>(-offset));
                extent -= shrink;
                offset = 0;
            }
            if (limit == 0) {
                extent = 0;
                offset = 0;
                return;
            }
            if (offset >= s32(limit)) {
                offset = s32(limit);
                extent = 0;
                return;
            }
            const u64 end_coord = u64(offset) + extent;
            if (end_coord > limit) {
                extent = limit - u32(offset);
            }
        };

        clamp_axis(rect.offset.x, rect.extent.width, render_area.width);
        clamp_axis(rect.offset.y, rect.extent.height, render_area.height);
        return rect.extent.width != 0 && rect.extent.height != 0;
    };
    if (!clamp_rect_to_render_area(clear_rect.rect)) {
        return;
    }

    const bool clear_covers_render_area =
        clear_rect.rect.offset.x == 0 && clear_rect.rect.offset.y == 0 &&
        clear_rect.rect.extent.width >= render_area.width &&
        clear_rect.rect.extent.height >= render_area.height;
    const bool can_defer_clear = ENABLE_DEFERRED_CLEAR && (!regs.clear_control.use_scissor || clear_covers_render_area) &&
                                 regs.clear_surface.layer == 0 &&
                                 !scheduler.IsRenderPassActive() &&
                                 (!use_color || color_full_channels) && ds_deferrable;
    if (!can_defer_clear) {
        scheduler.RequestRenderpass(framebuffer);
    }

    query_cache.NotifySegment(true);
    query_cache.CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, maxwell3d->regs.zpass_pixel_count_enable);
    UpdateViewportsState(*maxwell3d);

    const u32 color_attachment = regs.clear_surface.RT;
    if (use_color && framebuffer->HasAspectColorBit(color_attachment)) {
        const auto format = VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[color_attachment].format);
        bool is_integer = IsPixelFormatInteger(format);
        bool is_signed = IsPixelFormatSignedInteger(format);
        size_t int_size = PixelComponentSizeBitsInteger(format);
        VkClearValue clear_value{};
        if (!is_integer) {
            std::memcpy(clear_value.color.float32, regs.clear_color.data(), regs.clear_color.size() * sizeof(f32));
        } else if (!is_signed) {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.uint32[i] = u32(f32(u64(int_size) << 1U) * regs.clear_color[i]);
        } else {
            for (size_t i = 0; i < 4; i++)
                clear_value.color.int32[i] = s32(f32(s64(int_size - 1) << 1) * (regs.clear_color[i] - 0.5f));
        }

        if (color_full_channels) {
            if (can_defer_clear) {
                scheduler.DeferColorClear(framebuffer, color_attachment, clear_value);
            } else {
                scheduler.Record([color_attachment, clear_value, clear_rect](vk::CommandBuffer cmdbuf) {
                    const VkClearAttachment attachment{
                        .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                        .colorAttachment = color_attachment,
                        .clearValue = clear_value,
                    };
                    cmdbuf.ClearAttachments(attachment, clear_rect);
                });
            }
        } else {
            u8 color_mask = u8(regs.clear_surface.R | regs.clear_surface.G << 1 | regs.clear_surface.B << 2 | regs.clear_surface.A << 3);
            Region2D dst_region = {
                Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
                Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                         .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
            blit_image.ClearColor(framebuffer, color_mask, regs.clear_color, dst_region);
        }
    }

    if (!use_depth && !use_stencil) {
        return;
    }
    VkImageAspectFlags aspect_flags = 0;
    if (use_depth && framebuffer->HasAspectDepthBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_DEPTH_BIT;
    }
    if (use_stencil && framebuffer->HasAspectStencilBit()) {
        aspect_flags |= VK_IMAGE_ASPECT_STENCIL_BIT;
    }
    if (aspect_flags == 0) {
        return;
    }

    if (use_stencil && framebuffer->HasAspectStencilBit() && regs.stencil_front_mask != 0xFF &&
        regs.stencil_front_mask != 0) {
        Region2D dst_region = {
            Offset2D{.x = clear_rect.rect.offset.x, .y = clear_rect.rect.offset.y},
            Offset2D{.x = clear_rect.rect.offset.x + s32(clear_rect.rect.extent.width),
                     .y = clear_rect.rect.offset.y + s32(clear_rect.rect.extent.height)}};
        blit_image.ClearDepthStencil(framebuffer, use_depth, regs.clear_depth,
                                     u8(regs.stencil_front_mask), regs.clear_stencil,
                                     regs.stencil_front_func_mask, dst_region);
    } else if (can_defer_clear) {
        VkClearValue ds_value{};
        ds_value.depthStencil.depth = regs.clear_depth;
        ds_value.depthStencil.stencil = regs.clear_stencil;
        scheduler.DeferDepthStencilClear(framebuffer, ds_value);
    } else {
        scheduler.Record([clear_depth = regs.clear_depth, clear_stencil = regs.clear_stencil,
                          clear_rect, aspect_flags](vk::CommandBuffer cmdbuf) {
            VkClearAttachment attachment;
            attachment.aspectMask = aspect_flags;
            attachment.colorAttachment = 0;
            attachment.clearValue.depthStencil.depth = clear_depth;
            attachment.clearValue.depthStencil.stencil = clear_stencil;
            cmdbuf.ClearAttachments(attachment, clear_rect);
        });
    }
}

void RasterizerVulkan::DispatchCompute() {
    FlushPendingDraw(DrawDrain::DispatchCompute);
    FlushWork();
    gpu_memory->FlushCaching();

    ComputePipeline* const pipeline{pipeline_cache.CurrentComputePipeline()};
    if (!pipeline) {
        return;
    }
    std::scoped_lock lock{texture_cache.mutex, buffer_cache.mutex};
    if (!pipeline->Configure(*kepler_compute, *gpu_memory, scheduler, buffer_cache,
                             texture_cache)) {
        return;
    }

    const auto& qmd{kepler_compute->launch_description};
    auto indirect_address = kepler_compute->GetIndirectComputeAddress();
    if (indirect_address) {
        // DispatchIndirect
        static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
        const auto post_op = VideoCommon::ObtainBufferOperation::DiscardWrite;
        const auto [buffer, offset] =
            buffer_cache.ObtainBuffer(*indirect_address, 12, sync_info, post_op);
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([pipeline, indirect_buffer = buffer->Handle(),
                          indirect_offset = offset](vk::CommandBuffer cmdbuf) {
            if (!pipeline->IsBound()) {
                return;
            }
            cmdbuf.DispatchIndirect(indirect_buffer, indirect_offset);
        });
        return;
    }
    const std::array<u32, 3> dim{qmd.grid_dim_x, qmd.grid_dim_y, qmd.grid_dim_z};
    const std::array<u32, 3> max_dim{device.GetMaxComputeWorkGroupCount()};
    if (dim[0] > max_dim[0] || dim[1] > max_dim[1] || dim[2] > max_dim[2]) {
        return;
    }
    scheduler.RequestOutsideRenderPassOperationContext();
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
    };
    scheduler.Record([](vk::CommandBuffer cmdbuf) { cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               0, READ_BARRIER); });
    scheduler.Record([pipeline, dim](vk::CommandBuffer cmdbuf) {
        if (!pipeline->IsBound()) {
            return;
        }
        cmdbuf.Dispatch(dim[0], dim[1], dim[2]);
    });

    // Log compute dispatch
    if (GPU::Logging::IsActive() &&
        Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string params = fmt::format("groupCountX={}, groupCountY={}, groupCountZ={}",
            dim[0], dim[1], dim[2]);
        GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
            "vkCmdDispatch", params, VK_SUCCESS);
    }
}

void RasterizerVulkan::ResetCounter(VideoCommon::QueryType type) {
    FlushPendingDraw(DrawDrain::ResetCounter);
    switch (type) {
    case VideoCommon::QueryType::ZPassPixelCount64:
    case VideoCommon::QueryType::StreamingByteCount:
    case VideoCommon::QueryType::StreamingPrimitivesSucceeded:
    case VideoCommon::QueryType::VtgPrimitivesOut:
        query_cache.CounterReset(type);
        return;
    default:
        LOG_DEBUG(Render_Vulkan, "Unimplemented counter reset={}", type);
        return;
    }
}

void RasterizerVulkan::Query(GPUVAddr gpu_addr, VideoCommon::QueryType type,
                             VideoCommon::QueryPropertiesFlags flags, u32 payload, u32 subreport) {
    FlushPendingDraw(DrawDrain::QueryCounter);
    query_cache.CounterReport(gpu_addr, type, flags, payload, subreport);
}

void RasterizerVulkan::BindGraphicsUniformBuffer(size_t stage, u32 index, GPUVAddr gpu_addr,
                                                 u32 size) {
    if (resolver && resolver->job_bindings_enabled) {
        // Parser input only. Do not wait for or overwrite an older draw's bindings.
        resolver->BindUniformInput(stage, index, gpu_addr, size);
        return;
    }
    FlushPendingDraw(DrawDrain::UniformBind);
    buffer_cache.BindGraphicsUniformBuffer(stage, index, gpu_addr, size);
}

void Vulkan::RasterizerVulkan::DisableGraphicsUniformBuffer(size_t stage, u32 index) {
    if (resolver && resolver->job_bindings_enabled) {
        resolver->DisableUniformInput(stage, index);
        return;
    }
    FlushPendingDraw(DrawDrain::UniformBind);
    buffer_cache.DisableGraphicsUniformBuffer(stage, index);
}

void RasterizerVulkan::FlushAll() {}

void RasterizerVulkan::FlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (UsesGPUServiceHandoff() && !VideoCommon::tls_engine_snapshot && !gpu.IsGPUThread()) {
        gpu.RunGPUService([this, addr, size, which] { FlushRegion(addr, size, which); });
        return;
    }
    FlushPendingDraw(DrawDrain::Download);
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.DownloadMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.FlushRegion(addr, size);
    }
}

bool RasterizerVulkan::MustFlushRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    if (UsesGPUServiceHandoff()) {
        using Result = TailPipelineDiag::MustFlush::Result;
        auto& diag = TailPipelineDiag::must_flush;
        const bool buffers = True(which & VideoCommon::CacheType::BufferCache);
        const bool textures = Settings::IsGPULevelHigh() &&
                              True(which & VideoCommon::CacheType::TextureCache);
        if (!buffers && !textures) {
            return diag.Record(Result::Ignored);
        }
        // Foreign callers (including resolver callbacks) must not inspect GPU
        // producer-owned queue positions or wait while holding cache locks.
        // Actual downloads still take FlushRegion's GPU-service handoff/drain.
        if (!gpu.IsGPUThread()) {
            return diag.Record(Result::Foreign);
        }
        // SSBO/image-buffer/XFB dirty marks are installed by the tail, not at
        // enqueue. Without a complete pending write footprint, an unfinished
        // tail can affect any queried region: conservatively answer true.
        if (resolver && resolver->HasUnexecutedTailWrites()) {
            return diag.Record(Result::Pending);
        }
        // All enqueued tails have executed. Their dirty marks are visible even
        // before command publication; no new enqueue can race this GPU caller.
        if (buffers) {
            bool dirty;
            {
                std::scoped_lock lock{buffer_cache.mutex};
                dirty = buffer_cache.IsRegionGpuModified(addr, size);
            }
            if (dirty) {
                return diag.Record(Result::Dirty);
            }
        }
        if (textures) {
            bool dirty;
            {
                std::scoped_lock lock{texture_cache.mutex};
                dirty = texture_cache.IsRegionGpuModified(addr, size);
            }
            if (dirty) {
                return diag.Record(Result::Dirty);
            }
        }
        return diag.Record(Result::Clean);
    }
    FlushPendingDraw(DrawDrain::MustFlush);
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.IsRegionGpuModified(addr, size)) {
            return true;
        }
    }
    if (!Settings::IsGPULevelHigh()) {
        return false;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        return texture_cache.IsRegionGpuModified(addr, size);
    }
    return false;
}

VideoCore::RasterizerDownloadArea RasterizerVulkan::GetFlushArea(DAddr addr, u64 size) {
    FlushPendingDraw(DrawDrain::FlushArea);
    {
        std::scoped_lock lock{texture_cache.mutex};
        auto area = texture_cache.GetFlushArea(addr, size);
        if (area) {
            return *area;
        }
    }
    VideoCore::RasterizerDownloadArea new_area{
        .start_address = Common::AlignDown(addr, Core::DEVICE_PAGESIZE),
        .end_address = Common::AlignUp(addr + size, Core::DEVICE_PAGESIZE),
        .preemtive = true,
    };
    return new_area;
}

void RasterizerVulkan::InvalidateRegion(DAddr addr, u64 size, VideoCommon::CacheType which) {
    FlushPendingDraw(DrawDrain::Invalidation);
    if (addr == 0 || size == 0) {
        return;
    }
    if (True(which & VideoCommon::CacheType::TextureCache)) {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::BufferCache))) {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::QueryCache))) {
        query_cache.InvalidateRegion(addr, size);
    }
    if ((True(which & VideoCommon::CacheType::ShaderCache))) {
        pipeline_cache.InvalidateRegion(addr, size);
    }
}

void RasterizerVulkan::InnerInvalidation(std::span<const std::pair<DAddr, std::size_t>> sequences) {
    FlushPendingDraw(DrawDrain::FlushCaching);
    {
        std::scoped_lock lock{texture_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            texture_cache.WriteMemory(addr, size);
        }
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        for (const auto& [addr, size] : sequences) {
            buffer_cache.WriteMemory(addr, size);
        }
    }
    {
        for (const auto& [addr, size] : sequences) {
            query_cache.InvalidateRegion(addr, size);
            pipeline_cache.InvalidateRegion(addr, size);
        }
    }
}

bool RasterizerVulkan::OnCPUWrite(DAddr addr, u64 size) {
    FlushPendingDraw(DrawDrain::Invalidation);
    DEBUG_ASSERT(addr != 0 || size != 0);
    {
        std::scoped_lock lock{buffer_cache.mutex};
        if (buffer_cache.OnCPUWrite(addr, size)) {
            return true;
        }
    }
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
    return false;
}

void RasterizerVulkan::OnCacheInvalidation(DAddr addr, u64 size) {
    FlushPendingDraw(DrawDrain::Invalidation);
    if (addr == 0 || size == 0) {
        return;
    }

    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.WriteMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.InvalidateRegion(addr, size);
}

void RasterizerVulkan::InvalidateGPUCache() {
    FlushPendingDraw(DrawDrain::Invalidation);
    gpu.InvalidateGPUCache();
}

void RasterizerVulkan::UnmapMemory(DAddr addr, u64 size) {
    if (UsesGPUServiceHandoff() && !VideoCommon::tls_engine_snapshot && !gpu.IsGPUThread()) {
        gpu.RunGPUService([this, addr, size] { UnmapMemory(addr, size); });
        return;
    }
    FlushPendingDraw(DrawDrain::Invalidation);
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapMemory(addr, size);
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.WriteMemory(addr, size);
    }
    pipeline_cache.OnCacheInvalidation(addr, size);
}

void RasterizerVulkan::ModifyGPUMemory(size_t as_id, GPUVAddr addr, u64 size) {
    if (UsesGPUServiceHandoff() && !VideoCommon::tls_engine_snapshot && !gpu.IsGPUThread()) {
        gpu.RunGPUService([this, as_id, addr, size] { ModifyGPUMemory(as_id, addr, size); });
        return;
    }
    FlushPendingDraw(DrawDrain::ModifyGpuMem);
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.UnmapGPUMemory(as_id, addr, size);
    }
}

void RasterizerVulkan::SignalFence(std::function<void()>&& func) {
    FlushPendingDraw(DrawDrain::SignalSync);
    fence_manager.SignalFence(std::move(func));
}

void RasterizerVulkan::SyncOperation(std::function<void()>&& func) {
    FlushPendingDraw(DrawDrain::SignalSync);
    fence_manager.SyncOperation(std::move(func));
}

void RasterizerVulkan::SignalSyncPoint(u32 value) {
    FlushPendingDraw(DrawDrain::SignalSync);
    fence_manager.SignalSyncPoint(value);
}

void RasterizerVulkan::SignalReference() {
    FlushPendingDraw(DrawDrain::SignalSync);
    fence_manager.SignalReference();
}

void RasterizerVulkan::ReleaseFences(bool force) {
    FlushPendingDraw(DrawDrain::ReleaseFences);
    fence_manager.WaitPendingFences(force);
}

void RasterizerVulkan::FlushAndInvalidateRegion(DAddr addr, u64 size,
                                                VideoCommon::CacheType which) {
    FlushPendingDraw(DrawDrain::FlushInvalidate);
    if (Settings::IsGPULevelHigh()) {
        FlushRegion(addr, size, which);
    }
    InvalidateRegion(addr, size, which);
}

void RasterizerVulkan::WaitForIdle() {
    FlushPendingDraw(DrawDrain::WaitForIdle);
    // Everything but wait pixel operations. This intentionally includes FRAGMENT_SHADER_BIT because
    // fragment shaders can still write storage buffers.
    VkPipelineStageFlags flags =
        VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT | VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
        VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TESSELLATION_CONTROL_SHADER_BIT |
        VK_PIPELINE_STAGE_TESSELLATION_EVALUATION_SHADER_BIT |
        VK_PIPELINE_STAGE_GEOMETRY_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
        VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT;
    }

    query_cache.NotifyWFI();

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([event = *wfi_event, flags](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetEvent(event, flags);
        cmdbuf.WaitEvents(event, flags, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, {}, {}, {});
    });
    fence_manager.SignalOrdering();
}

void RasterizerVulkan::FragmentBarrier() {
    FlushPendingDraw(DrawDrain::FragmentBarrier);
    // We already put barriers when a render pass finishes
    scheduler.RequestOutsideRenderPassOperationContext();
}

void RasterizerVulkan::TiledCacheBarrier() {
    FlushPendingDraw(DrawDrain::TiledCacheBarrier);
    // TODO: Implementing tiled barriers requires rewriting a good chunk of the Vulkan backend
}

void RasterizerVulkan::FlushCommands() {
    FlushPendingDraw(DrawDrain::FlushCommands);
    if (draw_counter == 0) {
        return;
    }
    draw_counter = 0;
    scheduler.Flush();
}

void RasterizerVulkan::TickFrame() {
    // draw_counter is a dispatch budget, reset mid-frame by FlushWork/FlushCommands.
    diag_frame_total_draws += diag_frame_draws;
    diag_frame_max_draws = (std::max)(diag_frame_max_draws, diag_frame_draws);
    diag_frame_draws = 0;
    if (++diag_frames % 500 == 0) {
        LOG_INFO(Render_Vulkan,
                 "SerialFrame diag: frames={} draws={} draws_per_frame_avg={} draws_per_frame_max={}",
                 diag_frames, diag_frame_total_draws,
                 static_cast<double>(diag_frame_total_draws) / diag_frames, diag_frame_max_draws);
    }
    FlushPendingDraw(DrawDrain::TickFrame);
    draw_counter = 0;
    guest_descriptor_queue.TickFrame();
    compute_pass_descriptor_queue.TickFrame();
    descriptor_buffer_ring.TickFrame();
    fence_manager.TickFrame();
    staging_pool.TickFrame();
    {
        std::scoped_lock lock{texture_cache.mutex};
        texture_cache.TickFrame();
    }
    {
        std::scoped_lock lock{buffer_cache.mutex};
        buffer_cache.TickFrame();
    }
}

bool RasterizerVulkan::AccelerateConditionalRendering() {
    FlushPendingDraw(DrawDrain::CondRender);
    gpu_memory->FlushCaching();
    return query_cache.AccelerateHostConditionalRendering();
}

bool RasterizerVulkan::HasDrawTransformFeedback() {
    FlushPendingDraw(DrawDrain::QueryCounter);
    return device.IsTransformFeedbackDrawSupported();
}

bool RasterizerVulkan::AccelerateSurfaceCopy(const Tegra::Engines::Fermi2D::Surface& src,
                                             const Tegra::Engines::Fermi2D::Surface& dst,
                                             const Tegra::Engines::Fermi2D::Config& copy_config) {
    FlushPendingDraw(DrawDrain::SurfaceCopy);
    std::scoped_lock lock{texture_cache.mutex};
    return texture_cache.BlitImage(dst, src, copy_config);
}

Tegra::Engines::AccelerateDMAInterface& RasterizerVulkan::AccessAccelerateDMA() {
    FlushPendingDraw(DrawDrain::AccessDMA);
    return accelerate_dma;
}

void RasterizerVulkan::AccelerateInlineToMemory(GPUVAddr address, size_t copy_size,
                                                std::span<const u8> memory) {
    FlushPendingDraw(DrawDrain::InlineToMemory);
    auto cpu_addr = gpu_memory->GpuToCpuAddress(address);
    if (!cpu_addr) [[unlikely]] {
        gpu_memory->WriteBlock(address, memory.data(), copy_size);
        return;
    }
    gpu_memory->WriteBlockUnsafe(address, memory.data(), copy_size);
    {
        std::unique_lock<std::recursive_mutex> lock{buffer_cache.mutex};
        if (!buffer_cache.InlineMemory(*cpu_addr, copy_size, memory)) {
            buffer_cache.WriteMemory(*cpu_addr, copy_size);
        }
    }
    {
        std::scoped_lock lock_texture{texture_cache.mutex};
        texture_cache.WriteMemory(*cpu_addr, copy_size);
    }
    pipeline_cache.InvalidateRegion(*cpu_addr, copy_size);
    query_cache.InvalidateRegion(*cpu_addr, copy_size);
}

std::optional<FramebufferTextureInfo> RasterizerVulkan::AccelerateDisplay(
    const Tegra::FramebufferConfig& config, DAddr framebuffer_addr, u32 pixel_stride) {
    FlushPendingDraw(DrawDrain::AccelDisplay);
    if (!framebuffer_addr) {
        return {};
    }
    std::scoped_lock lock{texture_cache.mutex};
    const auto [image_view, scaled] =
        texture_cache.TryFindFramebufferImageView(config, framebuffer_addr);
    if (!image_view) {
        return {};
    }

    query_cache.NotifySegment(false);

    const auto& resolution = Settings::values.resolution_info;

    FramebufferTextureInfo info{};
    info.image = image_view->ImageHandle();
    info.image_view = image_view->Handle(Shader::TextureType::Color2D);
    info.width = image_view->size.width;
    info.height = image_view->size.height;
    info.scaled_width = scaled ? resolution.ScaleUp(info.width) : info.width;
    info.scaled_height = scaled ? resolution.ScaleUp(info.height) : info.height;
    return info;
}

void RasterizerVulkan::LoadDiskResources(u64 title_id, std::stop_token stop_loading,
                                         const VideoCore::DiskResourceLoadCallback& callback) {
    FlushPendingDraw();
    pipeline_cache.LoadDiskResources(title_id, stop_loading, callback);
}

void RasterizerVulkan::FlushWork() {
#ifdef __ANDROID__
    static constexpr u32 DRAWS_TO_DISPATCH = 512;
    static constexpr u32 CHECK_MASK = 3;
#else
    static constexpr u32 DRAWS_TO_DISPATCH = 4096;
    static constexpr u32 CHECK_MASK = 7;
#endif // __ANDROID__

    static_assert(DRAWS_TO_DISPATCH % (CHECK_MASK + 1) == 0);
    if ((++draw_counter & CHECK_MASK) != CHECK_MASK) {
        return;
    }
    if (draw_counter < DRAWS_TO_DISPATCH) {
        scheduler.DispatchWork();
        return;
    }
    scheduler.Flush();
    draw_counter = 0;
}

AccelerateDMA::AccelerateDMA(BufferCache& buffer_cache_, TextureCache& texture_cache_,
                             Scheduler& scheduler_)
    : buffer_cache{buffer_cache_}, texture_cache{texture_cache_}, scheduler{scheduler_} {}

bool AccelerateDMA::BufferClear(GPUVAddr src_address, u64 amount, u32 value) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMAClear(src_address, amount, value);
}

bool AccelerateDMA::BufferCopy(GPUVAddr src_address, GPUVAddr dest_address, u64 amount) {
    std::scoped_lock lock{buffer_cache.mutex};
    return buffer_cache.DMACopy(src_address, dest_address, amount);
}

template <bool IS_IMAGE_UPLOAD>
bool AccelerateDMA::DmaBufferImageCopy(const Tegra::DMA::ImageCopy& copy_info,
                                       const Tegra::DMA::BufferOperand& buffer_operand,
                                       const Tegra::DMA::ImageOperand& image_operand) {
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    const auto image_id = texture_cache.DmaImageId(image_operand, IS_IMAGE_UPLOAD);
    if (image_id == VideoCommon::NULL_IMAGE_ID) {
        return false;
    }
    const u32 buffer_size = static_cast<u32>(buffer_operand.pitch * buffer_operand.height);
    static constexpr auto sync_info = VideoCommon::ObtainBufferSynchronize::FullSynchronize;
    const auto post_op = IS_IMAGE_UPLOAD ? VideoCommon::ObtainBufferOperation::DoNothing
                                         : VideoCommon::ObtainBufferOperation::MarkAsWritten;
    const auto [buffer, offset] =
        buffer_cache.ObtainBuffer(buffer_operand.address, buffer_size, sync_info, post_op);

    const auto [image, copy] = texture_cache.DmaBufferImageCopy(
        copy_info, buffer_operand, image_operand, image_id, IS_IMAGE_UPLOAD);
    const std::span copy_span{&copy, 1};

    if constexpr (IS_IMAGE_UPLOAD) {
        texture_cache.PrepareImage(image_id, true, false);
        image->UploadMemory(buffer->Handle(), offset, copy_span);
    } else {
        if (offset % BytesPerBlock(image->info.format)) {
            return false;
        }
        texture_cache.DownloadImageIntoBuffer(image, buffer->Handle(), offset, copy_span,
                                              buffer_operand.address, buffer_size);
    }
    return true;
}

bool AccelerateDMA::ImageToBuffer(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::ImageOperand& image_operand,
                                  const Tegra::DMA::BufferOperand& buffer_operand) {
    return DmaBufferImageCopy<false>(copy_info, buffer_operand, image_operand);
}

bool AccelerateDMA::BufferToImage(const Tegra::DMA::ImageCopy& copy_info,
                                  const Tegra::DMA::BufferOperand& buffer_operand,
                                  const Tegra::DMA::ImageOperand& image_operand) {
    return DmaBufferImageCopy<true>(copy_info, buffer_operand, image_operand);
}

void RasterizerVulkan::UpdateDynamicStates(Tegra::Engines::Maxwell3D& engine,
                                          GraphicsPipeline* pipeline) {
    auto& regs = engine.regs;
    auto& flags = engine.dirty.flags;
    const auto topology = engine.draw_manager.draw_state.topology;
    const bool topology_changed = state_tracker.ChangePrimitiveTopology(topology);
    if (topology_changed) {
        flags[Dirty::DepthBiasEnable] = true;
        flags[Dirty::PrimitiveRestartEnable] = true;
    }

    UpdateViewportsState(engine);
    UpdateScissorsState(regs);
    UpdateDepthBias(regs);
    UpdateBlendConstants(regs);
    UpdateDepthBounds(regs);
    UpdateStencilFaces(regs);
    UpdateLineWidth(regs);

    if (device.IsExtExtendedDynamicStateSupported()) {
        UpdateCullMode(regs);
        UpdateDepthCompareOp(regs);
        UpdateFrontFace(regs);
        UpdateStencilOp(regs);
        if (state_tracker.TouchStateEnable()) {
            UpdateDepthBoundsTestEnable(regs);
            UpdateDepthTestEnable(regs);
            UpdateDepthWriteEnable(regs);
            UpdateStencilTestEnable(regs);
        }
        if (topology_changed) {
            scheduler.Record([topology_vk = MaxwellToVK::PrimitiveTopology(device, topology)](
                                 vk::CommandBuffer cmdbuf) {
                cmdbuf.SetPrimitiveTopologyEXT(topology_vk);
            });
        }
    }

    if (device.IsExtExtendedDynamicState2Supported()) {
        UpdatePrimitiveRestartEnable(engine);
        UpdateRasterizerDiscardEnable(regs);
        UpdateDepthBiasEnable(engine);
    }

    if (device.IsExtExtendedDynamicState2ExtrasSupported()) {
        UpdateLogicOp(regs);
    }

    if (device.IsExtExtendedDynamicState3EnablesSupported()) {
        UpdateLogicOpEnable(regs);
        UpdateDepthClampEnable(regs);
        UpdateLineRasterizationMode(regs);
        UpdateLineStippleEnable(regs);
        UpdateConservativeRasterizationMode(regs);
        UpdateAlphaToCoverageEnable(regs, pipeline);
        UpdateAlphaToOneEnable(regs, pipeline);
    }

    if (device.IsExtExtendedDynamicState3BlendingSupported()) {
        UpdateBlending(regs);
    } else if (device.IsExtColorWriteEnableSupported()) {
        UpdateColorWriteEnable(regs);
    }

    if (device.IsExtVertexInputDynamicStateSupported()) {
        if (pipeline && pipeline->HasDynamicVertexInput()) {
            UpdateVertexInput(engine);
        }
    }
}

void RasterizerVulkan::HandleTransformFeedback(Tegra::Engines::Maxwell3D& engine) {
    static std::once_flag warn_unsupported;

    const auto& regs = engine.regs;
    if (!device.IsExtTransformFeedbackSupported()) {
        if (regs.transform_feedback_enabled != 0) {
            std::call_once(warn_unsupported, [&] {
                LOG_WARNING(Render_Vulkan, "Transform feedback requested by guest but VK_EXT_transform_feedback is unavailable; queries disabled");
            });
        } else {
            std::call_once(warn_unsupported, [&] {
                LOG_INFO(Render_Vulkan, "VK_EXT_transform_feedback not available on device");
            });
        }
        return;
    }
    query_cache.CounterEnable(VideoCommon::QueryType::StreamingByteCount,
                              regs.transform_feedback_enabled);
    if (regs.transform_feedback_enabled != 0) {
        // Log extension usage for transform feedback
        if (GPU::Logging::IsActive()) {
            GPU::Logging::GPULogger::GetInstance().LogExtensionUsage(
                "VK_EXT_transform_feedback", "HandleTransformFeedback");
        }
        UNIMPLEMENTED_IF(regs.IsShaderConfigEnabled(Maxwell::ShaderType::TessellationInit) ||
                         regs.IsShaderConfigEnabled(Maxwell::ShaderType::Tessellation));
    }
}

void RasterizerVulkan::UpdateViewportsState(Tegra::Engines::Maxwell3D& engine) {
    if (!state_tracker.TouchViewports()) {
        return;
    }

    auto& regs = engine.regs;
    engine.dirty.flags[Dirty::Scissors] = true;

    if (!regs.viewport_scale_offset_enabled) {
        float x = static_cast<float>(regs.surface_clip.x);
        float y = static_cast<float>(regs.surface_clip.y);
        float width = (std::max)(1.0f, static_cast<float>(regs.surface_clip.width));
        float height = (std::max)(1.0f, static_cast<float>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y += height;
            height = -height;
        }
        VkViewport viewport{
            .x = x,
            .y = y,
            .width = width,
            .height = height,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        scheduler.Record([this, viewport](vk::CommandBuffer cmdbuf) {
            const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkViewport, Maxwell::NumViewports> viewport_list{};
            viewport_list.fill(viewport);
            const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
            cmdbuf.SetViewport(0, viewports);
        });
        return;
    }
    const bool is_rescaling{texture_cache.IsRescaling()};
    const float scale = is_rescaling ? Settings::values.resolution_info.up_factor : 1.0f;
    const std::array viewport_list{
        GetViewportState(device, regs, 0, scale),  GetViewportState(device, regs, 1, scale),
        GetViewportState(device, regs, 2, scale),  GetViewportState(device, regs, 3, scale),
        GetViewportState(device, regs, 4, scale),  GetViewportState(device, regs, 5, scale),
        GetViewportState(device, regs, 6, scale),  GetViewportState(device, regs, 7, scale),
        GetViewportState(device, regs, 8, scale),  GetViewportState(device, regs, 9, scale),
        GetViewportState(device, regs, 10, scale), GetViewportState(device, regs, 11, scale),
        GetViewportState(device, regs, 12, scale), GetViewportState(device, regs, 13, scale),
        GetViewportState(device, regs, 14, scale), GetViewportState(device, regs, 15, scale),
    };
    scheduler.Record([this, viewport_list](vk::CommandBuffer cmdbuf) {
        const u32 num_viewports = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkViewport> viewports(viewport_list.data(), num_viewports);
        cmdbuf.SetViewport(0, viewports);
    });
}

void RasterizerVulkan::UpdateScissorsState(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchScissors()) {
        return;
    }
    if (!regs.viewport_scale_offset_enabled) {
        u32 x = regs.surface_clip.x;
        u32 y = regs.surface_clip.y;
        u32 width = (std::max)(1u, static_cast<u32>(regs.surface_clip.width));
        u32 height = (std::max)(1u, static_cast<u32>(regs.surface_clip.height));
        if (regs.window_origin.mode != Maxwell::WindowOrigin::Mode::UpperLeft) {
            y = regs.surface_clip.height - (y + height);
        }
        VkRect2D scissor{};
        scissor.offset.x = static_cast<int32_t>(x);
        scissor.offset.y = static_cast<int32_t>(y);
        scissor.extent.width  = width;
        scissor.extent.height = height;
        scheduler.Record([this, scissor](vk::CommandBuffer cmdbuf) {
            const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
            std::array<VkRect2D, Maxwell::NumViewports> scissor_list{};
            scissor_list.fill(scissor);
            const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
            cmdbuf.SetScissor(0, scissors);
        });
        return;
    }
    u32 up_scale = 1;
    u32 down_shift = 0;
    if (texture_cache.IsRescaling()) {
        up_scale = Settings::values.resolution_info.up_scale;
        down_shift = Settings::values.resolution_info.down_shift;
    }
    const std::array scissor_list{
        GetScissorState(regs, 0, up_scale, down_shift),
        GetScissorState(regs, 1, up_scale, down_shift),
        GetScissorState(regs, 2, up_scale, down_shift),
        GetScissorState(regs, 3, up_scale, down_shift),
        GetScissorState(regs, 4, up_scale, down_shift),
        GetScissorState(regs, 5, up_scale, down_shift),
        GetScissorState(regs, 6, up_scale, down_shift),
        GetScissorState(regs, 7, up_scale, down_shift),
        GetScissorState(regs, 8, up_scale, down_shift),
        GetScissorState(regs, 9, up_scale, down_shift),
        GetScissorState(regs, 10, up_scale, down_shift),
        GetScissorState(regs, 11, up_scale, down_shift),
        GetScissorState(regs, 12, up_scale, down_shift),
        GetScissorState(regs, 13, up_scale, down_shift),
        GetScissorState(regs, 14, up_scale, down_shift),
        GetScissorState(regs, 15, up_scale, down_shift),
    };
    scheduler.Record([this, scissor_list](vk::CommandBuffer cmdbuf) {
        const u32 num_scissors = std::min<u32>(device.GetMaxViewports(), Maxwell::NumViewports);
        const vk::Span<VkRect2D> scissors(scissor_list.data(), num_scissors);
        cmdbuf.SetScissor(0, scissors);
    });
}

void RasterizerVulkan::UpdateDepthBias(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBias()) {
        return;
    }
    float units = regs.depth_bias / 2.0f;
    const bool is_d24 = regs.zeta.format == Tegra::DepthFormat::Z24_UNORM_S8_UINT ||
                        regs.zeta.format == Tegra::DepthFormat::X8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::S8Z24_UNORM ||
                        regs.zeta.format == Tegra::DepthFormat::V8Z24_UNORM;

    if (is_d24 && !device.SupportsD24DepthBuffer()) {
        static constexpr const size_t length = sizeof(NEEDS_D24) / sizeof(NEEDS_D24[0]);

        static constexpr const u64* start = NEEDS_D24;
        static constexpr const u64* end = NEEDS_D24 + length;

        const u64* it = std::find(start, end, program_id);

        if (it != end) {
            // the base formulas can be obtained from here:
            //   https://docs.microsoft.com/en-us/windows/win32/direct3d11/d3d10-graphics-programming-guide-output-merger-stage-depth-bias
            const double rescale_factor =
                static_cast<double>(1ULL << (32 - 24)) / (static_cast<double>(0x1.ep+127));
            units = static_cast<float>(static_cast<double>(units) * rescale_factor);
        }
    }

    scheduler.Record([constant = units, clamp = regs.depth_bias_clamp,
                      factor = regs.slope_scale_depth_bias, this](vk::CommandBuffer cmdbuf) {
        if (device.IsExtDepthBiasControlSupported()) {
            static VkDepthBiasRepresentationInfoEXT bias_info{
                .sType = VK_STRUCTURE_TYPE_DEPTH_BIAS_REPRESENTATION_INFO_EXT,
                .pNext = nullptr,
                .depthBiasRepresentation =
                    VK_DEPTH_BIAS_REPRESENTATION_LEAST_REPRESENTABLE_VALUE_FORCE_UNORM_EXT,
                .depthBiasExact = VK_FALSE,
            };

            cmdbuf.SetDepthBias(constant, clamp, factor, &bias_info);
        } else {
            cmdbuf.SetDepthBias(constant, clamp, factor);
        }
    });
}

void RasterizerVulkan::UpdateBlendConstants(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlendConstants()) {
        return;
    }
    const std::array blend_color = {regs.blend_color.r, regs.blend_color.g, regs.blend_color.b,
                                    regs.blend_color.a};
    scheduler.Record(
        [blend_color](vk::CommandBuffer cmdbuf) { cmdbuf.SetBlendConstants(blend_color.data()); });
}

void RasterizerVulkan::UpdateDepthBounds(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBounds()) {
        return;
    }
    scheduler.Record([min = regs.depth_bounds[0], max = regs.depth_bounds[1]](
                         vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBounds(min, max); });
}

void RasterizerVulkan::UpdateStencilFaces(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilProperties()) {
        return;
    }
    bool update_references = state_tracker.TouchStencilReference();
    bool update_write_mask = state_tracker.TouchStencilWriteMask();
    bool update_compare_masks = state_tracker.TouchStencilCompare();
    if (state_tracker.TouchStencilSide(regs.stencil_two_side_enable != 0)) {
        update_references = true;
        update_write_mask = true;
        update_compare_masks = true;
    }
    if (update_references) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_back_ref);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilReferenceFront(regs.stencil_front_ref);
                const bool back_changed =
                    state_tracker.CheckStencilReferenceBack(regs.stencil_front_ref);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_ref = regs.stencil_front_ref, back_ref = regs.stencil_back_ref,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_ref != back_ref;
                // Front face
                cmdbuf.SetStencilReference(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_ref);
                if (set_back) {
                    cmdbuf.SetStencilReference(VK_STENCIL_FACE_BACK_BIT, back_ref);
                }
            });
        }();
    }
    if (update_write_mask) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_back_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilWriteMaskFront(regs.stencil_front_mask);
                const bool back_changed =
                    state_tracker.CheckStencilWriteMaskBack(regs.stencil_front_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_write_mask = regs.stencil_front_mask,
                              back_write_mask = regs.stencil_back_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_write_mask != back_write_mask;
                // Front face
                cmdbuf.SetStencilWriteMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                    : VK_STENCIL_FACE_FRONT_AND_BACK,
                                           front_write_mask);
                if (set_back) {
                    cmdbuf.SetStencilWriteMask(VK_STENCIL_FACE_BACK_BIT, back_write_mask);
                }
            });
        }();
    }
    if (update_compare_masks) {
        [&]() {
            bool changed;
            if (regs.stencil_two_side_enable) {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_back_func_mask);
                changed = front_changed || back_changed;
            } else {
                const bool front_changed =
                    state_tracker.CheckStencilCompareMaskFront(regs.stencil_front_func_mask);
                const bool back_changed =
                    state_tracker.CheckStencilCompareMaskBack(regs.stencil_front_func_mask);
                changed = front_changed || back_changed;
            }
            if (!changed) {
                return;
            }
            scheduler.Record([front_test_mask = regs.stencil_front_func_mask,
                              back_test_mask = regs.stencil_back_func_mask,
                              two_sided = regs.stencil_two_side_enable](vk::CommandBuffer cmdbuf) {
                const bool set_back = two_sided && front_test_mask != back_test_mask;
                // Front face
                cmdbuf.SetStencilCompareMask(set_back ? VK_STENCIL_FACE_FRONT_BIT
                                                      : VK_STENCIL_FACE_FRONT_AND_BACK,
                                             front_test_mask);
                if (set_back) {
                    cmdbuf.SetStencilCompareMask(VK_STENCIL_FACE_BACK_BIT, back_test_mask);
                }
            });
        }();
    }
    state_tracker.ClearStencilReset();
}

void RasterizerVulkan::UpdateLineWidth(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineWidth()) {
        return;
    }
    const float width =
        regs.line_anti_alias_enable ? regs.line_width_smooth : regs.line_width_aliased;
    scheduler.Record([width](vk::CommandBuffer cmdbuf) { cmdbuf.SetLineWidth(width); });
}

void RasterizerVulkan::UpdateCullMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchCullMode()) {
        return;
    }
    scheduler.Record([enabled = regs.gl_cull_test_enabled,
                      cull_face = regs.gl_cull_face](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetCullModeEXT(enabled ? MaxwellToVK::CullFace(cull_face) : VK_CULL_MODE_NONE);
    });
}

void RasterizerVulkan::UpdateDepthBoundsTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthBoundsTestEnable()) {
        return;
    }
    bool enabled = regs.depth_bounds_enable;
    if (enabled && !device.IsDepthBoundsSupported()) {
        LOG_WARNING(Render_Vulkan, "Depth bounds is enabled but not supported");
        enabled = false;
    }
    scheduler.Record([enable = enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthBoundsTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_test_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateDepthWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthWriteEnable()) {
        return;
    }
    scheduler.Record([enable = regs.depth_write_enabled](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthWriteEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdatePrimitiveRestartEnable(Tegra::Engines::Maxwell3D& engine) {
    const auto& regs = engine.regs;
    if (!state_tracker.TouchPrimitiveRestartEnable()) {
        return;
    }

    bool enable = regs.primitive_restart.enabled != 0;
    if (device.IsMoltenVK()) {
        enable = true;
    } else if (enable) {
        const auto topology = MaxwellToVK::PrimitiveTopology(device, engine.draw_manager.draw_state.topology);
        enable = IsPrimitiveRestartSupported(device, topology);
    }

    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetPrimitiveRestartEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateRasterizerDiscardEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchRasterizerDiscardEnable()) {
        return;
    }
    scheduler.Record([disable = regs.rasterize_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetRasterizerDiscardEnableEXT(disable == 0);
    });
}

void RasterizerVulkan::UpdateConservativeRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchConservativeRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3ConservativeRasterizationMode()) {
        return;
    }

    scheduler.Record([enable = regs.conservative_raster_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetConservativeRasterizationModeEXT(
            enable ? VK_CONSERVATIVE_RASTERIZATION_MODE_OVERESTIMATE_EXT
                   : VK_CONSERVATIVE_RASTERIZATION_MODE_DISABLED_EXT);
    });
}

void RasterizerVulkan::UpdateLineStippleEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLineStippleEnable()) {
        return;
    }

    if (!device.SupportsDynamicState3LineStippleEnable()) {
        return;
    }

    scheduler.Record([enable = regs.line_stipple_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineStippleEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateLineRasterizationMode(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!device.IsExtLineRasterizationSupported()) {
        return;
    }
    if (!state_tracker.TouchLineRasterizationMode()) {
        return;
    }

    if (!device.SupportsDynamicState3LineRasterizationMode()) {
        static std::once_flag warn_missing_rect;
        std::call_once(warn_missing_rect, [] {
            LOG_WARNING(Render_Vulkan,
                        "Driver lacks rectangular line rasterization support; skipping dynamic "
                        "line state updates");
        });
        return;
    }

    const bool wants_smooth = regs.line_anti_alias_enable != 0;
    VkLineRasterizationModeEXT mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_EXT;
    if (wants_smooth) {
        if (device.SupportsSmoothLines()) {
            mode = VK_LINE_RASTERIZATION_MODE_RECTANGULAR_SMOOTH_EXT;
        } else {
            static std::once_flag warn_missing_smooth;
            std::call_once(warn_missing_smooth, [] {
                LOG_WARNING(Render_Vulkan,
                            "Line anti-aliasing requested but smoothLines feature unavailable; "
                            "using rectangular rasterization");
            });
        }
    }
    scheduler.Record([mode](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLineRasterizationModeEXT(mode);
    });
}

void RasterizerVulkan::UpdateDepthBiasEnable(Tegra::Engines::Maxwell3D& engine) {
    const auto& regs = engine.regs;
    if (!state_tracker.TouchDepthBiasEnable()) {
        return;
    }
    constexpr size_t POINT = 0;
    constexpr size_t LINE = 1;
    constexpr size_t POLYGON = 2;
    static constexpr std::array POLYGON_OFFSET_ENABLE_LUT = {
        POINT,   // Points
        LINE,    // Lines
        LINE,    // LineLoop
        LINE,    // LineStrip
        POLYGON, // Triangles
        POLYGON, // TriangleStrip
        POLYGON, // TriangleFan
        POLYGON, // Quads
        POLYGON, // QuadStrip
        POLYGON, // Polygon
        LINE,    // LinesAdjacency
        LINE,    // LineStripAdjacency
        POLYGON, // TrianglesAdjacency
        POLYGON, // TriangleStripAdjacency
        POLYGON, // Patches
    };
    const std::array enabled_lut{
        regs.polygon_offset_point_enable,
        regs.polygon_offset_line_enable,
        regs.polygon_offset_fill_enable,
    };
    const u32 topology_index = u32(engine.draw_manager.draw_state.topology);
    const u32 enable = enabled_lut[POLYGON_OFFSET_ENABLE_LUT[topology_index]];
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthBiasEnableEXT(enable != 0); });
}

void RasterizerVulkan::UpdateLogicOpEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOpEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3LogicOpEnable()) {
        return;
    }
    bool enable = regs.logic_op.enable != 0;
    if (enable && (device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_OPEN_SOURCE ||
                   device.GetDriverID() == VkDriverIdKHR::VK_DRIVER_ID_AMD_PROPRIETARY)) {
        // Do not mutate guest registers: a journaled shadow must remain exact.
        enable = !std::any_of(regs.vertex_attrib_format.begin(), regs.vertex_attrib_format.end(),
                             [](const auto& attrib) {
                                 return attrib.type == Maxwell::VertexAttribute::Type::Float;
                             });
    }
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetLogicOpEnableEXT(enable != 0);
    });
}

void RasterizerVulkan::UpdateDepthClampEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthClampEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3DepthClampEnable()) {
        return;
    }
    bool is_enabled = !(regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::Passthrough ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumXYZ ||
                        regs.viewport_clip_control.geometry_clip ==
                            Maxwell::ViewportClipControl::GeometryClip::FrustumZ);
    scheduler.Record(
        [is_enabled](vk::CommandBuffer cmdbuf) { cmdbuf.SetDepthClampEnableEXT(is_enabled); });
}

void RasterizerVulkan::UpdateAlphaToCoverageEnable(Tegra::Engines::Maxwell3D::Regs& regs,
                                               GraphicsPipeline* pipeline) {
    if (!state_tracker.TouchAlphaToCoverageEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToCoverageEnable()) {
        return;
    }
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToCoverage() &&
                        regs.anti_alias_alpha_control.alpha_to_coverage != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToCoverageEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateAlphaToOneEnable(Tegra::Engines::Maxwell3D::Regs& regs,
                                               GraphicsPipeline* pipeline) {
    if (!state_tracker.TouchAlphaToOneEnable()) {
        return;
    }
    if (!device.SupportsDynamicState3AlphaToOneEnable()) {
        static std::once_flag warn_alpha_to_one;
        std::call_once(warn_alpha_to_one, [] {
            LOG_WARNING(Render_Vulkan,
                        "Alpha-to-one is not supported on this device; forcing it disabled");
        });
        return;
    }
    const bool enable = pipeline != nullptr && pipeline->SupportsAlphaToOne() &&
                        regs.anti_alias_alpha_control.alpha_to_one != 0;
    scheduler.Record([enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetAlphaToOneEnableEXT(enable ? VK_TRUE : VK_FALSE);
    });
}

void RasterizerVulkan::UpdateDepthCompareOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchDepthCompareOp()) {
        return;
    }
    scheduler.Record([func = regs.depth_test_func](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetDepthCompareOpEXT(MaxwellToVK::ComparisonOp(func));
    });
}

void RasterizerVulkan::UpdateFrontFace(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchFrontFace()) {
        return;
    }

    VkFrontFace front_face = MaxwellToVK::FrontFace(regs.gl_front_face);
    if (regs.window_origin.flip_y != 0) {
        front_face = front_face == VK_FRONT_FACE_CLOCKWISE ? VK_FRONT_FACE_COUNTER_CLOCKWISE
                                                           : VK_FRONT_FACE_CLOCKWISE;
    }
    scheduler.Record(
        [front_face](vk::CommandBuffer cmdbuf) { cmdbuf.SetFrontFaceEXT(front_face); });
}

void RasterizerVulkan::UpdateStencilOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilOp()) {
        return;
    }
    const Maxwell::StencilOp::Op fail = regs.stencil_front_op.fail;
    const Maxwell::StencilOp::Op zfail = regs.stencil_front_op.zfail;
    const Maxwell::StencilOp::Op zpass = regs.stencil_front_op.zpass;
    const Maxwell::ComparisonOp compare = regs.stencil_front_op.func;
    if (regs.stencil_two_side_enable) {
        // Separate stencil op per face
        const Maxwell::StencilOp::Op back_fail = regs.stencil_back_op.fail;
        const Maxwell::StencilOp::Op back_zfail = regs.stencil_back_op.zfail;
        const Maxwell::StencilOp::Op back_zpass = regs.stencil_back_op.zpass;
        const Maxwell::ComparisonOp back_compare = regs.stencil_back_op.func;
        scheduler.Record([fail, zfail, zpass, compare, back_fail, back_zfail, back_zpass,
                          back_compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_BIT, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_BACK_BIT, MaxwellToVK::StencilOp(back_fail),
                                   MaxwellToVK::StencilOp(back_zpass),
                                   MaxwellToVK::StencilOp(back_zfail),
                                   MaxwellToVK::ComparisonOp(back_compare));
        });
    } else {
        // Front face defines the stencil op of both faces
        scheduler.Record([fail, zfail, zpass, compare](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetStencilOpEXT(VK_STENCIL_FACE_FRONT_AND_BACK, MaxwellToVK::StencilOp(fail),
                                   MaxwellToVK::StencilOp(zpass), MaxwellToVK::StencilOp(zfail),
                                   MaxwellToVK::ComparisonOp(compare));
        });
    }
}

void RasterizerVulkan::UpdateLogicOp(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchLogicOp()) {
        return;
    }
    const auto op_value = static_cast<u32>(regs.logic_op.op);
    auto op = op_value >= 0x1500 && op_value < 0x1510 ? static_cast<VkLogicOp>(op_value - 0x1500)
                                                      : VK_LOGIC_OP_NO_OP;
    scheduler.Record([op](vk::CommandBuffer cmdbuf) { cmdbuf.SetLogicOpEXT(op); });
}

void RasterizerVulkan::UpdateBlending(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchBlending()) {
        return;
    }

    if (state_tracker.TouchColorMask()) {
        std::array<VkColorComponentFlags, Maxwell::NumRenderTargets> setup_masks{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
            auto& current = setup_masks[index];
            if (mask.R) {
                current |= VK_COLOR_COMPONENT_R_BIT;
            }
            if (mask.G) {
                current |= VK_COLOR_COMPONENT_G_BIT;
            }
            if (mask.B) {
                current |= VK_COLOR_COMPONENT_B_BIT;
            }
            if (mask.A) {
                current |= VK_COLOR_COMPONENT_A_BIT;
            }
        }
        scheduler.Record([setup_masks](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorWriteMaskEXT(0, setup_masks);
        });
    }

    if (state_tracker.TouchBlendEnable()) {
        std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
        for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
            bool is_integer = false;
            if (regs.rt[index].format != Tegra::RenderTargetFormat::NONE) {
                const auto format =
                    VideoCore::Surface::PixelFormatFromRenderTargetFormat(regs.rt[index].format);
                is_integer = IsPixelFormatInteger(format);
            }
            setup_enables[index] =
                (!is_integer && regs.blend.enable[index] != 0) ? VK_TRUE : VK_FALSE;
        }
        scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEnableEXT(0, setup_enables);
        });
    }

    if (state_tracker.TouchBlendEquations()) {
        std::array<VkColorBlendEquationEXT, Maxwell::NumRenderTargets> setup_blends{};

        const auto blend_setup = [&](auto& host_blend, const auto& guest_blend) {
            host_blend.srcColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_source);
            host_blend.dstColorBlendFactor = MaxwellToVK::BlendFactor(guest_blend.color_dest);
            host_blend.colorBlendOp = MaxwellToVK::BlendEquation(guest_blend.color_op);
            host_blend.srcAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_source);
            host_blend.dstAlphaBlendFactor = MaxwellToVK::BlendFactor(guest_blend.alpha_dest);
            host_blend.alphaBlendOp = MaxwellToVK::BlendEquation(guest_blend.alpha_op);
        };

        // Single blend equation for all targets
        if (!regs.blend_per_target_enabled) {
            // Temporary workaround for games that use iterated blending
            if (regs.iterated_blend.enable && Settings::values.use_squashed_iterated_blend) {
                setup_blends[0].srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
                setup_blends[0].colorBlendOp = VK_BLEND_OP_ADD;
                setup_blends[0].srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
                setup_blends[0].dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
                setup_blends[0].alphaBlendOp = VK_BLEND_OP_ADD;
            } else {
                blend_setup(setup_blends[0], regs.blend);
            }

            // Copy first blend state to all other targets
            for (size_t index = 1; index < Maxwell::NumRenderTargets; index++) {
                setup_blends[index] = setup_blends[0];
            }
        } else {
            // Per-target blending
            for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
                blend_setup(setup_blends[index], regs.blend_per_target[index]);
            }
        }

        scheduler.Record([setup_blends](vk::CommandBuffer cmdbuf) {
            cmdbuf.SetColorBlendEquationEXT(0, setup_blends);
        });
    }
}

void RasterizerVulkan::UpdateColorWriteEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchColorMask()) {
        return;
    }
    std::array<VkBool32, Maxwell::NumRenderTargets> setup_enables{};
    for (size_t index = 0; index < Maxwell::NumRenderTargets; index++) {
        const auto& mask = regs.color_mask[regs.color_mask_common ? 0 : index];
        setup_enables[index] = (mask.R || mask.G || mask.B || mask.A) ? VK_TRUE : VK_FALSE;
    }
    scheduler.Record([setup_enables](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetColorWriteEnableEXT(setup_enables);
    });
}

void RasterizerVulkan::UpdateStencilTestEnable(Tegra::Engines::Maxwell3D::Regs& regs) {
    if (!state_tracker.TouchStencilTestEnable()) {
        return;
    }
    scheduler.Record([enable = regs.stencil_enable](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetStencilTestEnableEXT(enable);
    });
}

void RasterizerVulkan::UpdateVertexInput(Tegra::Engines::Maxwell3D& engine) {
    auto& regs = engine.regs;
    auto& dirty{engine.dirty.flags};
    const bool vertex_input_dirty = dirty[Dirty::VertexInput];
    const bool vertex_buffers_dirty = dirty[VideoCommon::Dirty::VertexBuffers];
    if (!vertex_input_dirty && !vertex_buffers_dirty) {
        return;
    }
    dirty[Dirty::VertexInput] = false;

    boost::container::static_vector<VkVertexInputBindingDescription2EXT, 32> bindings;
    boost::container::static_vector<VkVertexInputAttributeDescription2EXT, 32> attributes;

    const u32 max_attributes =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexAttributes,
                                          device.GetMaxVertexInputAttributes()));
    const u32 max_bindings =
        static_cast<u32>(std::min<size_t>(Maxwell::NumVertexArrays,
                                          device.GetMaxVertexInputBindings()));


    for (u32 index = 0; index < max_attributes; ++index) {
        const Maxwell::VertexAttribute attribute{regs.vertex_attrib_format[index]};
        const u32 binding{attribute.buffer};
        if (attribute.constant || binding >= max_bindings) {
            continue;
        }
        attributes.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .location = index,
            .binding = binding,
            .format = MaxwellToVK::VertexFormat(device, attribute.type, attribute.size),
            .offset = attribute.offset,
        });
    }

    for (u32 binding = 0; binding < max_bindings; ++binding) {
        const auto& input_binding{regs.vertex_streams[binding]};
        const bool is_instanced{regs.vertex_stream_instances.IsInstancingEnabled(binding)};
        bindings.push_back({
            .sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT,
            .pNext = nullptr,
            .binding = binding,
            .stride = input_binding.stride,
            .inputRate = is_instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX,
            .divisor = is_instanced ? input_binding.frequency : 1,
        });
    }

    for (size_t index = 0; index < Maxwell::NumVertexAttributes; ++index) {
        dirty[Dirty::VertexAttribute0 + index] = false;
    }
    for (size_t index = 0; index < Maxwell::NumVertexArrays; ++index) {
        dirty[Dirty::VertexBinding0 + index] = false;
    }

    scheduler.Record([bindings, attributes](vk::CommandBuffer cmdbuf) {
        cmdbuf.SetVertexInputEXT(bindings, attributes);
    });
}

void RasterizerVulkan::InitializeChannel(Tegra::Control::ChannelState& channel) {
    FlushPendingDraw();
    CreateChannel(channel);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.CreateChannel(channel);
        buffer_cache.CreateChannel(channel);
    }
    pipeline_cache.CreateChannel(channel);
    query_cache.CreateChannel(channel);
    state_tracker.SetupTables(channel);
}

void RasterizerVulkan::BindChannel(Tegra::Control::ChannelState& channel) {
    FlushPendingDraw(DrawDrain::Channel);
    const s32 channel_id = channel.bind_id;
    if (maxwell3d != &channel.payload->maxwell_3d) {
        // The resolver owns a reference to its source channel's memory manager.
        if (resolver && resolver->job_bindings_enabled) {
            std::scoped_lock lock{buffer_cache.mutex};
            resolver->ApplyPendingUniformInputs();
        }
        resolver.reset();
    }
    BindToChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.BindToChannel(channel_id);
        buffer_cache.BindToChannel(channel_id);
    }
    pipeline_cache.BindToChannel(channel_id);
    query_cache.BindToChannel(channel_id);
    state_tracker.ChangeChannel(channel);
    state_tracker.InvalidateState();
}

void RasterizerVulkan::ReleaseChannel(s32 channel_id) {
    FlushPendingDraw(DrawDrain::Channel);
    if (resolver && resolver->job_bindings_enabled) {
        std::scoped_lock lock{buffer_cache.mutex};
        resolver->ApplyPendingUniformInputs();
    }
    resolver.reset();
    EraseChannel(channel_id);
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        texture_cache.EraseChannel(channel_id);
        buffer_cache.EraseChannel(channel_id);
    }
    pipeline_cache.EraseChannel(channel_id);
    query_cache.EraseChannel(channel_id);
}

} // namespace Vulkan
