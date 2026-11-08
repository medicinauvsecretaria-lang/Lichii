// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <mutex>
#include <optional>

#include "common/common_types.h"

namespace Vulkan::GplCrashGuard {

/// Self-healing protection for drivers that crash inside graphics pipeline library calls (seen on
/// AMD's Windows driver with RDNA2 cards: an access violation in amdvlk64.dll while building
/// shader libraries).
///
/// Every guarded driver call writes "B <kind> <id>" before it and "E <kind> <id>" after it to
/// <shader dir>/gpl_guard_journal.txt, flushed at once. At the next start, any call that began
/// and never ended is taken as the one the driver crashed on: its id goes to
/// gpl_guard_blocklist.txt and that library or pipeline is built monolithic from then on, while
/// every other pipeline keeps using libraries. After a crash the guarded calls are also made one
/// at a time, in case the driver fails only when several run at once. After
/// MAX_CRASHES crashes in consecutive sessions the libraries are switched off, so the game
/// always starts; one session without a crash turns them back on. Faults the driver guard
/// caught (the game kept running) block their library or pipeline without counting as crashes.
/// Deleting the gpl_guard_* files resets it. EDEN_NX2_GPL_GUARD=0 turns it off.

inline constexpr u32 MAX_CRASHES = 3;

enum class Kind : char {
    Library = 'L',  ///< A pre-rasterization or fragment shader library.
    Pipeline = 'P', ///< Vertex input / fragment output libraries and the link of a pipeline.
};

/// False when libraries must not be used at all (too many crashes).
bool LibrariesAllowed();

/// True after the driver faulted inside a guarded call this session (caught, see
/// vk::GuardedCreateGraphicsPipelines): new pipelines are built monolithic until restart.
bool SessionDisabled();

/// True when this library or pipeline crashed the driver before and must be built monolithic.
bool IsBlocked(Kind kind, u64 id);

/// Stable id of a shader library from its part (0 pre-rasterization, 1 fragment) and the hash
/// of the program it is named after.
template <typename Part>
constexpr u64 LibraryId(Part part, u64 program_hash) {
    return static_cast<size_t>(part) == 0 ? program_hash : program_hash ^ 0x9E3779B97F4A7C15ULL;
}

/// Marks one guarded driver call; a no-op when the guard is off.
class Scope {
public:
    Scope(Kind kind, u64 id);
    ~Scope();

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

private:
    Kind kind;
    u64 id;
    bool active{};
    std::optional<std::unique_lock<std::mutex>> serial;
};

} // namespace Vulkan::GplCrashGuard
