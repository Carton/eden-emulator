// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "video_core/renderer_vulkan/vk_draw_resolver.h"

#include <algorithm>
#include <bit>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#include <intrin.h>
#endif

#include "common/logging.h"
#include "common/thread.h"
#include "video_core/control/engine_override.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/renderer_vulkan/vk_texture_cache.h"

namespace Vulkan {

using VideoCommon::tls_engine_snapshot;

namespace {
using Clock = std::chrono::steady_clock;

void ResolverPause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::atomic_signal_fence(std::memory_order_seq_cst);
#endif
}

// Predicates in these loops load atomics only. Mutex/cache access happens
// after the loop exits, including when a synchronization request is observed.
template <typename Predicate>
bool SpinUntil(u32 budget_us, Predicate ready) {
    if (budget_us == 0) {
        return false;
    }
    const auto end = Clock::now() + std::chrono::microseconds{budget_us};
    do {
        if (ready()) {
            return true;
        }
        ResolverPause();
    } while (Clock::now() < end);
    return ready();
}
} // namespace

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
            if (owner.tail_worker_enabled) {
                LOG_ERROR(Render_Vulkan,
                          "DrawToken worker tail failed: job={} completed_splices={}; "
                          "publication may be partial, replay forbidden",
                          job_id, batch ? batch->SpliceCount() : 0);
            }
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

// Conservative 2A: multiple immutable snapshots may queue, but resolve(N+1)
// cannot overwrite shared cache/scheduler producer state before tail(N).
// Only the GPU arms the FIFO head, at Kick/Wait after its own cache work ends.
struct DrawResolver::PipelineState final : Scheduler::CaptureSyncBridge {
    enum class Status : u32 { Queued, Complete, Failed };
    struct Entry {
        explicit Entry(Tegra::MemoryManager& memory)
            : engine{std::make_unique<Tegra::Engines::Maxwell3D>(memory)} {}
        Job job;
        std::unique_ptr<Tegra::Engines::Maxwell3D> engine;
        Tegra::Engines::Maxwell3D::Regs expected_regs{};
        // GPU-only catch-up journal. Never replay into an in-flight engine.
        std::array<Tegra::Engines::Maxwell3D::JournalEntry,
                   Tegra::Engines::Maxwell3D::JournalCapacity> pending_journal;
        size_t pending_journal_size{};
        bool registers_valid{};
        VideoCommon::UniformEpochTable epoch;
        std::optional<Scheduler::CapturedBatch> batch;
        Scheduler* scheduler{};
        std::exception_ptr error;
        std::atomic<Status> status{Status::Queued};
        u64 id{};
        u64 copies{}, skips{}, overflows{}, sync_requests{};
        std::chrono::nanoseconds resolve_ns{};
        Clock::time_point enqueued_at{}, emitted_at{};
        std::chrono::nanoseconds enqueue_latency{};
        u64 prefix_published{};
        u64 epoch_hits{}, epoch_misses{}, epoch_classic{};
        bool armed{}, spliced{}; // GPU-only
    };
    struct RequestData {
        Entry* entry;
        Scheduler::CaptureSync operation;
        u64 tick;
    };

    explicit PipelineState(DrawResolver& owner_)
        : owner{owner_}, gpu_thread{std::this_thread::get_id()}, depth{owner.pipeline_depth} {
        ASSERT(depth >= 1 && depth <= MaxPipelineDepth);
        if (depth < 1 || depth > MaxPipelineDepth) {
            throw std::logic_error("DrawToken invalid pipeline depth");
        }
        for (u32 i = 0; i < depth; ++i) {
            entries[i] = std::make_unique<Entry>(owner.gpu_memory);
            entries[i]->job.ctx.job_bindings = owner.job_bindings_enabled;
            entries[i]->job.ctx.check_job_bindings = owner.check_enabled;
        }
        thread = std::jthread([this](std::stop_token stop) {
            if (owner.tail_pipeline_enabled) {
                RunTails(stop);
            } else {
                Run(stop);
            }
        });
    }

    ~PipelineState() {
        ASSERT_MSG(head == tail, "DrawToken pipeline teardown has unconsumed jobs");
        thread.request_stop();
        cv.notify_all();
        thread.join();
    }

    Entry& Front() const { return *entries[head % depth]; }
    bool Empty() const { return head == tail; }
    bool Full() const { return tail - head == depth; }

    void SnapshotRegisters(Entry& target, Tegra::Engines::Maxwell3D& live) {
        // Each slot keeps its last register image. Deltas missed while other
        // slots were used are GPU-owned metadata, not writes to that image.
        const bool reset_chain = source != &live || !live.journal_active ||
                                 live.journal_overflow ||
                                 owner.snapshot_mode == SnapshotMode::FullCopy;
        live.journal_active = true;
        ASSERT(live.reg_journal_consumed <= live.reg_journal_size);
        if (reset_chain) {
            for (u32 i = 0; i < depth; ++i) {
                entries[i]->registers_valid = false;
                entries[i]->pending_journal_size = 0;
            }
            source = &live;
        }
        const size_t count = live.reg_journal_size - live.reg_journal_consumed;
        const auto* delta = live.reg_journal.data() + live.reg_journal_consumed;
        for (u32 i = 0; i < depth; ++i) {
            auto& slot = *entries[i];
            if (&slot == &target || !slot.registers_valid) {
                continue;
            }
            if (count > slot.pending_journal.size() - slot.pending_journal_size) {
                // Bounded storage: stale slot will resync when next reused.
                // Its current queued job and checker reference remain intact.
                slot.registers_valid = false;
                slot.pending_journal_size = 0;
                continue;
            }
            if (count != 0) {
                std::memcpy(slot.pending_journal.data() + slot.pending_journal_size,
                            delta, count * sizeof(*delta));
                slot.pending_journal_size += count;
            }
        }
        if (!target.registers_valid) {
            target.engine->regs = live.regs;
            target.registers_valid = true;
            ++owner.diag_resyncs;
        } else {
            target.engine->ReplayJournal(target.pending_journal.data(),
                                         target.pending_journal_size);
            target.engine->ReplayJournal(delta, count);
            owner.diag_journal_entries += target.pending_journal_size + count;
        }
        target.pending_journal_size = 0;
        live.reg_journal_size = 0;
        live.reg_journal_consumed = 0;
        live.journal_overflow = false;
    }

    bool Snapshot(Tegra::Engines::Maxwell3D& live, GraphicsPipeline* pipeline,
                  bool indexed, u32 instances) {
        if (fatal_error) {
            std::rethrow_exception(fatal_error);
        }
        const auto& src = live.draw_manager.draw_state;
        if (src.draw_mode == Tegra::Engines::Maxwell3D::DrawManager::DrawMode::InlineIndex ||
            !src.inline_index_draw_indexes.empty()) {
            return false;
        }
        ASSERT_MSG(!Full(), "DrawToken pipeline must commit its head before enqueue");
        if (Full()) {
            throw std::logic_error("DrawToken pipeline missing backpressure");
        }
        const auto snapshot_start = Clock::now();
        auto& e = *entries[tail % depth];
        SnapshotRegisters(e, live);
        if (owner.check_enabled) {
            e.expected_regs = live.regs;
        }
        e.engine->state = live.state;
        auto& dst = e.engine->draw_manager.draw_state;
        dst.topology = src.topology;
        dst.draw_mode = src.draw_mode;
        dst.draw_indexed = src.draw_indexed;
        dst.base_index = src.base_index;
        dst.vertex_buffer = src.vertex_buffer;
        dst.index_buffer = src.index_buffer;
        dst.base_instance = src.base_instance;
        dst.instance_count = src.instance_count;
        // GPU alone owns live flags. The next snapshot receives only writes
        // after this boundary; worker invalidations target this private set.
        e.engine->dirty.flags = live.dirty.flags;
        live.dirty.flags.reset();
        e.job.pipeline = pipeline;
        e.job.is_indexed = indexed;
        e.job.instance_count = instances;
        if (owner.tail_pipeline_enabled) {
            e.job.tail_complete = false;
            e.job.tail_ns = {};
            e.prefix_published = 0;
            if (owner.check_enabled) {
                owner.capture_tail_inputs(live, e.job);
            }
        }
        e.job.ctx.Reset(e.engine.get(), &owner.gpu_memory);
        if (owner.job_bindings_enabled) {
            owner.CaptureUniformInputs(e.job.ctx, pipeline->UniformBufferMasks());
        }
        e.job.epoch_valid = false;
        e.epoch.short_circuit = owner.epoch_table.short_circuit;
        e.copies = e.epoch.diag_copies;
        e.skips = e.epoch.diag_skips;
        e.overflows = e.epoch.diag_overflows;
        e.sync_requests = 0;
        e.resolve_ns = {};
        e.error = nullptr;
        e.scheduler = nullptr;
        e.armed = e.spliced = false;
        e.status.store(Status::Queued, std::memory_order_relaxed);
        e.id = ++owner.diag_kicks;
        ++tail;
        owner.diag_pipeline_max_inflight =
            (std::max)(owner.diag_pipeline_max_inflight, tail - head);
        owner.diag_pipeline_snapshot_ns += Clock::now() - snapshot_start;
        ++owner.diag_pipeline_snapshot_count;
        return true;
    }

    void ArmFront() {
        if (Empty() || Front().armed) {
            return;
        }
        auto& e = Front();
        ASSERT(e.scheduler != nullptr);
        std::scoped_lock lock{mutex};
        ASSERT(!request);
        e.armed = true;
        scheduled = &e;
        wake_sequence.fetch_add(1, std::memory_order_release);
        cv.notify_all();
    }

    void Kick(Scheduler& scheduler) {
        ASSERT(!teardown && std::this_thread::get_id() == gpu_thread);
        Poll(); // never overwrite a parked worker's request
        ASSERT(!Empty());
        entries[(tail - 1) % depth]->scheduler = &scheduler;
        if (owner.tail_pipeline_enabled) {
            auto& e = *entries[(tail - 1) % depth];
            e.enqueued_at = Clock::now();
            e.armed = true;
            // J synchronizes the notification with a parked worker. It never
            // nests a cache lock. Slot storage is published by ready_tail.
            std::scoped_lock lock{mutex};
            ready_tail.store(tail, std::memory_order_release);
            cv.notify_all();
            return;
        }
        ArmFront();
    }

    void Request(Scheduler::CapturedBatch& batch, u64 job_id,
                 Scheduler::CaptureSync operation, u64 tick) override {
        std::unique_lock lock{mutex};
        ASSERT(std::this_thread::get_id() == thread.get_id());
        if (owner.tail_pipeline_enabled && cancelled.load(std::memory_order_acquire)) {
            throw std::runtime_error("DrawToken tail FIFO cancelled");
        }
        ASSERT(!request && executing && executing->id == job_id &&
               executing->batch && &*executing->batch == &batch);
        ++executing->sync_requests;
        request = RequestData{executing, operation, tick};
        if (owner.tail_pipeline_enabled) {
            request_at = Clock::now();
        }
        request_pending.store(true, std::memory_order_release);
        cv.notify_all();
        cv.wait(lock, [this] { return !request; }); // releases J, retains B/T
        auto failure = std::exchange(sync_error, nullptr);
        lock.unlock();
        if (failure) {
            std::rethrow_exception(failure);
        }
    }

    void Splice(Entry& e) {
        if (teardown) {
            e.scheduler->AdoptCapturedReceiverForTeardown(*e.batch, e.id, gpu_thread);
        }
        e.scheduler->SpliceCaptured(*e.batch, e.id);
    }

    void Poll() {
        if (owner.tail_pipeline_enabled) {
            PollTails();
            return;
        }
        ASSERT(teardown || std::this_thread::get_id() == gpu_thread);
        if (!request_pending.load(std::memory_order_acquire)) {
            return;
        }
        std::unique_lock lock{mutex};
        if (!request) {
            return;
        }
        const auto service = *request;
        ASSERT(!Empty() && service.entry == &Front());
        lock.unlock();
        std::exception_ptr failure;
        try {
            auto& e = *service.entry;
            Splice(e);
            switch (service.operation) {
            case Scheduler::CaptureSync::Publish:
                e.scheduler->DispatchWork();
                break;
            case Scheduler::CaptureSync::WaitWorker:
                e.scheduler->WaitWorker();
                break;
            case Scheduler::CaptureSync::WaitTick:
                ASSERT(service.tick < e.scheduler->CurrentTick());
                e.scheduler->GetMasterSemaphore().Wait(service.tick);
                break;
            }
        } catch (...) {
            failure = std::current_exception();
        }
        lock.lock();
        sync_error = failure;
        request.reset();
        request_pending.store(false, std::memory_order_release);
        cv.notify_all();
    }

    void Wait() {
        if (owner.tail_pipeline_enabled) {
            WaitTail();
            return;
        }
        if (Empty()) {
            return;
        }
        ASSERT(teardown || std::this_thread::get_id() == gpu_thread);
        Poll();
        ArmFront();
        auto& e = Front();
        if (e.spliced) {
            return;
        }
        auto next_warning = Clock::now() + std::chrono::seconds{5};
        while (e.status.load(std::memory_order_acquire) == Status::Queued) {
            const bool spun = SpinUntil(owner.spin_us, [&] {
                return e.status.load(std::memory_order_acquire) != Status::Queued ||
                       request_pending.load(std::memory_order_acquire);
            });
            if (request_pending.load(std::memory_order_acquire)) {
                Poll(); // service BEFORE considering another completion wait
                continue;
            }
            if (e.status.load(std::memory_order_acquire) != Status::Queued) {
                owner.diag_pipeline_spin_wins += spun;
                break;
            }
            std::unique_lock lock{mutex};
            if (e.status.load(std::memory_order_acquire) == Status::Queued && !request) {
                ++owner.diag_pipeline_parks;
                cv.wait_for(lock, std::chrono::seconds{5}, [&] {
                    return e.status.load(std::memory_order_acquire) != Status::Queued || request;
                });
            }
            lock.unlock();
            Poll();
            if (Clock::now() >= next_warning) {
                LOG_WARNING(Render_Vulkan, "DrawToken pipeline waiting: job={} head={} tail={}",
                            e.id, head, tail);
                next_warning = Clock::now() + std::chrono::seconds{5};
            }
        }
        try {
            if (e.error) {
                std::rethrow_exception(e.error);
            }
            Splice(e);
        } catch (...) {
            // Poison the queue even if final splice partially published. Only
            // the head has executed; younger snapshots can be discarded.
            fatal_error = std::current_exception();
            LOG_ERROR(Render_Vulkan, "DrawToken pipeline failed: job={}; no replay", e.id);
            e.batch.reset();
            head = tail;
            throw;
        }
        e.spliced = true;
        ++owner.diag_capture_batches;
        owner.diag_captured_bytes += e.batch->CapturedBytes();
        owner.diag_splice_count += e.batch->SpliceCount();
        ++owner.diag_resolve_calls;
        owner.diag_resolve_ns += e.resolve_ns;
        ++owner.diag_worker_resolves;
        owner.diag_worker_sync_requests += e.sync_requests;
        ++owner.diag_pipeline_resolves;
        owner.epoch_table.diag_copies += e.epoch.diag_copies - e.copies;
        owner.epoch_table.diag_skips += e.epoch.diag_skips - e.skips;
        owner.epoch_table.diag_overflows += e.epoch.diag_overflows - e.overflows;
    }

    void Retire(Tegra::Engines::Maxwell3D& live) {
        ASSERT(!Empty() && Front().spliced);
        if (owner.tail_pipeline_enabled) {
            Front().batch.reset();
            ++head; // capacity includes every emitted-but-unpublished slot
            return;
        }
        // Preserve unconsumed bits, including new invalidations from resolve
        // and tail. No younger resolve can have started before this tail.
        const auto remaining = Front().engine->dirty.flags;
        Front().batch.reset();
        ++head;
        if (Empty()) {
            live.dirty.flags |= remaining;
        } else {
            ASSERT(!Front().armed);
            Front().engine->dirty.flags |= remaining;
        }
        // Do NOT arm here: gpu.TickWork/FlushWork still follow the tail.
        // Next Kick or Wait is the explicit scheduler/cache ownership handoff.
    }

    void Resolve(Entry& e) {
        e.batch.emplace();
        Scheduler::ResolverThreadScope record_scope{*e.scheduler};
        struct Restore {
            StateTracker& tracker;
            Tegra::Engines::Maxwell3D::DirtyState::Flags* flags;
            Tegra::Engines::Maxwell3D* previous{tls_engine_snapshot};
            Tegra::Engines::Maxwell3D* query_previous{
                VideoCommon::tls_pipeline_engine_snapshot};
            const VideoCommon::UniformEpochSnapshot* epoch_previous{VideoCommon::tls_uniform_epoch};
            ~Restore() {
                tracker.ExchangeFlags(flags);
                tls_engine_snapshot = previous;
                VideoCommon::tls_pipeline_engine_snapshot = query_previous;
                VideoCommon::tls_uniform_epoch = epoch_previous;
            }
        } restore{owner.state_tracker,
                  owner.state_tracker.ExchangeFlags(&e.engine->dirty.flags)};
        tls_engine_snapshot = e.engine.get();
        VideoCommon::tls_pipeline_engine_snapshot = e.engine.get();
        {
            std::scoped_lock lock{owner.buffer_cache.mutex, owner.texture_cache.mutex};
            Scheduler::CaptureScope capture{*e.scheduler, *e.batch, e.id, this, gpu_thread};
            if (owner.tail_pipeline_enabled) {
                e.engine->dirty.flags |= residual_dirty;
                e.engine->dirty.flags |= owner.buffer_cache.TakeGraphicsInvalidations();
                e.engine->dirty.flags |= owner.texture_cache.TakeGraphicsInvalidations();
                e.epoch_hits = owner.buffer_cache.diag_epoch_hits;
                e.epoch_misses = owner.buffer_cache.diag_epoch_misses;
                e.epoch_classic = owner.buffer_cache.diag_epoch_classic;
            }
            const auto start = Clock::now();
            e.job.pipeline->ConfigureResolve(e.job.ctx, e.job.is_indexed);
            e.resolve_ns = Clock::now() - start;
            if (owner.epoch_enabled) {
                e.job.epoch_entry_count = owner.buffer_cache.CaptureUniformEpoch(
                    *e.engine, e.job.pipeline->UniformBufferMasks(),
                    e.job.pipeline->UniformBufferSizeTable(), e.epoch,
                    e.job.epoch_entries.data(), e.job.epoch_entries.size());
                e.job.epoch_snapshot = {e.job.epoch_entries.data(), e.job.epoch_entry_count,
                                        e.epoch.bytes.data()};
                e.job.epoch_valid = true;
            }
            if (owner.tail_pipeline_enabled) {
                VideoCommon::tls_uniform_epoch = e.job.epoch_valid ? &e.job.epoch_snapshot : nullptr;
                owner.worker_tail(*e.engine, e.job);
                residual_dirty = e.engine->dirty.flags;
                e.job.tail_complete = true;
                e.epoch_hits = owner.buffer_cache.diag_epoch_hits - e.epoch_hits;
                e.epoch_misses = owner.buffer_cache.diag_epoch_misses - e.epoch_misses;
                e.epoch_classic = owner.buffer_cache.diag_epoch_classic - e.epoch_classic;
            }
        }
        e.scheduler->ReleaseCaptured(*e.batch, e.id);
    }

    // Stage 4 publication is GPU-only. A requesting job can be younger than
    // Front(): publish its completed predecessors, never wait on a successor.
    void SpliceTail(Entry& e) {
        const u64 prefix = e.batch->PrefixSequence();
        if (e.id != published_tail + 1 || prefix != e.prefix_published + 1) {
            throw std::logic_error("DrawToken tail prefix order violation");
        }
        Splice(e);
        e.prefix_published = prefix;
        last_prefix_job = e.id;
        last_prefix_seq = prefix;
    }

    void ObservePendingChunks(Entry* requested = nullptr) {
        u64 chunks = requested ? requested->batch->PendingChunks() : 0;
        const u64 end = executed_tail.load(std::memory_order_acquire);
        for (u64 seq = published_tail; seq < end; ++seq) {
            chunks += entries[seq % depth]->batch->PendingChunks();
        }
        queue_chunk_high_water = (std::max)(queue_chunk_high_water, chunks);
    }

    void PublishTails(u64 end) {
        ObservePendingChunks();
        ASSERT(end <= executed_tail.load(std::memory_order_acquire));
        while (published_tail < end) {
            auto& e = *entries[published_tail % depth];
            ASSERT(e.job.tail_complete && e.status.load(std::memory_order_acquire) == Status::Complete);
            SpliceTail(e);
            e.spliced = true;
            ++owner.diag_capture_batches;
            owner.diag_captured_bytes += e.batch->CapturedBytes();
            owner.diag_splice_count += e.batch->SpliceCount();
            captured_chunks += e.batch->CapturedChunks();
            chunk_high_water = (std::max)(chunk_high_water, e.batch->ChunkHighWater());
            ++owner.diag_resolve_calls;
            owner.diag_resolve_ns += e.resolve_ns;
            ++owner.diag_worker_resolves;
            owner.diag_worker_sync_requests += e.sync_requests;
            ++owner.diag_pipeline_resolves;
            owner.diag_tail_epoch_hits += e.epoch_hits;
            owner.diag_tail_epoch_misses += e.epoch_misses;
            owner.diag_tail_epoch_classic += e.epoch_classic;
            owner.epoch_table.diag_copies += e.epoch.diag_copies - e.copies;
            owner.epoch_table.diag_skips += e.epoch.diag_skips - e.skips;
            owner.epoch_table.diag_overflows += e.epoch.diag_overflows - e.overflows;
            enqueue_latency += e.enqueue_latency;
            publication_latency += Clock::now() - e.emitted_at;
            ++published_tail;
            published_observed.store(published_tail, std::memory_order_release);
        }
    }

    [[noreturn]] void PoisonTails(std::exception_ptr error) {
        const u64 failed_job = published_tail + 1;
        if (!fatal_error) {
            fatal_error = error;
        }
        cancelled.store(true, std::memory_order_release);
        std::unique_lock lock{mutex};
        cv.notify_all();
        // A younger job may already be emitting. Do not free its slot or wait
        // on B/T: reject any bridge request so it can unwind and stop itself.
        while (!stopped.load(std::memory_order_acquire)) {
            if (request) {
                sync_error = fatal_error;
                request.reset();
                request_pending.store(false, std::memory_order_release);
                cv.notify_all();
            }
            cv.wait(lock, [this] {
                return stopped.load(std::memory_order_acquire) || request.has_value();
            });
        }
        lock.unlock();
        const u64 reclaimed = head;
        for (auto& e : entries) {
            if (e) {
                e->batch.reset();
            }
        }
        discarded_jobs += tail - head;
        head = tail; // discarded suffix; fatal_error prevents all later enqueue
        LOG_ERROR(Render_Vulkan,
                  "DrawToken tail FIFO poisoned: failed_job={} enqueued={} executed={} "
                  "published={} reclaimed={}; partial publication possible, no replay",
                  failed_job, tail, executed_tail.load(), published_tail, reclaimed);
        std::rethrow_exception(fatal_error);
    }

    void CheckTails() {
        if (fatal_error) {
            std::rethrow_exception(fatal_error);
        }
        if (cancelled.load(std::memory_order_acquire)) {
            // Worker error and failed_seq precede the release cancellation.
            auto error = entries[failed_seq % depth]->error;
            try {
                PublishTails(executed_tail.load(std::memory_order_acquire));
            } catch (...) {
                PoisonTails(std::current_exception());
            }
            PoisonTails(error ? error : std::make_exception_ptr(
                std::runtime_error("DrawToken tail FIFO cancelled")));
        }
    }

    void PollTails() {
        ASSERT(teardown || std::this_thread::get_id() == gpu_thread);
        CheckTails();
        try {
            if (request_pending.load(std::memory_order_acquire)) {
                std::unique_lock lock{mutex};
                if (request) {
                    const auto service = *request;
                    const auto began = request_at;
                    lock.unlock();
                    ObservePendingChunks(service.entry);
                    bridge_predecessors += executed_tail.load(std::memory_order_acquire) - published_tail;
                    // Worker parked in job N. All jobs < N have completed,
                    // including ones not yet reclaimed by the GPU.
                    PublishTails(executed_tail.load(std::memory_order_acquire));
                    auto& e = *service.entry;
                    SpliceTail(e);
                    switch (service.operation) {
                    case Scheduler::CaptureSync::Publish:
                        e.scheduler->DispatchWork();
                        break;
                    case Scheduler::CaptureSync::WaitWorker:
                        e.scheduler->WaitWorker();
                        break;
                    case Scheduler::CaptureSync::WaitTick:
                        if (!e.scheduler->IsSubmissionPublished(service.tick)) {
                            throw std::logic_error("DrawToken wait before submit publication");
                        }
                        e.scheduler->GetMasterSemaphore().Wait(service.tick);
                        break;
                    }
                    bridge_latency += Clock::now() - began;
                    ++bridge_services;
                    lock.lock();
                    request.reset();
                    request_pending.store(false, std::memory_order_release);
                    cv.notify_all();
                }
            }
            PublishTails(executed_tail.load(std::memory_order_acquire));
        } catch (...) {
            PoisonTails(std::current_exception());
        }
        CheckTails();
    }

    void WaitTail() {
        if (Empty()) {
            return; // quiescent teardown must not rethrow an already reported poison
        }
        PollTails();
        auto next_warning = Clock::now() + std::chrono::seconds{5};
        while (published_tail <= head) {
            const bool spun = SpinUntil(owner.spin_us, [&] {
                return executed_tail.load(std::memory_order_acquire) > head ||
                       request_pending.load(std::memory_order_acquire) ||
                       cancelled.load(std::memory_order_acquire);
            });
            PollTails();
            if (published_tail > head) {
                owner.diag_pipeline_spin_wins += spun;
                break;
            }
            std::unique_lock lock{mutex};
            if (!request && executed_tail.load(std::memory_order_acquire) <= head &&
                !cancelled.load(std::memory_order_acquire)) {
                ++owner.diag_pipeline_parks;
                cv.wait_for(lock, std::chrono::seconds{5});
            }
            lock.unlock();
            PollTails();
            if (Clock::now() >= next_warning) {
                LOG_WARNING(Render_Vulkan,
                            "DrawToken tail wait: job={} executed={} published={} reclaimed={}",
                            Front().id, executed_tail.load(), published_tail, head);
                next_warning = Clock::now() + std::chrono::seconds{5};
            }
        }
    }

    void RunTails(std::stop_token stop) {
        Common::SetCurrentThreadName("DrawResolver");
        u64 cursor{};
        for (;;) {
            const bool spun = SpinUntil(owner.spin_us, [&] {
                return ready_tail.load(std::memory_order_acquire) > cursor ||
                       cancelled.load(std::memory_order_acquire) || stop.stop_requested();
            });
            std::unique_lock lock{mutex};
            if (ready_tail.load(std::memory_order_acquire) == cursor &&
                !cancelled.load(std::memory_order_acquire) && !stop.stop_requested()) {
                owner.diag_pipeline_worker_parks.fetch_add(1, std::memory_order_relaxed);
                cv.wait(lock, stop, [&] {
                    return ready_tail.load(std::memory_order_acquire) > cursor ||
                           cancelled.load(std::memory_order_acquire);
                });
            } else if (spun) {
                owner.diag_pipeline_worker_spin_wins.fetch_add(1, std::memory_order_relaxed);
            }
            if (stop.stop_requested() || cancelled.load(std::memory_order_acquire)) {
                stopped.store(true, std::memory_order_release);
                cv.notify_all();
                return;
            }
            auto& e = *entries[cursor % depth];
            executing = &e;
            max_executing.store(1, std::memory_order_relaxed);
            lock.unlock();
            try {
                e.enqueue_latency = Clock::now() - e.enqueued_at;
                Resolve(e);
                e.emitted_at = Clock::now();
            } catch (...) {
                e.error = std::current_exception();
            }
            lock.lock();
            executing = nullptr;
            if (e.error) {
                failed_seq = cursor;
                e.status.store(Status::Failed, std::memory_order_release);
                cancelled.store(true, std::memory_order_release);
                stopped.store(true, std::memory_order_release);
                cv.notify_all();
                return; // younger slots are poisoned, never executed
            }
            const u64 unpublished = cursor + 1 - published_observed.load(std::memory_order_acquire);
            max_unpublished.store((std::max)(max_unpublished.load(), unpublished));
            e.status.store(Status::Complete, std::memory_order_release);
            executed_tail.store(++cursor, std::memory_order_release);
            cv.notify_all();
            // No slot access after executed_tail publication: receiver can
            // publish/reclaim while this thread starts the next queued job.
        }
    }

    void Run(std::stop_token stop) {
        Common::SetCurrentThreadName("DrawResolver");
        u64 seen{};
        for (;;) {
            const bool spun = SpinUntil(owner.spin_us, [&] {
                return wake_sequence.load(std::memory_order_acquire) != seen ||
                       stop.stop_requested();
            });
            std::unique_lock lock{mutex};
            if (wake_sequence.load(std::memory_order_acquire) == seen && !stop.stop_requested()) {
                owner.diag_pipeline_worker_parks.fetch_add(1, std::memory_order_relaxed);
                cv.wait(lock, stop, [&] {
                    return wake_sequence.load(std::memory_order_acquire) != seen;
                });
            } else if (spun && !stop.stop_requested()) {
                owner.diag_pipeline_worker_spin_wins.fetch_add(1, std::memory_order_relaxed);
            }
            if (stop.stop_requested()) {
                return;
            }
            seen = wake_sequence.load(std::memory_order_acquire);
            auto* e = scheduled;
            executing = e;
            lock.unlock();
            try {
                Resolve(*e);
            } catch (...) {
                e->error = std::current_exception();
            }
            lock.lock();
            executing = nullptr;
            e->status.store(e->error ? Status::Failed : Status::Complete, std::memory_order_release);
            cv.notify_all();
            // No entry access after completion publication; GPU may retire it.
        }
    }

    DrawResolver& owner;
    const std::thread::id gpu_thread;
    const u32 depth;
    const Tegra::Engines::Maxwell3D* source{}; // GPU-only journal chain identity
    std::array<std::unique_ptr<Entry>, MaxPipelineDepth> entries;
    bool teardown{}; // receiver-only, ordinary GPU producer has stopped
    std::exception_ptr fatal_error; // GPU-only; failed prefixes must never replay
    u64 head{}, tail{}; // GPU-only FIFO, includes completed but unconsumed tails
    std::mutex mutex;
    std::condition_variable_any cv;
    std::optional<RequestData> request;
    std::exception_ptr sync_error;
    std::atomic<bool> request_pending{};
    std::atomic<u64> wake_sequence{};
    Entry* scheduled{}; // J-protected, published with wake_sequence
    Entry* executing{}; // J-protected for Request
    // Stage 4: tail/head are GPU enqueued/reclaimed positions. Publication is
    // independent; worker alone advances executed_tail. All are exclusive ends.
    std::atomic<u64> ready_tail{}, executed_tail{}, published_observed{};
    Scheduler* tail_scheduler{}; // GPU-only handoff target; survives all jobs
    u64 published_tail{};
    std::atomic<bool> cancelled{}, stopped{};
    u64 failed_seq{}; // published by stopped/cancelled after the error is stored
    Tegra::Engines::Maxwell3D::DirtyState::Flags residual_dirty{};
    Clock::time_point request_at{}; // J/bridge rendezvous
    std::atomic<u64> max_unpublished{};
    std::atomic<u64> max_executing{};
    u64 captured_chunks{}, chunk_high_water{}, queue_chunk_high_water{}, discarded_jobs{};
    std::chrono::nanoseconds enqueue_latency{}, publication_latency{}, bridge_latency{};
    u64 bridge_services{}, bridge_predecessors{}, last_prefix_job{}, last_prefix_seq{};
    std::jthread thread;
};

DrawResolver::DrawResolver(Tegra::MemoryManager& gpu_memory_, BufferCache& buffer_cache_,
                           TextureCache& texture_cache_, StateTracker& state_tracker_)
    : gpu_memory{gpu_memory_}, buffer_cache{buffer_cache_}, texture_cache{texture_cache_},
      state_tracker{state_tracker_}, shadow{std::make_unique<Tegra::Engines::Maxwell3D>(gpu_memory_)} {}

void DrawResolver::EnableJobBindings(const Tegra::Engines::Maxwell3D& engine) {
    job_bindings_enabled = true;
    job.ctx.job_bindings = true;
    job.ctx.check_job_bindings = check_enabled;
    // First activation can follow legacy CB callbacks, including a channel rebind.
    std::scoped_lock lock{buffer_cache.mutex};
    const auto bindings = buffer_cache.CopyGraphicsUniformBindings();
    for (u32 stage = 0; stage < VideoCommon::NUM_STAGES; ++stage) {
        for (u32 index = 0; index < VideoCommon::NUM_GRAPHICS_UNIFORM_BUFFERS; ++index) {
            uniform_inputs[stage][index] = {
                bindings[stage][index], 0, stage, index,
                engine.state.shader_stages[stage].const_buffers[index].enabled};
        }
    }
}

void DrawResolver::BindUniformInput(size_t stage, u32 index, GPUVAddr addr, u32 size) {
    auto& input = uniform_inputs[stage][index];
    input.binding = buffer_cache.ResolveGraphicsUniformBufferBinding(addr, size);
    input.enabled = true;
    ++input.revision;
    uniform_pending[stage] |= 1U << index;
}

void DrawResolver::DisableUniformInput(size_t stage, u32 index) {
    auto& input = uniform_inputs[stage][index];
    input.binding = VideoCommon::NULL_BINDING;
    input.enabled = false;
    ++input.revision;
    uniform_pending[stage] |= 1U << index;
}

void DrawResolver::CaptureUniformInputs(DrawContext& ctx, const std::array<u32, 5>& used) {
    ctx.uniform_records.clear();
    for (u32 stage = 0; stage < VideoCommon::NUM_STAGES; ++stage) {
        u32 mask = used[stage] | uniform_pending[stage];
        while (mask != 0) {
            const u32 index = std::countr_zero(mask);
            ctx.uniform_records.push_back(uniform_inputs[stage][index]);
            mask &= mask - 1;
        }
    }
    uniform_pending.fill(0);
    ctx.uniform_applied_versions = &uniform_applied_versions;
    RecordUniformBindingsCapture({ctx.uniform_records.data(), ctx.uniform_records.size()});
}

void DrawResolver::ApplyPendingUniformInputs() {
    ASSERT(!ResolveInFlight());
    if (std::all_of(uniform_pending.begin(), uniform_pending.end(),
                    [](u32 mask) { return mask == 0; })) {
        return;
    }
    // Only pending parser events, never replay all inputs over cached host IDs.
    DrawContext pending;
    CaptureUniformInputs(pending, {});
    ApplyJobUniformBindings(buffer_cache,
                            {pending.uniform_records.data(), pending.uniform_records.size()},
                            uniform_applied_versions, check_enabled);
}

DrawResolver::~DrawResolver() {
    WaitResolved();
    worker.reset();
    pipeline_state.reset();
}

void DrawResolver::WaitResolved() {
    if (pipeline_state) {
        pipeline_state->Wait();
        return;
    }
    if (worker) {
        worker->Wait();
        return;
    }
    while (job_phase.load(std::memory_order_acquire) == Phase::Resolving) {
        std::this_thread::yield();
    }
}

bool DrawResolver::ResolveInFlight() const {
    return pipeline_state ? !pipeline_state->Empty()
                          : job_phase.load(std::memory_order_relaxed) != Phase::Idle;
}

bool DrawResolver::PipelineFull() const {
    return pipeline_state && pipeline_state->Full();
}

void DrawResolver::PollResolveSync() {
    if (pipeline_state) {
        pipeline_state->Poll();
    }
}

bool DrawResolver::TailFrontReady() {
    PollResolveSync();
    return pipeline_state && pipeline_state->published_tail > pipeline_state->head;
}

void DrawResolver::ReturnTailOwnership(Tegra::Engines::Maxwell3D& engine) {
    if (!tail_pipeline_enabled || !tail_ownership) {
        return;
    }
    ASSERT(pipeline_state && pipeline_state->Empty());
    std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
    engine.dirty.flags |= pipeline_state->residual_dirty;
    pipeline_state->residual_dirty.reset();
    engine.dirty.flags |= buffer_cache.TakeGraphicsInvalidations();
    engine.dirty.flags |= texture_cache.TakeGraphicsInvalidations();
    buffer_cache.DeferGraphicsInvalidations(false);
    texture_cache.DeferGraphicsInvalidations(false);
    if (pipeline_state->tail_scheduler) {
        pipeline_state->tail_scheduler->SetTailProducerLoan(false);
        pipeline_state->tail_scheduler = nullptr;
    }
    tail_ownership = false;
}

void DrawResolver::LogTailPipelineDiag() {
    if (!pipeline_state) {
        return;
    }
    const auto& p = *pipeline_state;
    const u64 published = p.published_tail;
    LOG_INFO(Render_Vulkan,
             "DrawToken tail FIFO: enqueued={} executed={} published={} reclaimed={} "
             "max_queued={} max_executing={} max_emitted_unpublished={} capacity_waits={} "
             "enqueue_execution_avg_ns={} emission_publication_avg_ns={} "
             "bridge_services={} bridge_service_avg_ns={} captured_bytes={} captured_chunks={} "
             "bytes_per_draw={} chunks_per_draw={} batch_chunk_high_water={} "
             "unpublished_chunk_high_water={} discarded={} enqueue_execution_ns={} "
             "emission_publication_ns={} bridge_service_ns={} bridge_predecessors={} "
             "last_prefix_job={} last_prefix_seq={}",
             p.tail, p.executed_tail.load(std::memory_order_acquire), published, p.head - p.discarded_jobs,
             diag_pipeline_max_inflight, p.max_executing.load(), p.max_unpublished.load(),
             diag_pipeline_backpressure, published ? p.enqueue_latency.count() / published : 0,
             published ? p.publication_latency.count() / published : 0,
             p.bridge_services, p.bridge_services ? p.bridge_latency.count() / p.bridge_services : 0,
             diag_captured_bytes, p.captured_chunks,
             published ? diag_captured_bytes / published : 0,
             published ? static_cast<double>(p.captured_chunks) / published : 0.0,
             p.chunk_high_water, p.queue_chunk_high_water, p.discarded_jobs,
             p.enqueue_latency.count(), p.publication_latency.count(), p.bridge_latency.count(),
             p.bridge_predecessors, p.last_prefix_job, p.last_prefix_seq);
}

void DrawResolver::AbortTailPipeline(std::exception_ptr error) noexcept {
    if (tail_pipeline_enabled && pipeline_state) {
        try {
            pipeline_state->PoisonTails(error);
        } catch (...) {
            // Teardown only: poison already stopped the producer and discarded
            // all unpublished batches. The original failure was reported.
        }
    }
}

void DrawResolver::PreparePipelineEnqueue() {
    PollResolveSync();
    if (pipeline_state && !pipeline_state->Empty() && pipeline_state->Front().armed) {
        pipeline_state->Wait();
    }
}

void DrawResolver::BeginPipelineTeardown() {
    if (pipeline_state) {
        pipeline_state->teardown = true;
    }
}

DrawResolver::Job& DrawResolver::TakeJob() {
    ASSERT(!pipeline_state || (!pipeline_state->Empty() && pipeline_state->Front().spliced));
    return pipeline_state ? pipeline_state->Front().job : job;
}

Tegra::Engines::Maxwell3D& DrawResolver::SnapshotEngine() const {
    return pipeline_state ? *pipeline_state->Front().engine : *shadow;
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
    if (tail_worker_enabled && !tail_pipeline_enabled) {
        if (tail_worker_error) {
            std::rethrow_exception(tail_worker_error);
        }
        ASSERT(worker_enabled && batch_enabled && job_bindings_enabled && !pipeline_enabled);
        job.tail_complete = false;
        job.tail_ns = {};
    }
    if (pipeline_enabled) {
        if (!pipeline_state) {
            pipeline_state = std::make_unique<PipelineState>(*this);
        }
        if (tail_pipeline_enabled && !tail_ownership) {
            // Route foreign invalidations BEFORE copying live flags. Enabling
            // at Kick would lose an invalidation between snapshot and dispatch.
            std::scoped_lock lock{buffer_cache.mutex, texture_cache.mutex};
            buffer_cache.DeferGraphicsInvalidations(true);
            texture_cache.DeferGraphicsInvalidations(true);
            tail_ownership = true;
        }
        return pipeline_state->Snapshot(engine, pipeline, is_indexed, instance_count);
    }
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
    if (job_bindings_enabled) {
        CaptureUniformInputs(job.ctx, pipeline->UniformBufferMasks());
    }
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
    if (pipeline_enabled) {
        if (tail_pipeline_enabled && !pipeline_state->tail_scheduler) {
            ASSERT(tail_ownership);
            pipeline_state->tail_scheduler = &scheduler;
            scheduler.SetTailProducerLoan(true);
        }
        pipeline_state->Kick(scheduler);
        return; // 2A waits at consumption/barriers, never at this enqueue point.
    }
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
            if (tail_worker_enabled) {
                tail_worker_error = std::current_exception();
            }
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
        // Stage 3 lends all scheduler/cache emission state to this producer.
        // GPU Wait only services the queue bridge: no parser, B/T, retirement,
        // pending uniform application or StateTracker access until completion.
        struct RestoreTailState {
            StateTracker* tracker{};
            Tegra::Engines::Maxwell3D::DirtyState::Flags* flags{};
            Tegra::Engines::Maxwell3D* query{VideoCommon::tls_pipeline_engine_snapshot};
            const VideoCommon::UniformEpochSnapshot* epoch{VideoCommon::tls_uniform_epoch};
            ~RestoreTailState() {
                if (tracker) {
                    tracker->ExchangeFlags(flags);
                    VideoCommon::tls_pipeline_engine_snapshot = query;
                    VideoCommon::tls_uniform_epoch = epoch;
                }
            }
        };
        std::optional<RestoreTailState> restore_tail;
        if (tail_worker_enabled) {
            ASSERT(worker_state && worker_tail && !pipeline_enabled);
            restore_tail.emplace();
            restore_tail->tracker = &state_tracker;
            restore_tail->flags = state_tracker.ExchangeFlags(&shadow->dirty.flags);
            VideoCommon::tls_pipeline_engine_snapshot = shadow.get();
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
        if (tail_worker_enabled) {
            VideoCommon::tls_uniform_epoch = job.epoch_valid ? &job.epoch_snapshot : nullptr;
            worker_tail(*shadow, job);
            job.tail_complete = true;
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
    if (pipeline_state) {
        pipeline_state->Retire(engine);
        return;
    }
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
    if (pipeline_state) {
        // Oldest queued job is NOT the latest live engine. Compare with its
        // own immutable LIVE enqueue reference, never a replay-derived copy.
        // Unlike checking shadow + later writes against the latest live regs,
        // this also catches missed journal writes later overwritten by parser.
        const auto& e = pipeline_state->Front();
        for (size_t i = 0; i < e.expected_regs.reg_array.size(); ++i) {
            if (e.engine->regs.reg_array[i] != e.expected_regs.reg_array[i]) {
                ++diag_snapshot_mismatches;
                LOG_ERROR(Render_Vulkan, "DrawToken pipeline mismatch: job={} reg={:#x}",
                          e.id, i * sizeof(u32));
                return false;
            }
        }
        return true;
    }
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
