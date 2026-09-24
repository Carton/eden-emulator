// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <chrono>

#include "common/common_types.h"
#include "common/logging.h"

namespace Vulkan::TailPipelineDiag {

using Clock = std::chrono::steady_clock;

inline u64 Ns(Clock::duration duration) {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(duration).count());
}

inline constexpr std::array DrainNames{"other", "flush_caching", "guest_write", "map",
    "unmap", "cold_pipeline", "submit", "indirect", "fallback", "channel", "teardown",
    "invalidation", "sync_request", "download", "presentation", "capture"};

inline void Metric(const char* scope, const void* resolver, const char* segment,
                   u64 calls, u64 ns, u64 draws) {
    LOG_INFO(Render_Vulkan,
             "DrawToken stage5 diag: scope={} resolver={} segment={} calls={} total_ns={} "
             "total_us={} us_per_call={} us_per_draw={}",
             scope, resolver, segment, calls, ns, static_cast<double>(ns) / 1000.0,
             calls ? static_cast<double>(ns) / (1000.0 * calls) : 0.0,
             draws ? static_cast<double>(ns) / (1000.0 * draws) : 0.0);
}

// Only the GPU thread mutates these accumulators. The TLS destructor supplies
// the exact final ledger even when rasterizer destruction runs on another thread.
struct Gpu {
    u64 draws{}, admitted{}, poll_calls{}, poll_blocks{}, poll_ns{}, parse_ns{}, parse_drain_ns{};
    u64 capacity_calls{}, capacity_ns{}, capacity_max_ns{}, capacity_ge128{};
    std::array<u64, 8> capacity_hist{};
    std::array<u64, DrainNames.size()> drain_calls{}, drain_ns{};
    u64 all_drain_ns{};

    void Capacity(u64 ns) {
        ++capacity_calls;
        capacity_ns += ns;
        if (ns > capacity_max_ns) {
            capacity_max_ns = ns;
        }
        size_t bucket{};
        while (bucket < 7 && ns >= (1000ULL << bucket)) {
            ++bucket;
        }
        ++capacity_hist[bucket]; // final bucket is [64 us, infinity)
        capacity_ge128 += ns >= 128000; // distinguish overflow without a ninth bucket
    }

    void Drain(size_t reason, u64 ns) {
        ++drain_calls[reason];
        drain_ns[reason] += ns;
        all_drain_ns += ns;
    }

    void Log(const char* scope) const {
        if (!draws && !all_drain_ns) {
            return;
        }
        LOG_INFO(Render_Vulkan,
                 "DrawToken stage5 diag: scope={} thread=gpu draws={} admitted={} poll_calls={}",
                 scope, draws, admitted, poll_calls);
        Metric(scope, nullptr, "gpu_poll_publish_reclaim", poll_blocks, poll_ns, draws);
        Metric(scope, nullptr, "gpu_parse_snapshot_enqueue_inclusive", draws, parse_ns, draws);
        Metric(scope, nullptr, "gpu_parse_snapshot_enqueue_exclusive", draws,
               parse_ns - parse_drain_ns, draws);
        Metric(scope, nullptr, "gpu_capacity_wait_service", capacity_calls, capacity_ns, draws);
        Metric(scope, nullptr, "gpu_draw_accounted", draws, poll_ns + capacity_ns + parse_ns, draws);
        LOG_INFO(Render_Vulkan,
                 "DrawToken stage5 diag: scope={} capacity_waits={} capacity_total_ns={} "
                 "capacity_max_ns={} capacity_max_us={} hist_lt1={} hist_1_2={} hist_2_4={} "
                 "hist_4_8={} hist_8_16={} hist_16_32={} hist_32_64={} hist_64_plus={} ge128={}",
                 scope, capacity_calls, capacity_ns, capacity_max_ns,
                 static_cast<double>(capacity_max_ns) / 1000.0,
                 capacity_hist[0], capacity_hist[1], capacity_hist[2], capacity_hist[3],
                 capacity_hist[4], capacity_hist[5], capacity_hist[6], capacity_hist[7], capacity_ge128);
        for (size_t i = 0; i < DrainNames.size(); ++i) {
            LOG_INFO(Render_Vulkan,
                     "DrawToken stage5 diag: scope={} drain_reason={} timed_nonempty={} "
                     "wait_service_ns={} wait_service_us={} us_per_draw={}",
                     scope, DrainNames[i], drain_calls[i], drain_ns[i],
                     static_cast<double>(drain_ns[i]) / 1000.0,
                     draws ? static_cast<double>(drain_ns[i]) / (1000.0 * draws) : 0.0);
        }
    }

    ~Gpu() { Log("gpu_thread_final"); }
};

struct Worker {
    u64 jobs{}, resolve_ns{}, resolve_body_ns{}, tail_ns{}, publish_ns{}, idle_ns{};
    u64 tail_configure_ns{}, tail_emit_ns{};

    void Log(const char* scope, const void* resolver) const {
        Metric(scope, resolver, "worker_resolve_epoch_lock", jobs, resolve_ns, jobs);
        Metric(scope, resolver, "worker_resolve_body", jobs, resolve_body_ns, jobs);
        Metric(scope, resolver, "worker_tail", jobs, tail_ns, jobs);
        Metric(scope, resolver, "worker_tail_configure_bind", jobs, tail_configure_ns, jobs);
        Metric(scope, resolver, "worker_tail_dynamic_query_draw_capture", jobs, tail_emit_ns, jobs);
        Metric(scope, resolver, "worker_publish_handoff", jobs, publish_ns, jobs);
        Metric(scope, resolver, "worker_idle_spin_park", jobs, idle_ns, jobs);
        Metric(scope, resolver, "worker_active", jobs, resolve_ns + tail_ns + publish_ns, jobs);
        Metric(scope, resolver, "worker_cycle_including_idle", jobs,
               resolve_ns + tail_ns + publish_ns + idle_ns, jobs);
    }
};

struct WorkerJob {
    Clock::time_point tail_begin{}, emit_begin{}, tail_end{};
    bool emitted{};
};

inline thread_local Gpu gpu;
inline thread_local Worker worker;
inline thread_local WorkerJob worker_job;

} // namespace Vulkan::TailPipelineDiag
