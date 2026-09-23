// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <list>
#include <memory>
#include <utility>

#include "common/assert.h"
#include "common/cpu_features.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "common/settings_enums.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/frontend/emu_window.h"
#include "core/frontend/graphics_context.h"
#include "core/hle/service/nvdrv/nvdata.h"
#include "core/perf_stats.h"
#include "video_core/cdma_pusher.h"
#include "video_core/control/channel_state.h"
#include "video_core/control/scheduler.h"
#include "video_core/dma_pusher.h"
#include "video_core/engines/fermi_2d.h"
#include "video_core/engines/kepler_compute.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu.h"
#include "video_core/gpu_thread.h"
#include "video_core/host1x/host1x.h"
#include "video_core/host1x/syncpoint_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_base.h"
#include "video_core/shader_notify.h"

namespace Tegra {

namespace {
thread_local u64 ocr_preemtive_hits{};
thread_local u64 ocr_sync_flushes{};
thread_local u64 ocr_syncwait_cyc{};
thread_local u64 ocr_flusharea_cyc{};

constexpr u64 GpuClockMultiplier(Settings::GpuClock clock) {
    switch (clock) {
    case Settings::GpuClock::Boost:
        return 256;
    case Settings::GpuClock::Overclock:
        return 512;
    default:
        return 1;
    }
}
} // Anonymous namespace

// Called only at the CPU download reporting interval, on the same host thread.
void LogDownloadBlockingDiagnostics(u64 calls, u64 area_hits, u64 oncpu_reads) {
    const double ticks_per_second = static_cast<double>(
        Common::g_wall_clock.NsToTicks(std::chrono::seconds{1}));
    const double syncwait_us = static_cast<double>(ocr_syncwait_cyc) * 1'000'000.0 /
                               ticks_per_second;
    const double flusharea_us = static_cast<double>(ocr_flusharea_cyc) * 1'000'000.0 /
                                ticks_per_second;
    LOG_INFO(HW_GPU, "DLB diag: dl_calls={} dl_area_hits={} dl_oncpu_read={} "
                     "ocr_preemtive_hits={} ocr_sync_flushes={} ocr_syncwait_cyc={} "
                     "syncwait_avg_us={} syncwait_total_us={} flusharea_avg_us={}",
             calls, area_hits, oncpu_reads, ocr_preemtive_hits, ocr_sync_flushes,
             ocr_syncwait_cyc, ocr_sync_flushes ? syncwait_us / ocr_sync_flushes : 0.0,
             syncwait_us,
             (ocr_preemtive_hits + ocr_sync_flushes) ? flusharea_us /
                 double(ocr_preemtive_hits + ocr_sync_flushes) : 0.0);
}

struct GPU::Impl {
    explicit Impl(Core::System& system_, bool is_async_, bool use_nvdec_)
        : system{system_}
        , use_nvdec{use_nvdec_}
        , shader_notify()
        , is_async{is_async_}
        , gpu_thread{system_}
    {}

    ~Impl() = default;

    std::shared_ptr<Control::ChannelState> CreateChannel(s32 channel_id) {
        auto channel_state = std::make_shared<Tegra::Control::ChannelState>(channel_id);
        channels.emplace(channel_id, channel_state);
        scheduler.DeclareChannel(channel_state);
        return channel_state;
    }

    void BindChannel(s32 channel_id) {
        if (bound_channel != channel_id) {
            auto it = channels.find(channel_id);
            ASSERT(it != channels.end());
            bound_channel = channel_id;
            current_channel = it->second.get();
            renderer->ReadRasterizer()->BindChannel(*current_channel);
        }
    }

    std::shared_ptr<Control::ChannelState> AllocateChannel() {
        return CreateChannel(new_channel_id++);
    }

    void InitChannel(Control::ChannelState& to_init, u64 program_id) {
        to_init.Init(system, program_id);
        to_init.BindRasterizer(renderer->ReadRasterizer());
        renderer->ReadRasterizer()->InitializeChannel(to_init);
    }

    void InitAddressSpace(Tegra::MemoryManager& memory_manager) {
        memory_manager.BindRasterizer(renderer->ReadRasterizer());
    }

    void ReleaseChannel(Control::ChannelState& to_release) {
        UNIMPLEMENTED();
    }

    /// Binds a renderer to the GPU.
    void BindRenderer(std::unique_ptr<VideoCore::RendererBase> renderer_) {
        renderer = std::move(renderer_);
        system.Host1x().memory_manager.BindInterface(renderer->ReadRasterizer());
        system.Host1x().gmmu_manager.BindRasterizer(renderer->ReadRasterizer());
    }

    /// Flush all current written commands into the host GPU for execution.
    void FlushCommands() {
        renderer->ReadRasterizer()->FlushCommands();
    }

    /// Synchronizes CPU writes with Host GPU memory.
    void InvalidateGPUCache() {
        std::function<void(PAddr, size_t)> callback_writes([this](PAddr address, size_t size) {
            renderer->ReadRasterizer()->OnCacheInvalidation(address, size);
        });
        system.GatherGPUDirtyMemory(callback_writes);
    }

    /// Signal the ending of command list.
    void OnCommandListEnd() {
        renderer->ReadRasterizer()->ReleaseFences(false);
        Settings::UpdateGPUAccuracy();
    }

    /// Request a host GPU memory flush from the CPU.
    template <typename Func>
    [[nodiscard]] u64 RequestSyncOperation(Func&& action) {
        std::unique_lock lck{sync_request_mutex};
        if (renderer_failed.load()) {
            return last_sync_fence; // cancelled session: do not retain borrowed captures
        }
        const u64 fence = ++last_sync_fence;
        sync_requests.emplace_back(std::forward<Func>(action));
        return fence;
    }

    /// Obtains current flush request fence id.
    [[nodiscard]] u64 CurrentSyncRequestFence() const {
        return current_sync_fence.load(std::memory_order_relaxed);
    }

    void WaitForSyncOperation(const u64 fence) {
        std::unique_lock lck{sync_request_mutex};
        sync_request_cv.wait(lck, [this, fence] {
            return CurrentSyncRequestFence() >= fence || renderer_failed.load();
        });
    }

    void WaitForIdle() {
        const u64 fence = RequestSyncOperation([] {});
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(fence);
    }

    /// Tick pending requests within the GPU.
    void TickWork() {
        const bool handoff = renderer->ReadRasterizer()->UsesGPUServiceHandoff();
        if (handoff && (servicing_sync || renderer_failed.load() || std::uncaught_exceptions() != 0)) {
            return;
        }
        if (handoff) {
            servicing_sync = true;
        }
        SCOPE_EXIT {
            if (handoff) {
                servicing_sync = false;
            }
        };
        std::unique_lock lck{sync_request_mutex};
        while (!sync_requests.empty()) {
            auto request = std::move(sync_requests.front());
            sync_requests.pop_front();
            lck.unlock();
            try {
                if (handoff) {
                    // Drain before arbitrary service code, outside the sync mutex
                    // and before acquiring any cache/renderer producer locks.
                    renderer->ReadRasterizer()->PrepareGPUService();
                }
                request();
            } catch (...) {
                if (!renderer->ReadRasterizer()->AbortGPUService(std::current_exception())) {
                    throw;
                }
                // TickWork also runs in noexcept scope guards. Do not rethrow
                // a poisoned Stage-4 failure through those destructors.
                NotifyRendererFailure();
                return;
            }
            current_sync_fence.fetch_add(1, std::memory_order_release);
            lck.lock();
            sync_request_cv.notify_all();
        }
    }

    void NotifyRendererFailure() {
        {
            std::scoped_lock lock{sync_request_mutex};
            if (renderer_failed.exchange(true)) {
                return;
            }
            sync_requests.clear(); // cancellation, NOT successful fence completion
        }
        sync_request_cv.notify_all();
        LOG_ERROR(HW_GPU, "DrawToken GPU service aborted; requesting session exit (no replay)");
        system.Exit();
    }

    [[nodiscard]] u64 GetTicks() const {
        const u64 gpu_tick = system.CoreTiming().GetGPUTicks();
        return gpu_tick / GpuClockMultiplier(Settings::values.gpu_clock.GetValue());
    }

    void RendererFrameEndNotify() {
        system.GetPerfStats().EndGameFrame();
    }

    /// Performs any additional setup necessary in order to begin GPU emulation.
    /// This can be used to launch any necessary threads and register any necessary
    /// core timing events.
    void Start() {
        Settings::UpdateGPUAccuracy();
        gpu_thread.StartThread(*renderer, renderer->Context(), scheduler);
    }

    void NotifyShutdown() {
        std::unique_lock lk{sync_mutex};
        shutting_down.store(true, std::memory_order::relaxed);
        sync_cv.notify_all();
    }

    /// Obtain the CPU Context
    void ObtainContext() {
        if (!cpu_context) {
            cpu_context = renderer->GetRenderWindow().CreateSharedContext();
        }
        cpu_context->MakeCurrent();
    }

    /// Release the CPU Context
    void ReleaseContext() {
        cpu_context->DoneCurrent();
    }

    /// Push GPU command entries to be processed
    void PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {
        gpu_thread.SubmitList(channel, std::move(entries), is_async);
    }

    /// Notify rasterizer that any caches of the specified region should be flushed to Switch memory
    void FlushRegion(DAddr addr, u64 size) {
        gpu_thread.FlushRegion(addr, size, is_async);
    }

    VideoCore::RasterizerDownloadArea OnCPURead(DAddr addr, u64 size) {
        const u64 flusharea_start = static_cast<u64>(Common::g_wall_clock.GetUptime());
        auto raster_area = renderer->ReadRasterizer()->GetFlushArea(addr, size);
        ocr_flusharea_cyc += static_cast<u64>(Common::g_wall_clock.GetUptime()) - flusharea_start;
        if (raster_area.preemtive) {
            ++ocr_preemtive_hits;
            return raster_area;
        }
        raster_area.preemtive = true;
        ++ocr_sync_flushes;
        // On this host GetUptime uses FencedRDTSC; conversion is deferred to logging.
        const u64 syncwait_start = static_cast<u64>(Common::g_wall_clock.GetUptime());
        const u64 fence = RequestSyncOperation([this, &raster_area]() {
            renderer->ReadRasterizer()->FlushRegion(raster_area.start_address, raster_area.end_address - raster_area.start_address);
        });
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(fence);
        ocr_syncwait_cyc += static_cast<u64>(Common::g_wall_clock.GetUptime()) - syncwait_start;
        return raster_area;
    }

    /// Notify rasterizer that any caches of the specified region should be invalidated
    void InvalidateRegion(DAddr addr, u64 size) {
        gpu_thread.InvalidateRegion(addr, size);
    }

    bool OnCPUWrite(DAddr addr, u64 size) {
        return renderer->ReadRasterizer()->OnCPUWrite(addr, size);
    }

    /// Notify rasterizer that any caches of the specified region should be flushed and invalidated
    void FlushAndInvalidateRegion(DAddr addr, u64 size) {
        gpu_thread.FlushAndInvalidateRegion(addr, size, is_async);
    }

    void RequestComposite(std::vector<Tegra::FramebufferConfig>&& layers, std::vector<Service::Nvidia::NvFence>&& fences) {
        const size_t num_fences{fences.size()};
        size_t current_request_counter{};
        if (num_fences != 0) {
            std::unique_lock<std::mutex> lk(request_swap_mutex);
            if (free_swap_counters.empty()) {
                current_request_counter = request_swap_counters.size();
                request_swap_counters.emplace_back(num_fences);
            } else {
                current_request_counter = free_swap_counters.front();
                request_swap_counters[current_request_counter] = num_fences;
                free_swap_counters.pop_front();
            }
        }
        pending_composite_fence = RequestSyncOperation(
            [this, current_request_counter, num_fences, composite_layers = std::move(layers),
             composite_fences = std::move(fences)] {
                if (num_fences == 0) {
                    renderer->Composite(composite_layers);
                    return;
                }
                auto& syncpoint_manager = system.Host1x().GetSyncpointManager();
                const auto executer = [this, current_request_counter, composite_layers]() {
                    {
                        std::unique_lock<std::mutex> lk(request_swap_mutex);
                        if (--request_swap_counters[current_request_counter] != 0) {
                            return;
                        }
                        free_swap_counters.push_back(current_request_counter);
                    }
                    if (renderer->ReadRasterizer()->UsesGPUServiceHandoff()) {
                        // Guest actions can run under the syncpoint lock, on a
                        // foreign thread. Never drain/wait or produce there.
                        const auto fence = RequestSyncOperation([this, composite_layers] {
                            renderer->Composite(composite_layers);
                        });
                        (void)fence;
                        if (!gpu_thread.IsGPUThread()) {
                            gpu_thread.WakeGPUService();
                        }
                    } else {
                        renderer->Composite(composite_layers);
                    }
                };
                for (size_t i = 0; i < num_fences; i++) {
                    syncpoint_manager.RegisterGuestAction(composite_fences[i].id,
                                                          composite_fences[i].value, executer);
                }
            });
        gpu_thread.TickGPU(is_async);
    }

    void WaitForComposite() {
        const u64 fence = pending_composite_fence;
        if (fence == 0) {
            return;
        }
        pending_composite_fence = 0;
        if (shutting_down.load(std::memory_order_relaxed)) {
            return;
        }
        WaitForSyncOperation(fence);
    }

    std::vector<u8> GetAppletCaptureBuffer() {
        std::vector<u8> out;

        const auto wait_fence =
            RequestSyncOperation([&] { out = renderer->GetAppletCaptureBuffer(); });
        gpu_thread.TickGPU(is_async);
        WaitForSyncOperation(wait_fence);

        return out;
    }

    Core::System& system;

    std::unique_ptr<VideoCore::RendererBase> renderer;
    const bool use_nvdec;

    s32 new_channel_id{1};
    /// Shader build notifier
    VideoCore::ShaderNotify shader_notify;
    /// When true, we are about to shut down emulation session, so terminate outstanding tasks
    std::atomic_bool shutting_down{};
    std::atomic_bool renderer_failed{};
    bool servicing_sync{}; // GPU thread only; suppress nested Stage-4 TickWork

    std::array<std::atomic<u32>, Service::Nvidia::MaxSyncPoints> syncpoints{};

    std::array<std::list<u32>, Service::Nvidia::MaxSyncPoints> syncpt_interrupts;

    std::mutex sync_mutex;
    std::mutex device_mutex;

    std::condition_variable sync_cv;

    std::list<std::function<void()>> sync_requests;
    std::atomic<u64> current_sync_fence{};
    u64 last_sync_fence{};
    std::mutex sync_request_mutex;
    std::condition_variable sync_request_cv;

    const bool is_async;

    VideoCommon::GPUThread::ThreadManager gpu_thread;
    std::unique_ptr<Core::Frontend::GraphicsContext> cpu_context;

    Tegra::Control::Scheduler scheduler;
    ::Common::unordered_map<s32, std::shared_ptr<Tegra::Control::ChannelState>> channels;
    Tegra::Control::ChannelState* current_channel;
    s32 bound_channel{-1};

    std::deque<size_t> free_swap_counters;
    std::deque<size_t> request_swap_counters;
    std::mutex request_swap_mutex;
    u64 pending_composite_fence{};
};

GPU::GPU(Core::System& system, bool is_async, bool use_nvdec)
    : impl{std::make_unique<Impl>(system, is_async, use_nvdec)}
{}

GPU::~GPU() = default;

std::shared_ptr<Control::ChannelState> GPU::AllocateChannel() {
    return impl->AllocateChannel();
}

void GPU::InitChannel(Control::ChannelState& to_init, u64 program_id) {
    impl->InitChannel(to_init, program_id);
}

void GPU::BindChannel(s32 channel_id) {
    impl->BindChannel(channel_id);
}

void GPU::ReleaseChannel(Control::ChannelState& to_release) {
    impl->ReleaseChannel(to_release);
}

void GPU::InitAddressSpace(Tegra::MemoryManager& memory_manager) {
    impl->InitAddressSpace(memory_manager);
}

void GPU::BindRenderer(std::unique_ptr<VideoCore::RendererBase> renderer) {
    impl->BindRenderer(std::move(renderer));
}

void GPU::FlushCommands() {
    impl->FlushCommands();
}

void GPU::InvalidateGPUCache() {
    impl->InvalidateGPUCache();
}

void GPU::OnCommandListEnd() {
    impl->OnCommandListEnd();
}

u64 GPU::RequestFlush(DAddr addr, std::size_t size) {
    return impl->RequestSyncOperation([this, addr, size]() {
        impl->renderer->ReadRasterizer()->FlushRegion(addr, size);
    });
}

u64 GPU::CurrentSyncRequestFence() const {
    return impl->CurrentSyncRequestFence();
}

void GPU::WaitForSyncOperation(u64 fence) {
    return impl->WaitForSyncOperation(fence);
}

void GPU::TickWork() {
    impl->TickWork();
}

bool GPU::IsGPUThread() const {
    return impl->gpu_thread.IsGPUThread();
}

void GPU::RunGPUService(std::function<void()> action) {
    if (IsGPUThread()) {
        impl->renderer->ReadRasterizer()->PrepareGPUService();
        action();
        return;
    }
    const u64 fence = impl->RequestSyncOperation(std::move(action));
    impl->gpu_thread.TickGPU(true);
    impl->WaitForSyncOperation(fence);
}

void GPU::NotifyRendererFailure() {
    impl->NotifyRendererFailure();
}

bool GPU::HasRendererFailure() const {
    return impl->renderer_failed.load();
}

/// Gets a mutable reference to the Host1x interface
Host1x::Host1x& GPU::Host1x() {
    return impl->system.Host1x();
}

/// Gets an immutable reference to the Host1x interface.
const Host1x::Host1x& GPU::Host1x() const {
    return impl->system.Host1x();
}

Engines::Maxwell3D& GPU::Maxwell3D() {
    return impl->current_channel->payload->maxwell_3d;
}

const Engines::Maxwell3D& GPU::Maxwell3D() const {
    return impl->current_channel->payload->maxwell_3d;
}

Engines::KeplerCompute& GPU::KeplerCompute() {
    return impl->current_channel->payload->kepler_compute;
}

const Engines::KeplerCompute& GPU::KeplerCompute() const {
    return impl->current_channel->payload->kepler_compute;
}

Tegra::DmaPusher& GPU::DmaPusher() {
    return impl->current_channel->payload->dma_pusher;
}

const Tegra::DmaPusher& GPU::DmaPusher() const {
    return impl->current_channel->payload->dma_pusher;
}

VideoCore::RendererBase& GPU::Renderer() {
    return *impl->renderer;
}

const VideoCore::RendererBase& GPU::Renderer() const {
    return *impl->renderer;
}

VideoCore::ShaderNotify& GPU::ShaderNotify() {
    return impl->shader_notify;
}

const VideoCore::ShaderNotify& GPU::ShaderNotify() const {
    return impl->shader_notify;
}

void GPU::RequestComposite(std::vector<Tegra::FramebufferConfig>&& layers,
                           std::vector<Service::Nvidia::NvFence>&& fences) {
    impl->RequestComposite(std::move(layers), std::move(fences));
}

void GPU::WaitForComposite() {
    impl->WaitForComposite();
}

std::vector<u8> GPU::GetAppletCaptureBuffer() {
    return impl->GetAppletCaptureBuffer();
}

u64 GPU::GetTicks() const {
    return impl->GetTicks();
}

bool GPU::IsAsync() const {
    return impl->is_async;
}

bool GPU::UseNvdec() const {
    return impl->use_nvdec;
}

void GPU::RendererFrameEndNotify() {
    impl->RendererFrameEndNotify();
}

void GPU::Start() {
    impl->Start();
}

void GPU::NotifyShutdown() {
    impl->NotifyShutdown();
}

void GPU::WaitForIdle() {
    impl->WaitForIdle();
}

void GPU::ObtainContext() {
    impl->ObtainContext();
}

void GPU::ReleaseContext() {
    impl->ReleaseContext();
}

void GPU::PushGPUEntries(s32 channel, Tegra::CommandList&& entries) {
    impl->PushGPUEntries(channel, std::move(entries));
}

VideoCore::RasterizerDownloadArea GPU::OnCPURead(PAddr addr, u64 size) {
    return impl->OnCPURead(addr, size);
}

void GPU::FlushRegion(DAddr addr, u64 size) {
    impl->FlushRegion(addr, size);
}

void GPU::InvalidateRegion(DAddr addr, u64 size) {
    impl->InvalidateRegion(addr, size);
}

bool GPU::OnCPUWrite(DAddr addr, u64 size) {
    return impl->OnCPUWrite(addr, size);
}

void GPU::FlushAndInvalidateRegion(DAddr addr, u64 size) {
    impl->FlushAndInvalidateRegion(addr, size);
}

} // namespace Tegra
