// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <memory>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "dynarmic/backend/x64/a64_jitstate.h"
#include "dynarmic/backend/x64/block_of_code.h"
#include "dynarmic/backend/x64/reg_alloc.h"
#include "dynarmic/ir/basic_block.h"
#include "dynarmic/ir/opcodes.h"

using namespace Dynarmic;
using namespace Dynarmic::Backend::X64;

TEST_CASE("Register reverse lookup handles boundary names and spills", "[reg-alloc]") {
    RunCodeCallbacks callbacks{
        std::make_unique<SimpleCallback>(+[]() -> const void* { return nullptr; }),
        nullptr, nullptr, false,
    };
    A64JitState state{};
    BlockOfCode code{std::move(callbacks), JitStateInfo{state}, 32 * 1024 * 1024,
                     [](BlockOfCode&) {}};
    RegAlloc alloc{BuildRegSet({HostLoc::RAX, HostLoc::RCX}), {}};
    IR::Block block{IR::LocationDescriptor{0}};
    std::vector<IR::Inst*> values;
    std::vector<IR::Inst*> users;
    constexpr std::array names{0u, 1u, 4095u, 4096u, 65536u};
    for (unsigned i = 0; i < 40; ++i) {
        auto value = block.AppendNewInst(IR::Opcode::Add64,
                                        {IR::Value{u64{1}}, IR::Value{u64{2}}, IR::Value{false}});
        value->SetName(i < names.size() ? names[i] : i);
        values.push_back(&*value);
        users.push_back(&*block.AppendNewInst(IR::Opcode::Identity, {IR::Value{&*value}}));
    }
    // Keeping every value live while reusing one GPR forces 39 spill locations.
    for (auto* value : values) {
        auto reg = alloc.ScratchGpr(code, HostLoc::RAX);
        alloc.DefineValue(code, value, reg);
        alloc.EndOfAllocScope();
    }
    for (size_t i = 0; i < values.size(); ++i) {
        REQUIRE(alloc.IsValueLive(values[i]));
        auto args = alloc.GetArgumentInfo(users[i]);
        if (i + 1 < values.size()) {
            REQUIRE(args[0].IsInMemory(alloc));
        }
        alloc.Use(code, args[0], HostLoc::RCX);
        REQUIRE(args[0].IsInGpr(alloc));
        alloc.EndOfAllocScope();
    }
    alloc.AssertNoMoreUses();
}

TEST_CASE("Register reverse lookup retracks aliases during exchange", "[reg-alloc]") {
    RunCodeCallbacks callbacks{
        std::make_unique<SimpleCallback>(+[]() -> const void* { return nullptr; }),
        nullptr, nullptr, false,
    };
    A64JitState state{};
    BlockOfCode code{std::move(callbacks), JitStateInfo{state}, 32 * 1024 * 1024,
                     [](BlockOfCode&) {}};
    RegAlloc alloc{BuildRegSet({HostLoc::RAX, HostLoc::RCX}), {}};
    IR::Block block{IR::LocationDescriptor{0}};
    const auto value = [&block](unsigned name) {
        auto inst = block.AppendNewInst(IR::Opcode::Add64,
                                       {IR::Value{u64{1}}, IR::Value{u64{2}}, IR::Value{false}});
        inst->SetName(name);
        return &*inst;
    };
    auto* first = value(4095);
    auto* second = value(4096);
    auto alias = block.AppendNewInst(IR::Opcode::Identity, {IR::Value{first}});
    alias->SetName(1);
    auto first_user = block.AppendNewInst(IR::Opcode::Identity, {IR::Value{first}});
    auto second_user = block.AppendNewInst(IR::Opcode::Identity, {IR::Value{second}});
    auto alias_user = block.AppendNewInst(IR::Opcode::Identity, {IR::Value{&*alias}});

    alloc.DefineValue(code, first, alloc.ScratchGpr(code, HostLoc::RAX));
    alloc.EndOfAllocScope();
    alloc.DefineValue(code, second, alloc.ScratchGpr(code, HostLoc::RCX));
    alloc.EndOfAllocScope();
    auto alias_args = alloc.GetArgumentInfo(&*alias);
    alloc.DefineValue(code, &*alias, alias_args[0]);
    alloc.EndOfAllocScope();

    // Both registers are occupied: moving first to RCX must exchange them and
    // update every alias, including the names on either side of the table limit.
    auto args = alloc.GetArgumentInfo(&*first_user);
    alloc.Use(code, args[0], HostLoc::RCX);
    alloc.EndOfAllocScope();
    auto alias_use = alloc.GetArgumentInfo(&*alias_user);
    REQUIRE(alloc.UseGpr(code, alias_use[0]).getIdx() == code.rcx.getIdx());
    alloc.EndOfAllocScope();
    auto second_use = alloc.GetArgumentInfo(&*second_user);
    REQUIRE(alloc.UseGpr(code, second_use[0]).getIdx() == code.rax.getIdx());
    alloc.EndOfAllocScope();
    alloc.AssertNoMoreUses();
}
