// (local-only) Standalone regression tests; no emulator, driver or game required.
// cl /std:c++20 /EHsc /utf-8 /I src tools/windows/test_uniform_epoch.cpp
#include <cstdio>
#include <cstdlib>
#include "video_core/control/engine_override.h"

#define CHECK(expr) do { if (!(expr)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); std::exit(1); } } while (false)

int main() {
    using VideoCommon::UniformEpochTable;
    UniformEpochTable table;
    bool valid = true;
    CHECK(table.Acquire(0x1000, 0, valid) == nullptr);
    CHECK(!valid);
    CHECK(table.Acquire(0x1000, UniformEpochTable::kSlotBytes + 1, valid) == nullptr);

    // Fill every slot with distinct keys. Hashes select their exact slots.
    table.BeginCapture();
    for (size_t i = 0; i < table.kSlots; ++i) {
        auto* slot = table.Acquire(0x10000 + i * 256, 32, valid);
        CHECK(slot != nullptr && !valid);
        slot[0] = static_cast<u8>(i + 1);
    }
    CHECK(table.Acquire(0x20000, 32, valid) == nullptr);

    // A hit at the next clock victim must remain intact when a new key is
    // captured later in the SAME draw. The old code evicted this hit.
    table.BeginCapture();
    auto* first = table.Acquire(0x10000, 32, valid);
    CHECK(first != nullptr && valid && first[0] == 1);
    auto* second = table.Acquire(0x20000, 32, valid);
    CHECK(second != nullptr && !valid && second != first);
    second[0] = 0xee;
    CHECK(first[0] == 1);
    CHECK(table.Acquire(0x10000, 32, valid) == first && valid);

    // Different sizes at the same address own different, pinned slots.
    auto* different_size = table.Acquire(0x10000, 64, valid);
    CHECK(different_size != nullptr && !valid && different_size != first);
    different_size[0] = 0xdd;
    CHECK(first[0] == 1);

    // A new draw releases the pins, but preserves content-valid cache hits.
    table.BeginCapture();
    CHECK(table.Acquire(0x20000, 32, valid) == second && valid);
    CHECK(second[0] == 0xee);
    std::puts("Uniform epoch regression tests passed");
}
