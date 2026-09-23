// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <memory>
#include <algorithm>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <fmt/format.h>

#include "video_core/renderer_vulkan/vk_query_cache.h"

#include "common/settings.h"
#include "common/thread.h"
#include "video_core/gpu_logging/gpu_logging.h"
#include "video_core/renderer_vulkan/vk_command_pool.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

thread_local Scheduler::ResolverCaptureContext* Scheduler::active_capture = nullptr;
thread_local Scheduler* Scheduler::resolver_record_owner = nullptr;

void Scheduler::CheckLoanedProducer(std::source_location caller) const {
    const bool valid = resolver_record_owner == this;
    ASSERT_MSG(valid, "DrawToken semantic producer used without tail FIFO handoff: {}:{} {}",
               caller.file_name(), caller.line(), caller.function_name());
    if (!valid) {
        throw std::logic_error(fmt::format("DrawToken scheduler producer ownership violation: {}:{} {}",
                                          caller.file_name(), caller.line(), caller.function_name()));
    }
}

Scheduler::ResolverThreadScope::ResolverThreadScope(Scheduler& scheduler_)
    : scheduler{scheduler_} {
    const bool valid = resolver_record_owner == nullptr && active_capture == nullptr &&
                       std::this_thread::get_id() != scheduler.worker_thread.get_id();
    ASSERT_MSG(valid, "DrawToken worker record scope must be exclusive");
    if (!valid) {
        throw std::logic_error("DrawToken worker record scope conflict");
    }
    resolver_record_owner = &scheduler;
}

Scheduler::ResolverThreadScope::~ResolverThreadScope() {
    ASSERT_MSG(resolver_record_owner == &scheduler && active_capture == nullptr,
               "DrawToken worker record scope leaked capture state");
    resolver_record_owner = nullptr;
}

void Scheduler::RecordResolverCommandAbiV2(void* command,
                                          bool (*record)(CommandChunk&, void*)) {
    auto* capture = active_capture;
    const bool valid = resolver_record_owner == this && capture &&
                       &capture->scheduler == this && capture->bridge &&
                       capture->batch.owner == this && capture->job_id != 0 &&
                       capture->batch.job_id == capture->job_id && capture->batch.capturing &&
                       !capture->batch.handed_off &&
                       capture->batch.producer == std::this_thread::get_id();
    ASSERT_MSG(valid, "DrawToken worker Record requires an owned, active capture");
    if (!valid) {
        // Soft assertion alone may continue. Reject the job rather than
        // silently recording into the GPU producer's chunk or dropping work.
        throw std::logic_error("DrawToken worker Record outside capture");
    }
    auto& batch = capture->batch;
    if (!batch.current) {
        batch.current = std::make_unique<CommandChunk>(true);
    }
    const size_t before = batch.current->UsedBytes();
    if (record(*batch.current, command)) {
        batch.captured_bytes += batch.current->UsedBytes() - before;
        return;
    }
    batch.SealChunk();
    batch.current = std::make_unique<CommandChunk>(true);
    const bool recorded = record(*batch.current, command);
    ASSERT_MSG(recorded, "DrawToken worker command must fit in an empty chunk");
    if (!recorded) {
        throw std::logic_error("DrawToken worker command exceeds chunk capacity");
    }
    batch.captured_bytes += batch.current->UsedBytes();
}

Scheduler::CaptureScope::CaptureScope(Scheduler& scheduler, CapturedBatch& batch, u64 job_id,
                                     CaptureSyncBridge* bridge, std::thread::id receiver)
    : context{scheduler, batch, job_id, bridge} {
    ASSERT(active_capture == nullptr); // No nested captures, even across schedulers.
    ASSERT(batch.owner == nullptr && job_id != 0);
    ASSERT(std::this_thread::get_id() != scheduler.worker_thread.get_id());
    batch.owner = &scheduler;
    batch.job_id = job_id;
    batch.producer = std::this_thread::get_id();
    batch.receiver = bridge ? receiver : batch.producer;
    ASSERT(batch.receiver != std::thread::id{});
    ASSERT(!bridge || batch.receiver != batch.producer);
    batch.capturing = true;
    active_capture = &context;
}

Scheduler::CaptureScope::~CaptureScope() {
    ASSERT(active_capture == &context);
    ASSERT(context.batch.owner == &context.scheduler);
    ASSERT(context.batch.job_id == context.job_id);
    ASSERT(context.batch.producer == std::this_thread::get_id());
    context.batch.capturing = false;
    active_capture = nullptr;
}

Scheduler::ResolverCaptureContext* Scheduler::ActiveCapture(CaptureAbiV2) {
    if (active_capture) {
        ASSERT(&active_capture->scheduler == this);
        ASSERT(active_capture->batch.owner == this);
        ASSERT(active_capture->batch.job_id == active_capture->job_id);
        ASSERT(active_capture->batch.capturing);
        ASSERT(!active_capture->batch.handed_off);
        ASSERT(active_capture->batch.producer == std::this_thread::get_id());
    }
    return active_capture;
}

void Scheduler::CommandChunk::DiscardAll() noexcept {
    while (first) {
        auto* next = first->GetNext();
        first->~Command();
        first = next;
    }
    last = nullptr;
    command_offset = 0;
    submit = false;
}

Scheduler::CapturedBatch::~CapturedBatch() {
    ASSERT(!capturing);
    // Private CommandChunk destructors release unexecuted captures, including
    // chunks still queued when the scheduler shuts down. Executed chunks have
    // an empty command list; no lambda is destroyed twice.
}

void Scheduler::CapturedBatch::SealChunk() {
    if (current && !current->Empty()) {
        sealed.emplace_back(std::move(current));
        ++captured_chunks;
        chunk_high_water = (std::max)(chunk_high_water, static_cast<u64>(sealed.size()));
    }
}

void Scheduler::SpliceCaptured(CapturedBatch& batch, u64 job_id) {
    ASSERT(active_capture == nullptr && !batch.capturing);
    ASSERT(batch.owner == this && batch.job_id == job_id);
    ASSERT(batch.receiver == std::this_thread::get_id());
    ASSERT(batch.producer == batch.receiver || batch.handed_off);
    batch.SealChunk();
    if (batch.sealed.empty()) {
        return;
    }
    // Publish pre-capture commands first. DispatchWork installs a replacement
    // before publication; captured chunks never enter the producer's arena.
    DispatchWork();
    {
        std::scoped_lock ql{queue_mutex};
        for (auto& entry : batch.sealed) {
            work_queue.push(std::move(entry));
        }
        // This boundary describes a successfully published PREFIX, not a tick
        // merely allocated by the emitting thread. Partial push failure poisons
        // the FIFO and cannot authorize a wait/replay.
        if (batch.has_submission) {
            published_submission_tick = (std::max)(published_submission_tick, batch.submission_tick);
            has_published_submission = true;
        }
    }
    batch.sealed.clear();
    ++batch.splice_count;
    event_cv.notify_all();
}

void Scheduler::AdoptCapturedReceiverForTeardown(CapturedBatch& batch, u64 job_id,
                                                 std::thread::id former_receiver) {
    const auto receiver = std::this_thread::get_id();
    const bool valid = active_capture == nullptr && resolver_record_owner == nullptr &&
                       batch.owner == this && batch.job_id == job_id &&
                       !batch.capturing && batch.handed_off && batch.producer != receiver &&
                       (batch.receiver == former_receiver || batch.receiver == receiver);
    ASSERT_MSG(valid, "DrawToken teardown requires exclusive handed-off batch ownership");
    if (!valid) {
        throw std::logic_error("DrawToken invalid teardown receiver transfer");
    }
    batch.receiver = receiver;
}

void Scheduler::ReleaseCaptured(CapturedBatch& batch, u64 job_id) {
    ASSERT(active_capture == nullptr && !batch.capturing && !batch.handed_off);
    ASSERT(batch.owner == this && batch.job_id == job_id);
    ASSERT(batch.producer == std::this_thread::get_id());
    batch.SealChunk();
    batch.handed_off = true;
    ++batch.prefix_sequence;
}

bool Scheduler::BridgeCaptureSync(CaptureSync operation, u64 tick) {
    auto* capture = ActiveCapture();
    if (!capture || !capture->bridge) {
        return false;
    }
    // The bridge parks this producer until the GPU has finished with its
    // prefix. Restore capture ownership on both success and exception paths.
    struct RestoreCapture {
        ResolverCaptureContext*& tls;
        ResolverCaptureContext* saved;
        bool& capturing;
        bool& handed_off;
        ~RestoreCapture() {
            handed_off = false;
            capturing = true;
            tls = saved;
        }
    } restore{active_capture, capture, capture->batch.capturing, capture->batch.handed_off};
    capture->batch.capturing = false;
    active_capture = nullptr;
    ReleaseCaptured(capture->batch, capture->job_id);
    capture->bridge->Request(capture->batch, capture->job_id, operation, tick);
    return true;
}

void Scheduler::DrainCapturePrefix() {
    if (BridgeCaptureSync(CaptureSync::Publish)) {
        return;
    }
    auto* capture = ActiveCapture();
    if (!capture) {
        return;
    }
    // Stage 1A only: we already ARE the GPU producer, so a synchronous API
    // can hand off its prefix directly. No GPU callback, new lock, or wait
    // bridge. Restore TLS even if allocation/queue publication throws.
    struct SuspendCapture {
        ResolverCaptureContext* context;
        SuspendCapture(ResolverCaptureContext* context_) : context{context_} {
            context->batch.capturing = false;
            active_capture = nullptr;
        }
        ~SuspendCapture() {
            active_capture = context;
            context->batch.capturing = true;
        }
    } suspend{capture};
    SpliceCaptured(capture->batch, capture->job_id);
    // WaitWorker must publish pre-capture work even when this private prefix
    // is empty. Outside capture an empty Splice intentionally does nothing.
    DispatchWork();
}

void Scheduler::CommandChunk::ExecuteAll(vk::CommandBuffer cmdbuf,
                                         vk::CommandBuffer upload_cmdbuf) {
    auto command = first;
    while (command != nullptr) {
        auto next = command->GetNext();
        command->Execute(cmdbuf, upload_cmdbuf);
        command->~Command();
        command = next;
    }
    submit = false;
    command_offset = 0;
    first = nullptr;
    last = nullptr;
}

Scheduler::Scheduler(const Device& device_, StateTracker& state_tracker_)
    : device{device_}, state_tracker{state_tracker_},
      master_semaphore{std::make_unique<MasterSemaphore>(device)},
      command_pool{std::make_unique<CommandPool>(*master_semaphore, device)} {

    AcquireNewChunk();
    AllocateWorkerCommandBuffer();
    worker_thread = std::jthread([this](std::stop_token token) { WorkerThread(token); });
}

Scheduler::~Scheduler() = default;

u64 Scheduler::Flush(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    CheckProducer();
    // When flushing, we only send data to the worker thread; no waiting is necessary.
    const u64 signal_value = SubmitExecution(signal_semaphore, wait_semaphore);
    AllocateNewContext();
    return signal_value;
}

void Scheduler::Finish(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    CheckProducer();
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitExecution(signal_semaphore, wait_semaphore);
    Wait(presubmit_tick);
    AllocateNewContext();
}

void Scheduler::WaitWorker() {
    if (BridgeCaptureSync(CaptureSync::WaitWorker)) {
        return;
    }
    // DispatchWork alone only seals a private chunk during capture.
    DrainCapturePrefix();
    DispatchWork();

    // Ensure the queue is drained.
    {
        std::unique_lock ql{queue_mutex};
        event_cv.wait(ql, [this] { return work_queue.empty(); });
    }

    // Now wait for execution to finish.
    std::scoped_lock el{execution_mutex};
}

void Scheduler::DispatchWork() {
    if (auto* capture = ActiveCapture()) {
        capture->batch.SealChunk();
        return;
    }
    if (chunk && !chunk->Empty()) {
        auto work = std::move(chunk);
        const bool submit = work->HasSubmit();
        AcquireNewChunk();
        {
            std::scoped_lock ql{queue_mutex};
            work_queue.push(std::move(work));
            if (submit) {
                published_submission_tick = (std::max)(published_submission_tick, main_submission_tick);
                has_published_submission = true;
            }
        }
        event_cv.notify_all();
    }
}

void Scheduler::BeginRenderPassImpl(const Framebuffer* framebuffer, VkRenderPass renderpass,
                                    const VkClearValue* clear_values, u32 clear_value_count) {
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    state.renderpass = renderpass;
    state.framebuffer = framebuffer_handle;
    state.render_area = render_area;

    if (GPU::Logging::IsActive() && Settings::values.gpu_log_vulkan_calls.GetValue()) {
        const std::string render_pass_info =
            fmt::format("renderArea={}x{}, numImages={}", render_area.width, render_area.height,
                        framebuffer->NumImages());
        GPU::Logging::GPULogger::GetInstance().LogRenderPassBegin(render_pass_info);
    }

    std::array<VkClearValue, 9> values{};
    for (u32 i = 0; i < clear_value_count && i < values.size(); ++i) {
        values[i] = clear_values[i];
    }
    Record([renderpass, framebuffer_handle, render_area, values, clear_value_count](
               vk::CommandBuffer cmdbuf) {
        const VkRenderPassBeginInfo renderpass_bi{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = renderpass,
            .framebuffer = framebuffer_handle,
            .renderArea =
                {
                    .offset = {.x = 0, .y = 0},
                    .extent = render_area,
                },
            .clearValueCount = clear_value_count,
            .pClearValues = clear_value_count != 0 ? values.data() : nullptr,
        };
        cmdbuf.BeginRenderPass(renderpass_bi, VK_SUBPASS_CONTENTS_INLINE);
    });
    num_renderpass_images = framebuffer->NumImages();
    renderpass_images = framebuffer->Images();
    renderpass_image_ranges = framebuffer->ImageRanges();
    framebuffer->MarkResolveShadowsUpToDate();
}

void Scheduler::RealizeDeferredClear() {
    CheckProducer();
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    const DeferredClear dc = deferred_clear;
    deferred_clear = {};

    std::array<VkClearValue, 9> clear_values{};
    u32 count = 0;
    const RenderPassKey& base = dc.framebuffer->RenderPassKeyBase();
    for (u32 slot = 0; slot < 8; ++slot) {
        if (base.color_formats[slot] == VideoCore::Surface::PixelFormat::Invalid) {
            continue;
        }
        clear_values[count++] = dc.color_values[slot];
    }
    if (base.depth_format != VideoCore::Surface::PixelFormat::Invalid) {
        clear_values[count++] = dc.depth_stencil_value;
    }
    const u32 color_discard_mask =
        dc.framebuffer->DiscardsMsaaColor() ? dc.color_clear_mask : 0u;
    const bool depth_stencil_discard =
        dc.depth_stencil && dc.framebuffer->DiscardsMsaaDepthStencil();
    const VkRenderPass renderpass = dc.framebuffer->RenderPassVariant(
        dc.color_clear_mask, dc.depth_stencil, color_discard_mask, depth_stencil_discard);
    EndRenderPass();
    BeginRenderPassImpl(dc.framebuffer, renderpass, clear_values.data(), count);
}

bool Scheduler::DeferColorClear(const Framebuffer* framebuffer, u32 rt_slot,
                                const VkClearValue& value) {
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.color_clear_mask |= 1u << rt_slot;
    deferred_clear.color_values[rt_slot] = value;
    return true;
}

bool Scheduler::DeferDepthStencilClear(const Framebuffer* framebuffer, const VkClearValue& value) {
    CheckProducer();
    if (IsRenderPassActive()) {
        return false;
    }
    if (deferred_clear.framebuffer != nullptr && deferred_clear.framebuffer != framebuffer) {
        RealizeDeferredClear();
        EndRenderPass();
    }
    deferred_clear.framebuffer = framebuffer;
    deferred_clear.depth_stencil = true;
    deferred_clear.depth_stencil_value = value;
    return true;
}

void Scheduler::FlushDeferredClear() {
    CheckProducer();
    if (deferred_clear.framebuffer == nullptr) {
        return;
    }
    RealizeDeferredClear();
    EndRenderPass();
}

void Scheduler::RequestRenderpass(const Framebuffer* framebuffer) {
    CheckProducer();
    if (deferred_clear.framebuffer == framebuffer) {
        RealizeDeferredClear();
        return;
    }
    const VkRenderPass renderpass = framebuffer->RenderPass();
    const VkFramebuffer framebuffer_handle = framebuffer->Handle();
    const VkExtent2D render_area = framebuffer->RenderArea();
    if (renderpass == state.renderpass && framebuffer_handle == state.framebuffer &&
        render_area.width == state.render_area.width &&
        render_area.height == state.render_area.height) {
        return;
    }
    // Ends any active pass and realizes a deferred clear
    EndRenderPass();
    BeginRenderPassImpl(framebuffer, renderpass, nullptr, 0);
}

void Scheduler::RequestOutsideRenderPassOperationContext() {
    CheckProducer();
    EndRenderPass();
}

bool Scheduler::UpdateGraphicsPipeline(GraphicsPipeline* pipeline) {
    CheckProducer();
    if (state.graphics_pipeline == pipeline) {
        if (pipeline && pipeline->UsesExtendedDynamicState() &&
            state.needs_state_enable_refresh) {
            state_tracker.InvalidateStateEnableFlag();
            state.needs_state_enable_refresh = false;
        }
        return false;
    }

    state.graphics_pipeline = pipeline;

    if (!pipeline) {
        return true;
    }

    if (!pipeline->UsesExtendedDynamicState()) {
        state.needs_state_enable_refresh = true;
    } else if (state.needs_state_enable_refresh) {
        state_tracker.InvalidateStateEnableFlag();
        state.needs_state_enable_refresh = false;
    }

    return true;
}

bool Scheduler::UpdateRescaling(bool is_rescaling) {
    CheckProducer();
    if (state.rescaling_defined && is_rescaling == state.is_rescaling) {
        return false;
    }
    state.rescaling_defined = true;
    state.is_rescaling = is_rescaling;
    return true;
}

bool Scheduler::UpdateDescriptorBufferChunk(u32 descriptor_chunk) {
    CheckProducer();
    if (state.descriptor_buffer_bound && descriptor_chunk == state.descriptor_buffer_chunk) {
        return false;
    }
    state.descriptor_buffer_bound = true;
    state.descriptor_buffer_chunk = descriptor_chunk;
    return true;
}

void Scheduler::WorkerThread(std::stop_token stop_token) {
    Common::SetCurrentThreadName("VulkanWorker");
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();

    const auto TryPopQueue{[this](auto& work) -> bool {
        if (work_queue.empty()) {
            return false;
        }

        work = std::move(work_queue.front());
        work_queue.pop();
        event_cv.notify_all();
        return true;
    }};

    while (!stop_token.stop_requested()) {
        std::unique_ptr<CommandChunk> work;

        {
            std::unique_lock lk{queue_mutex};

            // Wait for work.
            event_cv.wait(lk, stop_token, [&] { return TryPopQueue(work); });

            // If we've been asked to stop, we're done.
            if (stop_token.stop_requested()) {
                return;
            }

            // Exchange lock ownership so that we take the execution lock before
            // the queue lock goes out of scope. This allows us to force execution
            // to complete in the next step.
            void(std::exchange(lk, std::unique_lock{execution_mutex}));

            // Perform the work, tracking whether the chunk was a submission
            // before executing.
            const bool has_submit = work->HasSubmit();
            work->ExecuteAll(current_cmdbuf, current_upload_cmdbuf);

            // If the chunk was a submission, reallocate the command buffer.
            if (has_submit) {
                AllocateWorkerCommandBuffer();
            }
        }

        if (!work->IsCaptured()) {
            std::scoped_lock rl{reserve_mutex};

            // Private capture chunks are destroyed after execution. Feeding
            // freshly allocated per-draw chunks into the main reserve would
            // grow it without bound; capture never borrows from that reserve.
            chunk_reserve.emplace_back(std::move(work));
        }
    }
}

void Scheduler::AllocateWorkerCommandBuffer() {
    current_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
    current_upload_cmdbuf = vk::CommandBuffer(command_pool->Commit(), device.GetDispatchLoader());
    current_upload_cmdbuf.Begin({
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    });
}

u64 Scheduler::SubmitExecution(VkSemaphore signal_semaphore, VkSemaphore wait_semaphore) {
    EndPendingOperations();
    InvalidateState();

    const u64 signal_value = master_semaphore->NextTick();
    RecordWithUploadBuffer([signal_semaphore, wait_semaphore, signal_value,
                            this](vk::CommandBuffer cmdbuf, vk::CommandBuffer upload_cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        };
        upload_cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, WRITE_BARRIER);
        upload_cmdbuf.End();
        cmdbuf.End();

        if (on_submit) {
            on_submit();
        }

        std::scoped_lock lock{submit_mutex};
        switch (const VkResult result = master_semaphore->SubmitQueue(
                    cmdbuf, upload_cmdbuf, signal_semaphore, wait_semaphore, signal_value)) {
        case VK_SUCCESS:
            // Log successful queue submission
            if (GPU::Logging::IsActive() &&
                Settings::values.gpu_log_vulkan_calls.GetValue()) {
                GPU::Logging::GPULogger::GetInstance().LogVulkanCall(
                    "vkQueueSubmit", "", VK_SUCCESS);
            }
            break;
        case VK_ERROR_DEVICE_LOST:
            device.ReportLoss();
            [[fallthrough]];
        default:
            vk::Check(result);
            break;
        }
    });
    if (auto* capture = ActiveCapture()) {
        // Submit must terminate this private chunk: the worker allocates its
        // next command buffers after executing a chunk carrying HasSubmit.
        ASSERT(capture->batch.current != nullptr);
        capture->batch.current->MarkSubmit();
        capture->batch.submission_tick = signal_value;
        capture->batch.has_submission = true;
        capture->batch.SealChunk();
        DrainCapturePrefix();
    } else {
        chunk->MarkSubmit();
        main_submission_tick = signal_value;
        DispatchWork();
    }
    return signal_value;
}

void Scheduler::AllocateNewContext() {
    // Enable counters once again. These are disabled when a command buffer is finished.
}

void Scheduler::InvalidateState() {
    CheckProducer();
    state.graphics_pipeline = nullptr;
    state.rescaling_defined = false;
    state.descriptor_buffer_bound = false;
    state_tracker.InvalidateCommandBufferState();
}

void Scheduler::EndPendingOperations() {
    query_cache->CounterReset(VideoCommon::QueryType::ZPassPixelCount64);
    EndRenderPass();
}

void Scheduler::EndRenderPass()
    {
        RealizeDeferredClear();
        if (!state.renderpass) {
            return;
        }

        query_cache->CounterClose(VideoCommon::QueryType::StreamingByteCount);

        // Log render pass end
        if (GPU::Logging::IsActive() &&
            Settings::values.gpu_log_vulkan_calls.GetValue()) {
            GPU::Logging::GPULogger::GetInstance().LogRenderPassEnd();
        }

        query_cache->CounterEnable(VideoCommon::QueryType::ZPassPixelCount64, false);
        query_cache->NotifySegment(false);

        Record([num_images = num_renderpass_images,
                       images = renderpass_images,
                       ranges = renderpass_image_ranges,
                       has_transform_feedback = device.IsExtTransformFeedbackSupported()](
                          vk::CommandBuffer cmdbuf) {
            std::array<VkImageMemoryBarrier, 9> barriers;
            for (size_t i = 0; i < num_images; ++i) {
                const VkImageSubresourceRange& range = ranges[i];
                const bool is_color = (range.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) != 0;
                const bool is_depth_stencil = (range.aspectMask
                                              & (VK_IMAGE_ASPECT_DEPTH_BIT
                                                 | VK_IMAGE_ASPECT_STENCIL_BIT)) !=0;

                VkAccessFlags src_access = 0;

                if (is_color)
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                else if (is_depth_stencil)
                    src_access |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
                else
                    src_access |= VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                  | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

                barriers[i] = VkImageMemoryBarrier{
                        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                        .pNext = nullptr,
                        .srcAccessMask = src_access,
                        .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT
                                         | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                        .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = images[i],
                        .subresourceRange = range,
                };
            }
            cmdbuf.EndRenderPass();
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT |
                                   VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                                   0, nullptr, nullptr, vk::Span(barriers.data(), num_images));
            if (has_transform_feedback) {
                static constexpr VkMemoryBarrier XFB_OUTPUT_BARRIER{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFORM_FEEDBACK_WRITE_BIT_EXT,
                    .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFORM_FEEDBACK_BIT_EXT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       0, XFB_OUTPUT_BARRIER);
            }
        });

        state.renderpass = VkRenderPass{};
        num_renderpass_images = 0;
    }


void Scheduler::AcquireNewChunk() {
    std::scoped_lock rl{reserve_mutex};

    if (chunk_reserve.empty()) {
        // If we don't have anything reserved, we need to make a new chunk.
        chunk = std::make_unique<CommandChunk>();
    } else {
        // Otherwise, we can just take from the reserve.
        chunk = std::move(chunk_reserve.back());
        chunk_reserve.pop_back();
    }
}

} // namespace Vulkan
