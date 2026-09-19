// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

namespace Tegra::Engines {
class Maxwell3D;
}

namespace VideoCommon {

// (local-only) P2 parallel draw resolver: a thread with this set reads engine
// state from the draw snapshot instead of the live engine. Only the resolver
// thread (during the resolve phase) and the GPU thread (while committing a
// resolved draw) ever set it; every other thread leaves it null and observes
// the live engine.
inline thread_local Tegra::Engines::Maxwell3D* tls_engine_snapshot = nullptr;

} // namespace VideoCommon
