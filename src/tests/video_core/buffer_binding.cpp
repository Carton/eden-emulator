// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstddef>
#include <memory>
#include <new>

#include <catch2/catch_test_macros.hpp>

#include "video_core/buffer_cache/buffer_cache_base.h"

TEST_CASE("Texture buffer bindings initialize reused storage", "[video_core][buffer_cache]") {
    using VideoCommon::TextureBufferBinding;
    using VideoCore::Surface::PixelFormat;
    alignas(TextureBufferBinding) std::array<std::byte, sizeof(TextureBufferBinding)> storage;
    storage.fill(std::byte{0xa5});

    // Deliberately default-initialize, just like the channel's binding arrays.
    auto* binding = ::new (storage.data()) TextureBufferBinding;
    REQUIRE(binding->device_addr == 0);
    REQUIRE(binding->size == 0);
    REQUIRE(binding->format == PixelFormat::Invalid);
    binding->device_addr = 0x1000;
    binding->size = 64;
    binding->format = PixelFormat::R8_UNORM;
    std::destroy_at(binding);

    binding = ::new (storage.data()) TextureBufferBinding;
    REQUIRE(binding->device_addr == 0);
    REQUIRE(binding->size == 0);
    REQUIRE(binding->format == PixelFormat::Invalid);
    std::destroy_at(binding);
}
