// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_draw_resolver.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

#include "common/logging.h"
#include "common/thread.h"
#include "video_core/control/engine_override.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"

namespace Vulkan {

using VideoCommon::tls_engine_snapshot;

namespace {
using Clock = std::chrono::steady_clock;
}

// Single-job mailbox. Mutex J protects requests/completion, not cache work.
// GPU releases J before servicing a request; resolver releases J before resolve
// and before every wait. No path holding J acquires B/T or scheduler locks.
struct DrawResolver::WorkerState final : Scheduler::CaptureSyncBridge {
    struct RequestData {
        Scheduler::CapturedBatch* batch;
        u64 job_id;
        Scheduler::CaptureSync operation;
        u64 tick;
    };

    explicit WorkerState(DrawResolver& owner_)
        : owner{owner_}, gpu_thread{std::this_thread::get_id()} {
        thread = std::jthread([this](std::stop_token stop) { Run(stop); });
    }

    ~WorkerState() {
        // DrawResolver drains active jobs before reaching this destructor.
        ASSERT(!active);
        thread.request_stop();
        cv.notify_all();
        thread.join();
    }

    void Kick(Scheduler& scheduler_) {
        ASSERT(std::this_thread::get_id() == gpu_thread && !active);
        std::scoped_lock lock{mutex};
        ASSERT(!batch && !request && !pending);
        scheduler = &scheduler_;
        job_id = owner.diag_kicks;
        batch.emplace();
        error = nullptr;
        sync_error = nullptr;
        done = false;
        active = true;
        pending = true;
        cv.notify_all();
    }

    void Request(Scheduler::CapturedBatch& prefix, u64 requested_job,
                 Scheduler::CaptureSync operation, u64 tick) override {
        ASSERT(std::this_thread::get_id() == thread.get_id());
        std::unique_lock lock{mutex};
        ASSERT(!request && !done);
        ASSERT(batch && &prefix == &*batch && requested_job == job_id);
        ++owner.diag_worker_sync_requests;
        request = RequestData{&prefix, requested_job, operation, tick};
        cv.notify_all();
        cv.wait(lock, [this] { return !request; });
        auto failure = std::exchange(sync_error, nullptr);
        lock.unlock();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    void Wait() {
        if (!active) {
            // Quiescent teardown may run on a different thread after the GPU
            // producer has been joined. It needs no scheduler service.
            return;
        }
        ASSERT(std::this_thread::get_id() == gpu_thread);
        std::unique_lock lock{mutex};
        while (!done) {
            cv.wait(lock, [this] { return done || request.has_value(); });
            if (!request) {
                continue;
            }
            const auto service = *request;
            lock.unlock();
            std::exception_ptr failure;
            try {
                // Resolver remains parked (possibly holding B/T). These
                // operations neither acquire cache locks nor enter the tail.
                scheduler->SpliceCaptured(*service.batch, service.job_id);
                switch (service.operation) {
                case Scheduler::CaptureSync::Publish:
                    scheduler->DispatchWork();
                    break;
                case Scheduler::CaptureSync::WaitWorker:
                    scheduler->WaitWorker();
                    break;
                case Scheduler::CaptureSync::WaitTick:
                    // Worker already performed any required Flush with its
                    // snapshot TLS. Do not run EndPendingOperations here.
                    ASSERT(service.tick < scheduler->CurrentTick());
                    scheduler->GetMasterSemaphore().Wait(service.tick);
                    break;
                }
            } catch (...) {
                failure = std::current_exception();
            }
            lock.lock();
            sync_error = failure;
            request.reset();
            cv.notify_all();
        }
        auto failure = std::exchange(error, nullptr);
        active = false;
        lock.unlock();
        try {
            if (failure) {
                std::rethrow_exception(failure);
            }
            scheduler->SpliceCaptured(*batch, job_id);
            ++owner.diag_capture_batches;
            owner.diag_captured_bytes += batch->CapturedBytes();
            owner.diag_splice_count += batch->SpliceCount();
            ++owner.diag_worker_resolves;
            batch.reset();
            owner.job_phase.store(Phase::Resolved, std::memory_order_release);
        } catch (...) {
            // Fail the job; never replay an already submitted prefix inline.
            batch.reset();
            owner.job_phase.store(Phase::Idle, std::memory_order_release);
            throw;
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("DrawResolver");
        for (;;) {
            std::unique_lock lock{mutex};
            cv.wait(lock, stop, [this] { return pending; });
            if (stop.stop_requested() && !pending) {
                return;
            }
            pending = false;
            lock.unlock();
            std::exception_ptr failure;
            try {
                Scheduler::ResolverThreadScope record_scope{*scheduler};
                owner.ExecuteResolveImpl(*scheduler, this);
            } catch (...) {
                failure = std::current_exception();
            }
            lock.lock();
            error = failure;
            done = true; // publishes batch, epoch, diagnostics and scheduler state
            cv.notify_all();
        }
    }

    DrawResolver& owner;
    const std::thread::id gpu_thread;
    Scheduler* scheduler{};
    u64 job_id{};
    std::optional<Scheduler::CapturedBatch> batch;
    std::mutex mutex;
    std::condition_variable_any cv;
    std::optional<RequestData> request;
    std::exception_ptr error;
    std::exception_ptr sync_error;
    bool pending{};
    bool done{};
    bool active{}; // GPU-only; worker observes pending/done instead
    std::jthread thread; // last; explicit stop/notify/join before mailbox destruction
};

DrawResolver::DrawResolver(Tegra::MemoryManager& gpu_memory_, BufferCache& buffer_cache_,
                           TextureCache& texture_cache_)
    : gpu_memory{gpu_memory_}, buffer_cache{buffer_cache_}, texture_cache{texture_cache_},
      shadow{std::make_unique<Tegra::Engines::Maxwell3D>(gpu_memory_)} {}

DrawResolver::~DrawResolver() {
    WaitResolved();
    worker.reset();
}

void DrawResolver::WaitResolved() {
    if (worker) {
        worker->Wait();
        return;
    }
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

void DrawResolver::ExecuteResolve(Scheduler& scheduler) {
    if (worker_enabled) {
        try {
            if (!worker) {
                worker = std::make_unique<WorkerState>(*this);
            }
            worker->Kick(scheduler);
            WaitResolved(); // mandatory 1B rendezvous; no GPU work may overlap resolve
        } catch (...) {
            // Also cover thread-creation failure before a job is dispatched;
            // teardown must not spin forever on the enqueue's Resolving state.
            job_phase.store(Phase::Idle, std::memory_order_release);
            throw;
        }
        return;
    }
    ExecuteResolveImpl(scheduler, nullptr);
}

void DrawResolver::ExecuteResolveImpl(Scheduler& scheduler, WorkerState* worker_state) {
    std::optional<Scheduler::CapturedBatch> inline_batch;
    if (!worker_state && batch_enabled) {
        inline_batch.emplace();
    }
    auto* batch = worker_state ? &*worker_state->batch
                              : (inline_batch ? &*inline_batch : nullptr);
    // Restore TLS on exceptions too; a persistent resolver thread must never
    // retain a previous job's snapshot after an aborted resolve.
    struct RestoreSnapshot {
        Tegra::Engines::Maxwell3D* previous{tls_engine_snapshot};
        ~RestoreSnapshot() { tls_engine_snapshot = previous; }
    } restore_snapshot;
    tls_engine_snapshot = shadow.get();
    {
        std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
        std::optional<Scheduler::CaptureScope> capture;
        if (batch) {
            capture.emplace(scheduler, *batch, diag_kicks, worker_state,
                            worker_state ? worker_state->gpu_thread : std::thread::id{});
        }
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
        // Layout comes from the (immutable) pipeline, never channel state:
        // The 1B GPU producer is parked; overlapped resolve remains disabled.
        job.epoch_valid = false;
        if (epoch_enabled) {
            job.epoch_entry_count = buffer_cache.CaptureUniformEpoch(
                *shadow, job.pipeline->UniformBufferMasks(),
                job.pipeline->UniformBufferSizeTable(), epoch_table,
                job.epoch_entries.data(), job.epoch_entries.size());
            job.epoch_snapshot = {job.epoch_entries.data(), job.epoch_entry_count,
                                  epoch_table.bytes.data()};
            job.epoch_valid = true;
        }
    }
    tls_engine_snapshot = nullptr;
    if (worker_state) {
        scheduler.ReleaseCaptured(*batch, diag_kicks);
        ++diag_resolve_calls;
        return; // WorkerState publishes completion; GPU owns final splice/stats.
    }
    if (batch) {
        // Capture scope and both cache locks have ended. Merge immediately:
        // deferring this to CommitPendingDraw would let intervening records
        // overtake resolve commands. Both tail modes run after this point.
        scheduler.SpliceCaptured(*batch, diag_kicks);
        ++diag_capture_batches;
        diag_captured_bytes += batch->CapturedBytes();
        diag_splice_count += batch->SpliceCount();
    }
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
