// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

namespace FrontendCommon {
// Call once at main entry, after CRT startup and before QApplication.
bool InitWindowForensics() noexcept;
void ForensicsMainReturn() noexcept;
} // namespace FrontendCommon

namespace Common::Log {
// 0 = flushed, 1 = timeout, 2 = worker/flush failure. Windows only.
unsigned long ForensicsFlushLogging() noexcept;
}
