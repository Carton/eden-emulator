// SPDX-FileCopyrightText: 2016 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <catch2/catch_test_macros.hpp>

#include <array>
#include <bitset>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "common/thread.h"
#include "core/core.h"
#include "core/core_timing.h"

namespace {
// Numbers are chosen randomly to make sure the correct one is given.
constexpr std::array<u64, 5> calls_order{{2, 0, 1, 4, 3}};
struct ScopeInit final {
    ScopeInit() {
        core_timing.SetMulticore(true);
        core_timing.Initialize([]() {});
    }

    std::vector<std::shared_ptr<Core::Timing::EventType>> CreateEvents() {
        std::vector<std::shared_ptr<Core::Timing::EventType>> events;
        for (std::size_t i = 0; i < delays.size(); ++i) {
            events.push_back(Core::Timing::CreateEvent(
                "callback" + std::to_string(i),
                [this, i](s64, std::chrono::nanoseconds ns_late)
                    -> std::optional<std::chrono::nanoseconds> {
                    callbacks_ran_flags.set(i);
                    delays[i] = ns_late.count();
                    if (callbacks_ran_flags.all()) {
                        callbacks_complete.Set();
                    }
                    return std::nullopt;
                }));
        }
        return events;
    }

    std::array<s64, 5> delays{};
    std::bitset<5> callbacks_ran_flags;
    Common::Event callbacks_complete;
    // Destroy the timer first so callbacks cannot outlive the state above.
    Core::Timing::CoreTiming core_timing;
};

u64 TestTimerSpeed(Core::Timing::CoreTiming& core_timing) {
    const u64 start = core_timing.GetGlobalTimeNs().count();
    volatile u64 placebo = 0;
    for (std::size_t i = 0; i < 1000; i++) {
        placebo = placebo + core_timing.GetGlobalTimeNs().count();
    }
    const u64 end = core_timing.GetGlobalTimeNs().count();
    return end - start;
}

} // Anonymous namespace

TEST_CASE("CoreTiming[BasicOrder]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;
    auto events = guard.CreateEvents();

    core_timing.SyncPause(true);

    const u64 one_micro = 1000U;
    for (std::size_t i = 0; i < events.size(); i++) {
        const u64 order = calls_order[i];
        const auto future_ns = std::chrono::nanoseconds{static_cast<s64>(i * one_micro + 100)};

        core_timing.ScheduleEvent(future_ns, events[order]);
    }
    /// test pause
    REQUIRE(guard.callbacks_ran_flags.none());

    core_timing.Pause(false); // No need to sync

    REQUIRE(guard.callbacks_complete.WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    core_timing.SyncPause(true);
    REQUIRE(guard.callbacks_ran_flags.all());

    for (std::size_t i = 0; i < guard.delays.size(); i++) {
        const double delay = static_cast<double>(guard.delays[i]);
        const double micro = delay / 1000.0f;
        const double mili = micro / 1000.0f;
        printf("HostTimer Pausing Delay[%zu]: %.3f %.6f\n", i, micro, mili);
    }
}

TEST_CASE("CoreTiming[BasicOrderNoPausing]", "[core]") {
    ScopeInit guard;
    auto& core_timing = guard.core_timing;
    auto events = guard.CreateEvents();

    core_timing.SyncPause(true);
    core_timing.SyncPause(false);

    const u64 start = core_timing.GetGlobalTimeNs().count();
    const u64 one_micro = 1000U;

    for (std::size_t i = 0; i < events.size(); i++) {
        const u64 order = calls_order[i];
        const auto future_ns = std::chrono::nanoseconds{static_cast<s64>(i * one_micro + 100)};
        core_timing.ScheduleEvent(future_ns, events[order]);
    }

    const u64 end = core_timing.GetGlobalTimeNs().count();
    const double scheduling_time = static_cast<double>(end - start);
    const double timer_time = static_cast<double>(TestTimerSpeed(core_timing));

    REQUIRE(guard.callbacks_complete.WaitUntil(std::chrono::steady_clock::now() + std::chrono::seconds(10)));
    core_timing.SyncPause(true);
    REQUIRE(guard.callbacks_ran_flags.all());

    for (std::size_t i = 0; i < guard.delays.size(); i++) {
        const double delay = static_cast<double>(guard.delays[i]);
        const double micro = delay / 1000.0f;
        const double mili = micro / 1000.0f;
        printf("HostTimer No Pausing Delay[%zu]: %.3f %.6f\n", i, micro, mili);
    }

    const double micro = scheduling_time / 1000.0f;
    const double mili = micro / 1000.0f;
    printf("HostTimer No Pausing Scheduling Time: %.3f %.6f\n", micro, mili);
    printf("HostTimer No Pausing Timer Time: %.3f %.6f\n", timer_time / 1000.f,
           timer_time / 1000000.f);
}
