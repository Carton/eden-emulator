// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <chrono>
#include <cstring>
#include <iterator>
#include <string>
#include <utility>

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_cache_file.h"

namespace {
struct CacheFile {
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
        ("eden-pipeline-cache-test-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::path path = directory / "cache.bin";
    static constexpr std::array<char, 4> magic{'t', 'e', 's', 't'};

    CacheFile() {
        std::filesystem::create_directory(directory);
        std::ofstream(path, std::ios::binary) << "previous cache";
    }
    ~CacheFile() {
        std::error_code ignored;
        std::filesystem::remove_all(directory, ignored);
    }
    std::string Read() const {
        std::ifstream file(path, std::ios::binary);
        return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
    }
    template <typename Read>
    void Save(std::size_t limit, Read&& read) {
        Vulkan::SavePipelineCacheFile(path, magic, 18, limit, std::forward<Read>(read));
    }
};
} // namespace

TEST_CASE("Driver cache replaces an existing file only after a complete snapshot",
          "[video_core][pipeline_cache]") {
    CacheFile cache;
    cache.Save(32, [](std::size_t* size, void* data) {
        if (!data) {
            *size = 16;
        } else {
            // Mirrors VK_INCOMPLETE: only the returned byte count may be persisted.
            std::memcpy(data, "valid", 5);
            *size = 5;
        }
    });
    const auto contents = cache.Read();
    REQUIRE(contents.size() == 4 + sizeof(std::uint32_t) + 5);
    REQUIRE(contents.substr(0, 4) == "test");
    REQUIRE(contents.substr(8) == "valid");
    REQUIRE_FALSE(std::filesystem::exists(cache.path.string() + ".tmp"));
}

TEST_CASE("Driver cache preserves the last good file on snapshot failures",
          "[video_core][pipeline_cache]") {
    CacheFile cache;
    SECTION("Memory budget rejects before allocating or fetching the payload") {
        unsigned calls{};
        REQUIRE_THROWS_AS(cache.Save(32, [&](std::size_t* size, void*) {
            ++calls;
            *size = 116740934; // 22:43 crash's requested snapshot size.
        }), std::length_error);
        REQUIRE(calls == 1);
    }
    SECTION("Allocation failure propagates to the nonfatal save boundary") {
        REQUIRE_THROWS_AS(cache.Save(32, [](std::size_t*, void*) {
            throw std::bad_alloc{};
        }), std::bad_alloc);
    }
    SECTION("Driver failure after the size query") {
        REQUIRE_THROWS_AS(cache.Save(32, [](std::size_t* size, void* data) {
            if (data) {
                throw std::runtime_error("VK_ERROR_OUT_OF_HOST_MEMORY");
            }
            *size = 16;
        }), std::runtime_error);
    }
    SECTION("Invalid driver output size") {
        REQUIRE_THROWS_AS(cache.Save(32, [](std::size_t* size, void* data) {
            *size = data ? 33 : 16;
        }), std::length_error);
    }
    REQUIRE(cache.Read() == "previous cache");
    REQUIRE_FALSE(std::filesystem::exists(cache.path.string() + ".tmp"));
}

TEST_CASE("Driver cache preserves the previous file when the temporary file cannot open",
          "[video_core][pipeline_cache]") {
    CacheFile cache;
    std::filesystem::create_directory(cache.path.string() + ".tmp");
    REQUIRE_THROWS_AS(cache.Save(32, [](std::size_t* size, void*) { *size = 0; }),
                      std::ios_base::failure);
    REQUIRE(cache.Read() == "previous cache");
}

TEST_CASE("Driver cache cleans up its temporary file when publication fails",
          "[video_core][pipeline_cache]") {
    CacheFile cache;
    const auto previous = cache.path;
    cache.path = cache.directory / "blocked-destination";
    std::filesystem::create_directory(cache.path);
    REQUIRE_THROWS_AS(cache.Save(32, [](std::size_t* size, void*) { *size = 0; }),
                      std::filesystem::filesystem_error);
    REQUIRE_FALSE(std::filesystem::exists(cache.path.string() + ".tmp"));
    REQUIRE(std::filesystem::is_directory(cache.path));
    cache.path = previous;
    REQUIRE(cache.Read() == "previous cache");
}
