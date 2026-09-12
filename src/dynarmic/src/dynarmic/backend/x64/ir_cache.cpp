// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// See ir_cache.h for design notes. Local profiling feature, not upstream.

#include "dynarmic/backend/x64/ir_cache.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <limits>
#include <shared_mutex>
#include <unordered_map>

#include "dynarmic/backend/x64/jit_stats.h"
#include "dynarmic/ir/cond.h"
#include "dynarmic/ir/opcodes.h"
#include "dynarmic/ir/terminal.h"

namespace Dynarmic::Backend::X64 {

namespace {

constexpr std::uint32_t kMagic = 0x45495243;  // 'EIRC'
constexpr std::uint32_t kVersion = 2;
constexpr std::size_t kValueSize = sizeof(IR::Value);

// Bounded byte reader/writer over a flat buffer.
struct Buffer {
    std::vector<std::uint8_t>* out = nullptr;  // write mode when non-null
    const std::uint8_t* in = nullptr;          // read mode
    std::size_t in_size = 0;
    std::size_t pos = 0;

    void W(const void* p, std::size_t n) {
        const auto* b = static_cast<const std::uint8_t*>(p);
        out->insert(out->end(), b, b + n);
    }
    template <typename T>
    void Wt(T v) {
        W(&v, sizeof(T));
    }
    bool R(void* p, std::size_t n) {
        if (n > in_size - pos) {
            return false;
        }
        std::memcpy(p, in + pos, n);
        pos += n;
        return true;
    }
    template <typename T>
    bool Rt(T& v) {
        return R(&v, sizeof(T));
    }
};

void WriteValue(Buffer& b, const IR::Value& value) {
    const bool imm = value.IsImmediate();
    b.Wt<std::uint8_t>(imm ? 1 : 0);
    if (imm) {
        // IR::Value is trivially copyable and self-describing; keep exact bits.
        b.W(&value, kValueSize);
    }
    // Non-immediate values are written separately with their inst index so
    // that all instructions can be allocated before arguments are linked.
}

void WriteTerminal(Buffer& b, const IR::Terminal& term) {
    b.Wt<std::uint8_t>(static_cast<std::uint8_t>(term.which()));
    switch (term.which()) {
    case 2: {  // LinkBlock
        b.Wt<std::uint64_t>(boost::get<IR::Term::LinkBlock>(term).next.Value());
        break;
    }
    case 3: {  // LinkBlockFast
        b.Wt<std::uint64_t>(boost::get<IR::Term::LinkBlockFast>(term).next.Value());
        break;
    }
    case 6: {  // If
        const auto& t = boost::get<IR::Term::If>(term);
        b.Wt<std::uint8_t>(static_cast<std::uint8_t>(t.if_));
        WriteTerminal(b, t.then_);
        WriteTerminal(b, t.else_);
        break;
    }
    case 7: {  // CheckBit
        const auto& t = boost::get<IR::Term::CheckBit>(term);
        WriteTerminal(b, t.then_);
        WriteTerminal(b, t.else_);
        break;
    }
    case 8: {  // CheckHalt
        WriteTerminal(b, boost::get<IR::Term::CheckHalt>(term).else_);
        break;
    }
    default:
        break;  // Invalid/ReturnToDispatch/PopRSBHint/FastDispatchHint: no payload
    }
}

}  // namespace

std::vector<std::uint8_t> IRCache::Serialize(const IR::Block& block) {
    std::vector<std::uint8_t> out;
    Buffer b;
    b.out = &out;
    b.Wt<std::uint32_t>(kMagic);
    b.Wt<std::uint32_t>(kVersion);
    b.Wt<std::uint64_t>(block.EndLocation().Value());
    b.Wt<std::uint8_t>(static_cast<std::uint8_t>(block.GetCondition()));
    const bool has_cf = block.HasConditionFailedLocation();
    b.Wt<std::uint8_t>(has_cf ? 1 : 0);
    if (has_cf) {
        b.Wt<std::uint64_t>(block.ConditionFailedLocation().Value());
    }
    b.Wt<std::uint64_t>(block.ConditionFailedCycleCount());
    b.Wt<std::uint64_t>(block.CycleCount());

    // Instructions: header (opcode/name) pass, then inst-ref indices pass.
    std::vector<const IR::Inst*> insts;
    insts.reserve(32);
    std::unordered_map<const IR::Inst*, std::uint32_t> indices;
    for (const auto& inst : block.Instructions()) {
        indices.emplace(&inst, static_cast<std::uint32_t>(insts.size()));
        insts.push_back(&inst);
    }
    b.Wt<std::uint32_t>(static_cast<std::uint32_t>(insts.size()));
    for (const IR::Inst* inst : insts) {
        b.Wt<std::uint16_t>(static_cast<std::uint16_t>(inst->GetOpcode()));
        b.Wt<std::uint32_t>(inst->GetName());
        const std::size_t nargs = inst->NumArgs();
        b.Wt<std::uint8_t>(static_cast<std::uint8_t>(nargs));
        for (std::size_t i = 0; i < nargs; i++) {
            WriteValue(b, inst->GetArg(i));
        }
    }
    // Resolve non-immediate args to indices (indices of definitions, which
    // must precede uses in this linear IR; assert-checked on load).
    for (const IR::Inst* inst : insts) {
        const std::size_t nargs = inst->NumArgs();
        for (std::size_t i = 0; i < nargs; i++) {
            const IR::Value& v = inst->GetArg(i);
            if (!v.IsImmediate()) {
                const IR::Inst* target = v.GetInst();
                b.Wt<std::uint32_t>(indices.at(target));
            }
        }
    }

    WriteTerminal(b, block.GetTerminal());
    return out;
}

bool IRCache::Load(const Entry& entry, IR::Block& block) {
    Buffer b;
    b.in = entry.bytes.data();
    b.in_size = entry.bytes.size();

    std::uint32_t magic = 0, version = 0;
    if (!b.Rt(magic) || !b.Rt(version) || magic != kMagic || version != kVersion) {
        return false;
    }
    std::uint64_t end_loc = 0;
    if (!b.Rt(end_loc)) {
        return false;
    }
    block.SetEndLocation(IR::LocationDescriptor{end_loc});
    std::uint8_t cond = 0;
    if (!b.Rt(cond)) {
        return false;
    }
    block.SetCondition(static_cast<IR::Cond>(cond));
    std::uint8_t has_cf = 0;
    if (!b.Rt(has_cf)) {
        return false;
    }
    if (has_cf) {
        std::uint64_t cf = 0;
        if (!b.Rt(cf)) {
            return false;
        }
        block.SetConditionFailedLocation(IR::LocationDescriptor{cf});
    }
    std::uint64_t cfcc = 0, cc = 0;
    if (!b.Rt(cfcc) || !b.Rt(cc)) {
        return false;
    }
    block.ConditionFailedCycleCount() = cfcc;
    block.CycleCount() = cc;

    std::uint32_t count = 0;
    if (!b.Rt(count) || count > 100000) {
        return false;
    }

    // Phase 1: allocate every instruction with its opcode (mirrors the pool
    // logic of IR::Block::PrependNewInst; args are linked in phase 2 so that
    // forward references would also work).
    std::vector<IR::Inst*> insts;
    insts.reserve(count);
    struct PendingArg {
        std::uint16_t op;
        std::uint32_t name;
    };
    std::vector<PendingArg> pending;
    pending.reserve(count);
    // Per-instruction immediate args are read first; non-immediate arg slots
    // are read here as placeholders and fixed up by the index stream below.
    struct ArgSlot {
        IR::Value value;      // set for immediates
        bool is_ref = false;  // true: value to be filled from index stream
    };
    std::vector<std::vector<ArgSlot>> arg_slots(count);

    for (std::uint32_t n = 0; n < count; n++) {
        std::uint16_t op = 0;
        std::uint32_t name = 0;
        std::uint8_t nargs_raw = 0;
        if (!b.Rt(op) || !b.Rt(name) || !b.Rt(nargs_raw)) {
            return false;
        }
        const auto opcode = static_cast<IR::Opcode>(op);
        if (op >= IR::OpcodeCount) {
            return false;
        }
        const std::size_t nargs = IR::GetNumArgsOf(opcode);
        if (nargs != nargs_raw) {
            return false;  // payload disagrees with opcode table
        }
        pending.push_back({op, name});
        arg_slots[n].reserve(nargs);
        for (std::size_t i = 0; i < nargs; i++) {
            std::uint8_t imm = 0;
            if (!b.Rt(imm)) {
                return false;
            }
            ArgSlot slot;
            if (imm) {
                IR::Value v;
                if (!b.R(&v, kValueSize)) {
                    return false;
                }
                slot.value = v;
            } else {
                slot.is_ref = true;
            }
            arg_slots[n].push_back(slot);
        }
    }
    // Index stream for non-immediate args, in the same traversal order.
    auto next_index = [&](std::uint32_t& index) -> bool {
        return b.Rt(index);
    };
    for (std::uint32_t n = 0; n < count; n++) {
        const auto opcode = static_cast<IR::Opcode>(pending[n].op);
        IR::Inst* inst;
        if (block.inlined_inst.size() < block.inlined_inst.max_size()) {
            block.inlined_inst.emplace_back(opcode);
            inst = &block.inlined_inst.back();
        } else {
            if (block.pooled_inst.empty() ||
                block.pooled_inst.back().size() == block.pooled_inst.back().max_size()) {
                block.pooled_inst.emplace_back();
            }
            block.pooled_inst.back().emplace_back(opcode);
            inst = &block.pooled_inst.back().back();
        }
        inst->SetName(pending[n].name);
        block.instructions.push_back(inst);
        insts.push_back(inst);
    }
    for (std::uint32_t n = 0; n < count; n++) {
        IR::Inst* inst = insts[n];
        const std::size_t nargs = arg_slots[n].size();
        for (std::size_t i = 0; i < nargs; i++) {
            if (arg_slots[n][i].is_ref) {
                std::uint32_t index = 0;
                if (!next_index(index) || index >= insts.size()) {
                    return false;
                }
                // SetArg maintains use_count and pseudo-op links.
                inst->SetArg(i, IR::Value{insts[index]});
            } else {
                inst->SetArg(i, arg_slots[n][i].value);
            }
        }
    }

    // Terminal (recursive).
    std::function<IR::Terminal(bool&)> read_term = [&](bool& ok) -> IR::Terminal {
        std::uint8_t which = 0;
        if (!b.Rt(which)) {
            ok = false;
            return IR::Term::Invalid{};
        }
        auto read_loc = [&](IR::LocationDescriptor& d) -> bool {
            std::uint64_t v = 0;
            if (!b.Rt(v)) {
                return false;
            }
            d = IR::LocationDescriptor{v};
            return true;
        };
        switch (which) {
        case 1:
            return IR::Term::ReturnToDispatch{};
        case 2: {
            IR::LocationDescriptor d{0};
            if (!read_loc(d)) {
                ok = false;
                return IR::Term::Invalid{};
            }
            return IR::Term::LinkBlock{d};
        }
        case 3: {
            IR::LocationDescriptor d{0};
            if (!read_loc(d)) {
                ok = false;
                return IR::Term::Invalid{};
            }
            return IR::Term::LinkBlockFast{d};
        }
        case 4:
            return IR::Term::PopRSBHint{};
        case 5:
            return IR::Term::FastDispatchHint{};
        case 6: {
            std::uint8_t cond = 0;
            if (!b.Rt(cond)) {
                ok = false;
                return IR::Term::Invalid{};
            }
            IR::Terminal t = read_term(ok);
            IR::Terminal e = read_term(ok);
            if (!ok) {
                return IR::Term::Invalid{};
            }
            return IR::Term::If{static_cast<IR::Cond>(cond), t, e};
        }
        case 7: {
            IR::Terminal t = read_term(ok);
            IR::Terminal e = read_term(ok);
            if (!ok) {
                return IR::Term::Invalid{};
            }
            return IR::Term::CheckBit{t, e};
        }
        case 8: {
            IR::Terminal e = read_term(ok);
            if (!ok) {
                return IR::Term::Invalid{};
            }
            return IR::Term::CheckHalt{e};
        }
        default:
            ok = false;
            return IR::Term::Invalid{};
        }
    };
    bool ok = true;
    IR::Terminal term = read_term(ok);
    if (!ok) {
        return false;
    }
    block.SetTerminal(std::move(term));
    return true;
}

namespace {

struct SharedState {
    std::shared_mutex mutex;
    std::unordered_map<std::uint64_t, IRCache::EntryPtr> map;
    std::size_t total_bytes = 0;
    std::size_t jit_count = 0;
};

SharedState& GetState() {
    static SharedState state;
    return state;
}

bool EnvEnabled() {
    // Default on; EDEN_JIT_IRCACHE=0 disables.
    const char* env = std::getenv("EDEN_JIT_IRCACHE");
    return !(env != nullptr && env[0] == '0');
}

std::size_t MaxCacheBytes() {
    // Payload allocation budget, excluding map nodes and allocator overhead.
    constexpr std::size_t kDefaultMax = std::size_t(512) << 20;
    const char* env = std::getenv("EDEN_JIT_IRCACHE_MAXBYTES");
    if (env == nullptr || env[0] == '\0') {
        return kDefaultMax;
    }
    if (env[0] < '0' || env[0] > '9') {
        return kDefaultMax;
    }
    char* end = nullptr;
    errno = 0;
    const unsigned long long v = std::strtoull(env, &end, 10);
    return errno == ERANGE || *end != '\0' || v == 0 ||
                   v > std::numeric_limits<std::size_t>::max()
        ? kDefaultMax : static_cast<std::size_t>(v);
}

}  // namespace

bool IRCache::Enabled() {
    static const bool enabled = EnvEnabled();
    return enabled;
}

void IRCache::RegisterJit() {
    SharedState& s = GetState();
    std::unique_lock lock{s.mutex};
    ++s.jit_count;
}

void IRCache::UnregisterJit() {
    SharedState& s = GetState();
    std::unique_lock lock{s.mutex};
    if (--s.jit_count == 0) {
        decltype(s.map){}.swap(s.map);
        s.total_bytes = 0;
        JitStats::ir_cache_entries.store(0, std::memory_order_relaxed);
        JitStats::ir_cache_bytes.store(0, std::memory_order_relaxed);
    }
}

IRCache::EntryPtr IRCache::Lookup(std::uint64_t descriptor_value) {
    SharedState& s = GetState();
    std::shared_lock lock{s.mutex};
    const auto it = s.map.find(descriptor_value);
    if (it == s.map.end()) {
        return nullptr;
    }
    return it->second;
}

void IRCache::Store(std::uint64_t descriptor_value, const IR::Block& block,
                    std::uint64_t start_pc, std::uint64_t end_pc,
                    std::uint64_t content_hash, const Config& config,
                    std::vector<std::uint32_t> code) {
    // Avoid serialization once the budget is full, but allow stale entries
    // to be replaced. The exclusive-lock check below remains authoritative.
    {
        SharedState& s = GetState();
        std::shared_lock lock{s.mutex};
        const auto it = s.map.find(descriptor_value);
        if (it == s.map.end() && s.total_bytes >= MaxCacheBytes()) {
            return;
        }
        if (it != s.map.end() && it->second->config == config && it->second->code == code) {
            return;
        }
    }
    auto entry = std::make_shared<Entry>();
    entry->config = config;
    entry->code = std::move(code);
    entry->bytes = Serialize(block);
    entry->start_pc = start_pc;
    entry->end_pc = end_pc;
    entry->content_hash = content_hash;

    SharedState& s = GetState();
    std::unique_lock lock{s.mutex};
    const auto allocation_size = [](const Entry& e) {
        return sizeof(Entry) + e.bytes.capacity() + e.code.capacity() * sizeof(std::uint32_t);
    };
    const auto it = s.map.find(descriptor_value);
    if (it != s.map.end() && it->second->config == config && it->second->code == entry->code) {
        return;  // another core already published the same translation
    }
    const std::size_t old_size = it == s.map.end() ? 0 : allocation_size(*it->second);
    const std::size_t new_size = allocation_size(*entry);
    const std::size_t retained_size = s.total_bytes - old_size;
    const std::size_t budget = MaxCacheBytes();
    if (retained_size > budget || new_size > budget - retained_size) {
        return;
    }
    s.map.insert_or_assign(descriptor_value, std::move(entry));
    s.total_bytes = retained_size + new_size;
    JitStats::ir_stores.fetch_add(1, std::memory_order_relaxed);
    JitStats::ir_cache_entries.store(s.map.size(), std::memory_order_relaxed);
    JitStats::ir_cache_bytes.store(s.total_bytes, std::memory_order_relaxed);
}

}  // namespace Dynarmic::Backend::X64
