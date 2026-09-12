// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/x64/ir_cache.h"
#include "dynarmic/backend/x64/jit_stats.h"
#include "dynarmic/tests/A64/testenv.h"

using namespace Dynarmic;
namespace Stats = Backend::X64::JitStats;

namespace {
A64::UserConfig ConfigFor(A64TestEnv& env) {
    A64::UserConfig config;
    config.callbacks = &env;
    config.code_cache_size = 32 * 1024 * 1024;
    return config;
}

void StepAdd(A64::Jit& jit, A64TestEnv& env) {
    env.ticks_left = 1;
    jit.SetPC(0);
    jit.SetRegister(1, 2);
    jit.SetRegister(2, 3);
    jit.Step();
}
}

TEST_CASE("A64 IR reuse checks translation configuration", "[ir-cache-execution]") {
    if (!Backend::X64::IRCache::Enabled()) {
        SKIP("Shared IR cache is disabled by the environment");
    }
    A64TestEnv env;
    env.code_mem = {0x8b020020}; // ADD X0, X1, X2
    auto config = ConfigFor(env);
    A64::Jit first{config};
    StepAdd(first, env);
    REQUIRE(first.GetRegister(0) == 5);

    const auto hits = Stats::ir_hits.load();
    A64::Jit peer{config};
    StepAdd(peer, env);
    REQUIRE(peer.GetRegister(0) == 5);
    REQUIRE(Stats::ir_hits.load() == hits + 1);

    config.check_halt_on_memory_access = true;
    A64::Jit checked{config};
    StepAdd(checked, env);
    REQUIRE(checked.GetRegister(0) == 5);
    REQUIRE(Stats::ir_hits.load() == hits + 1);
}

TEST_CASE("A64 IR reuse observes changed instructions after invalidation", "[ir-cache-execution]") {
    A64TestEnv env;
    env.code_mem = {0x8b020020}; // ADD X0, X1, X2
    const auto config = ConfigFor(env);
    A64::Jit jit{config};
    StepAdd(jit, env);
    REQUIRE(jit.GetRegister(0) == 5);

    env.code_mem[0] = 0xcb020020; // SUB X0, X1, X2
    jit.InvalidateCacheRange(0, 4);
    StepAdd(jit, env);
    REQUIRE(jit.GetRegister(0) == u64(-1));
    A64::Jit peer{config};
    StepAdd(peer, env);
    REQUIRE(peer.GetRegister(0) == u64(-1));
}

TEST_CASE("A64 IR reuse distinguishes unreadable code from zero", "[ir-cache-execution]") {
    struct FaultEnv final : A64TestEnv {
        bool readable = true;
        std::optional<A64::Exception> exception;
        std::optional<u32> MemoryReadCode(u64) override {
            return readable ? std::optional<u32>{0} : std::nullopt;
        }
        void ExceptionRaised(u64, A64::Exception value) override { exception = value; }
    } env;
    A64::Jit jit{ConfigFor(env)};
    env.ticks_left = 1;
    jit.SetPC(0);
    jit.Step();
    REQUIRE(env.exception == A64::Exception::UnallocatedEncoding);

    env.readable = false;
    env.exception.reset();
    jit.InvalidateCacheRange(0, 4);
    jit.SetPC(0);
    env.ticks_left = 1;
    jit.Step();
    REQUIRE(env.exception == A64::Exception::NoExecuteFault);
}
