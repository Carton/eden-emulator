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
    REQUIRE(Stats::ir_hits.load() == hits + (Stats::Enabled() ? 1 : 0));

    config.check_halt_on_memory_access = true;
    A64::Jit checked{config};
    StepAdd(checked, env);
    REQUIRE(checked.GetRegister(0) == 5);
    REQUIRE(Stats::ir_hits.load() == hits + (Stats::Enabled() ? 1 : 0));
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

TEST_CASE("A64 IR reuse preserves branches, memory and vector results", "[ir-cache-execution]") {
    A64TestEnv env;
    env.code_mem = {
        0x8b010000, // ADD X0, X0, X1
        0xf1000442, // SUBS X2, X2, #1
        0x54ffffc1, // B.NE 0
        0xf9000060, // STR X0, [X3]
        0xf9400064, // LDR X4, [X3]
        0x1e622820, // FADD D0, D1, D2
        0x4ea58483, // ADD V3.4S, V4.4S, V5.4S
        0x4e2008e6, // REV64 V6.16B, V7.16B
        0x14000000, // B .
    };
    const auto config = ConfigFor(env);
    const auto run = [&](A64::Jit& jit) {
        env.ticks_left = 15;
        env.modified_memory.clear();
        jit.SetPC(0);
        jit.SetRegister(0, 0);
        jit.SetRegister(1, 7);
        jit.SetRegister(2, 3);
        jit.SetRegister(3, 0x1000);
        jit.SetVector(1, {0x3ff8000000000000, 0}); // 1.5
        jit.SetVector(2, {0x4002000000000000, 0}); // 2.25
        jit.SetVector(4, {0x0000000200000001, 0x0000000400000003});
        jit.SetVector(5, {0x000000140000000a, 0x000000280000001e});
        jit.SetVector(7, {0x0123456789abcdef, 0x1122334455667788});
        jit.Run();
        REQUIRE(jit.GetPC() == 32);
        REQUIRE(jit.GetRegister(0) == 21);
        REQUIRE(jit.GetRegister(2) == 0);
        REQUIRE(jit.GetRegister(4) == 21);
        REQUIRE(env.MemoryRead64(0x1000) == 21);
        REQUIRE(jit.GetVector(0) == A64::Vector{0x400e000000000000, 0}); // 3.75
        REQUIRE(jit.GetVector(3) == A64::Vector{0x000000160000000b, 0x0000002c00000021});
        REQUIRE(jit.GetVector(6) == A64::Vector{0xefcdab8967452301, 0x8877665544332211});
    };
    A64::Jit first{config};
    run(first);
    const auto hits = Stats::ir_hits.load();
    A64::Jit peer{config};
    run(peer);
    REQUIRE(peer.GetRegisters() == first.GetRegisters());
    REQUIRE(peer.GetVectors() == first.GetVectors());
    REQUIRE(peer.GetPstate() == first.GetPstate());
    if (!Stats::Enabled()) {
        REQUIRE(Stats::block_lookups.load() == 0);
        REQUIRE(Stats::block_compiles.load() == 0);
        REQUIRE(Stats::compile_ns.load() == 0);
        REQUIRE(Stats::translate_ns.load() == 0);
        REQUIRE(Stats::optimize_ns.load() == 0);
        REQUIRE(Stats::emit_ns.load() == 0);
        REQUIRE(Stats::ir_hits.load() == 0);
        REQUIRE(Stats::ir_stores.load() == 0);
    }
    if (Backend::X64::IRCache::Enabled() && Stats::Enabled()) {
        REQUIRE(Stats::ir_hits.load() > hits);
    }
}

TEST_CASE("A64 IR reuse preserves AES results with host-dependent polyfills", "[ir-cache-execution]") {
    A64TestEnv env;
    env.code_mem = {
        0x4e284820, // AESE V0.16B, V1.16B
        0x4e286802, // AESMC V2.16B, V0.16B
        0x14000000, // B .
    };
    const auto config = ConfigFor(env);
    const auto run = [&](A64::Jit& jit) {
        env.ticks_left = 3;
        jit.SetPC(0);
        jit.SetVector(0, {0, 0});
        jit.SetVector(1, {0, 0});
        jit.Run();
        REQUIRE(jit.GetPC() == 8);
        REQUIRE(jit.GetVector(0) == A64::Vector{0x6363636363636363, 0x6363636363636363});
        REQUIRE(jit.GetVector(2) == jit.GetVector(0));
    };
    A64::Jit first{config};
    run(first);
    const auto hits = Stats::ir_hits.load();
    A64::Jit peer{config};
    run(peer);
    if (Backend::X64::IRCache::Enabled() && Stats::Enabled()) {
        REQUIRE(Stats::ir_hits.load() > hits);
    }
}
