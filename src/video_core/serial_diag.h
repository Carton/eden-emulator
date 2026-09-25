// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

namespace VideoCommon {

inline bool SerialDiagEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_SERIAL_DIAG");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

} // namespace VideoCommon
