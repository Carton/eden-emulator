// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <exception>

#include "common/assert.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "core/frontend/graphics_context.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/host1x/host1x.h"
#include "video_core/renderer_base.h"

namespace VideoCommon::GPUThread {

ThreadManager::ThreadManager(Core::System& system_)
    : system{system_}
{}

ThreadManager::~ThreadManager() = default;

void ThreadManager::StartThread(VideoCore::RendererBase& renderer, Core::Frontend::GraphicsContext& context, Tegra::Control::Scheduler& scheduler) {
    rasterizer = renderer.ReadRasterizer();
    thread = std::jthread([&](std::stop_token stop_token) {
        Common::SetCurrentThreadName("GPU");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
        Common::SetCurrentThreadToPerformanceCores();
        system.RegisterHostThread();

        auto current_context = context.Acquire();
        CommandDataContainer next;
        while (!stop_token.stop_requested()) {
            bool has_command = true;
            if (rasterizer->UsesGPUServiceHandoff()) {
                std::unique_lock lock{service_mutex};
                service_cv.wait(lock, stop_token, [&] {
                    has_command = state.queue.TryPop(next);
                    return has_command || service_wake;
                });
                service_wake = false;
            } else {
                state.queue.PopWait(next, stop_token);
            }
            if (stop_token.stop_requested()) {
                break;
            }
            if (!has_command) {
                system.GPU().TickWork();
                continue;
            }
            try {
                if (system.GPU().HasRendererFailure()) {
                    // Keep acknowledging cancelled queue entries until frontend
                    // shutdown, so synchronous submitters cannot strand teardown.
                } else if (auto* submit_list = std::get_if<SubmitListCommand>(&next.data)) {
                    scheduler.Push(system.GPU(), submit_list->channel, std::move(submit_list->entries));
                } else if (std::holds_alternative<GPUTickCommand>(next.data)) {
                    system.GPU().TickWork();
                } else if (const auto* flush = std::get_if<FlushRegionCommand>(&next.data)) {
                    renderer.ReadRasterizer()->FlushRegion(flush->addr, flush->size);
                } else if (const auto* invalidate = std::get_if<InvalidateRegionCommand>(&next.data)) {
                    renderer.ReadRasterizer()->OnCacheInvalidation(invalidate->addr, invalidate->size);
                } else {
                    ASSERT(false);
                }
            } catch (...) {
                if (!rasterizer->AbortGPUService(std::current_exception())) {
                    throw; // preserve non-Stage-4 error behavior
                }
                system.GPU().NotifyRendererFailure();
            }
            state.signaled_fence.store(next.fence);
            if (next.block) {
                // We have to lock the write_lock to ensure that the condition_variable wait not get a
                // race between the check and the lock itself.
                std::scoped_lock lk{state.write_lock};
                state.cv.notify_all();
            }
            if (rasterizer->UsesGPUServiceHandoff()) {
                // Service callbacks also make progress under command pressure.
                // TickWork catches Stage-4 failures.
                system.GPU().TickWork();
            }
        }
    });
}

void ThreadManager::SubmitList(s32 channel, Tegra::CommandList&& entries, bool is_async) {
    PushCommand(SubmitListCommand(channel, std::move(entries)), false, is_async);
}

void ThreadManager::FlushRegion(DAddr addr, u64 size, bool is_async) {
    if (!is_async) {
        // Always flush with synchronous GPU mode
        PushCommand(FlushRegionCommand(addr, size), false, is_async);
    }
}

void ThreadManager::TickGPU(bool is_async) {
    PushCommand(GPUTickCommand(), false, is_async);
}

void ThreadManager::WakeGPUService() {
    {
        std::scoped_lock lock{service_mutex};
        service_wake = true;
    }
    service_cv.notify_one();
}

void ThreadManager::InvalidateRegion(DAddr addr, u64 size) {
    rasterizer->OnCacheInvalidation(addr, size);
}

void ThreadManager::FlushAndInvalidateRegion(DAddr addr, u64 size, bool is_async) {
    if (Settings::IsGPULevelHigh()) {
        if (!is_async) {
            PushCommand(FlushRegionCommand(addr, size), false, is_async);
        } else {
            auto& gpu = system.GPU();
            const u64 fence = gpu.RequestFlush(addr, size);
            TickGPU(is_async);
            gpu.WaitForSyncOperation(fence);
        }
    }
    rasterizer->OnCacheInvalidation(addr, size);
}

u64 ThreadManager::PushCommand(CommandData&& command_data, bool block, bool is_async) {
    if (!is_async) {
        // In synchronous GPU mode, block the caller until the command has executed
        block = true;
    }

    std::unique_lock lk(state.write_lock);
    const u64 fence{++state.last_fence};
    state.queue.EmplaceWait(std::move(command_data), fence, block);
    if (rasterizer->UsesGPUServiceHandoff()) {
        WakeGPUService();
    }

    if (block) {
        state.cv.wait(lk, thread.get_stop_token(), [this, fence] {
            return fence <= state.signaled_fence.load(std::memory_order_relaxed);
        });
    }

    return fence;
}

} // namespace VideoCommon::GPUThread
