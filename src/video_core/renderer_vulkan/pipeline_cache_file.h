// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <stdexcept>
#include <vector>

namespace Vulkan {

// Read must check the driver's result and return the number of bytes actually written.
// Snapshot before opening any file: allocation/driver failures must preserve the old cache.
template <typename Read>
void SavePipelineCacheFile(const std::filesystem::path& filename, std::span<const char> magic,
                           std::uint32_t version, std::size_t snapshot_limit, Read&& read) {
    std::size_t size{};
    read(&size, nullptr);
    if (size > snapshot_limit) {
        throw std::length_error("Pipeline cache snapshot exceeds the available memory budget");
    }
    std::vector<char> data(size);
    if (size != 0) {
        read(&size, data.data());
        if (size > data.size()) {
            throw std::length_error("Pipeline cache driver returned an invalid snapshot size");
        }
    }

    auto temporary = filename;
    temporary += ".tmp";
    try {
        std::ofstream file;
        file.exceptions(std::ios::failbit | std::ios::badbit);
        file.open(temporary, std::ios::binary | std::ios::trunc);
        file.write(magic.data(), static_cast<std::streamsize>(magic.size()));
        file.write(reinterpret_cast<const char*>(&version), sizeof(version));
        if (size != 0) {
            file.write(data.data(), static_cast<std::streamsize>(size));
        }
        file.close(); // Detect buffered write failures before replacing the last good cache.
        std::filesystem::rename(temporary, filename);
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

} // namespace Vulkan
