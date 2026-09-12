// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/x64/ir_cache.h"
#include "dynarmic/backend/x64/jit_stats.h"
#include "dynarmic/ir/opcodes.h"

using namespace Dynarmic;
using Backend::X64::IRCache;
namespace Stats = Backend::X64::JitStats;

namespace {
struct CacheLifetime {
    CacheLifetime() { IRCache::RegisterJit(); }
    ~CacheLifetime() { IRCache::UnregisterJit(); }
};

struct CacheBudget {
    std::optional<std::string> previous;
    explicit CacheBudget(const char* value) {
        if (const char* env = std::getenv("EDEN_JIT_IRCACHE_MAXBYTES")) {
            previous = env;
        }
        Set(value);
    }
    ~CacheBudget() { Set(previous ? previous->c_str() : nullptr); }
    static void Set(const char* value) {
#ifdef _WIN32
        _putenv_s("EDEN_JIT_IRCACHE_MAXBYTES", value ? value : "");
#else
        if (value) {
            setenv("EDEN_JIT_IRCACHE_MAXBYTES", value, 1);
        } else {
            unsetenv("EDEN_JIT_IRCACHE_MAXBYTES");
        }
#endif
    }
};
}

TEST_CASE("IR cache preserves pooled instructions, names and references", "[ir-cache]") {
    CacheLifetime lifetime;
    IR::Block original{IR::LocationDescriptor{0}};
    IR::Inst* previous = nullptr;
    // Cross both the inline storage and multiple pooled storage boundaries.
    for (unsigned i = 0; i < 96; ++i) {
        const auto input = previous ? IR::Value{previous} : IR::Value{u64{1}};
        auto inst = original.AppendNewInst(IR::Opcode::Add64,
                                          {input, IR::Value{u64{2}}, IR::Value{false}});
        inst->SetName(65536 + i);
        previous = &*inst;
    }
    auto carry = original.AppendNewInst(IR::Opcode::GetCarryFromOp, {IR::Value{previous}});
    carry->SetName(70000);
    original.SetEndLocation(IR::LocationDescriptor{4});
    original.SetTerminal(IR::Term::CheckBit{IR::Term::LinkBlock{IR::LocationDescriptor{8}},
                                          IR::Term::ReturnToDispatch{}});
    original.CycleCount() = 7;
    IRCache::Store(0, original, 0, 4, 0, {}, {0});
    auto entry = IRCache::Lookup(0);
    REQUIRE(entry);
    IR::Block restored{IR::LocationDescriptor{0}};
    REQUIRE(IRCache::Load(*entry, restored));
    REQUIRE(restored.CycleCount() == 7);
    REQUIRE(restored.GetTerminal().which() == original.GetTerminal().which());
    auto source = original.Instructions().begin();
    IR::Inst* last = nullptr;
    for (auto& inst : restored.Instructions()) {
        REQUIRE(inst.GetName() == source->GetName());
        REQUIRE(inst.GetOpcode() == source->GetOpcode());
        REQUIRE(inst.UseCount() == source->UseCount());
        if (last) {
            REQUIRE(inst.GetArg(0).GetInst() == last);
        }
        last = &inst;
        ++source;
    }
    REQUIRE(source == original.Instructions().end());
}

TEST_CASE("IR cache replaces changed code and configuration", "[ir-cache]") {
    CacheLifetime lifetime;
    IR::Block block{IR::LocationDescriptor{0}};
    block.SetTerminal(IR::Term::ReturnToDispatch{});
    IRCache::Config config{};
    IRCache::Store(0, block, 0, 4, 1, config, {1});
    auto old = IRCache::Lookup(0);
    REQUIRE(old);
    IRCache::Store(0, block, 0, 4, 2, config, {2});
    REQUIRE(IRCache::Lookup(0)->code == std::vector<u32>{2});
    config.wall_clock_cntpct = true;
    IRCache::Store(0, block, 0, 4, 2, config, {2});
    REQUIRE(IRCache::Lookup(0)->config == config);
    REQUIRE(old->code == std::vector<u32>{1});
    REQUIRE(Stats::ir_cache_entries.load() == 1);
}

TEST_CASE("IR cache survives peers and clears after the last JIT", "[ir-cache]") {
    {
        CacheLifetime first;
        IR::Block block{IR::LocationDescriptor{0}};
        block.SetTerminal(IR::Term::ReturnToDispatch{});
        {
            CacheLifetime second;
            IRCache::Store(0, block, 0, 4, 0, {}, {0});
        }
        REQUIRE(IRCache::Lookup(0));
    }
    REQUIRE_FALSE(IRCache::Lookup(0));
    REQUIRE(Stats::ir_cache_bytes.load() == 0);
    REQUIRE(Stats::ir_cache_entries.load() == 0);
}

TEST_CASE("IR cache permits concurrent publication and lookup", "[ir-cache]") {
    CacheLifetime lifetime;
    std::vector<std::thread> threads;
    for (unsigned thread = 0; thread < 4; ++thread) {
        threads.emplace_back([thread] {
            IR::Block block{IR::LocationDescriptor{0}};
            block.SetTerminal(IR::Term::ReturnToDispatch{});
            for (u64 i = 0; i < 128; ++i) {
                IRCache::Store(i, block, 0, 4, thread, {}, {thread});
                (void)IRCache::Lookup(i);
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE(Stats::ir_cache_entries.load() == 128);
    REQUIRE(Stats::ir_cache_bytes.load() > 0);
}

TEST_CASE("IR cache enforces the payload budget under contention", "[ir-cache]") {
    CacheBudget budget{"1024"};
    CacheLifetime lifetime;
    std::vector<std::thread> threads;
    for (u64 thread = 0; thread < 4; ++thread) {
        threads.emplace_back([thread] {
            IR::Block block{IR::LocationDescriptor{0}};
            block.SetTerminal(IR::Term::ReturnToDispatch{});
            for (u64 i = 0; i < 32; ++i) {
                IRCache::Store(thread * 32 + i, block, 0, 4, 0, {}, {0});
            }
        });
    }
    for (auto& thread : threads) {
        thread.join();
    }
    REQUIRE(Stats::ir_cache_bytes.load() <= 1024);
    REQUIRE(Stats::ir_cache_entries.load() > 0);
    REQUIRE(Stats::ir_cache_entries.load() < 128);
}

TEST_CASE("IR cache rejects oversized entries without counting a store", "[ir-cache]") {
    CacheBudget budget{"1"};
    CacheLifetime lifetime;
    IR::Block block{IR::LocationDescriptor{0}};
    block.SetTerminal(IR::Term::ReturnToDispatch{});
    const auto stores = Stats::ir_stores.load();
    IRCache::Store(0, block, 0, 4, 0, {}, {0});
    REQUIRE_FALSE(IRCache::Lookup(0));
    REQUIRE(Stats::ir_stores.load() == stores);
    REQUIRE(Stats::ir_cache_bytes.load() == 0);
}
