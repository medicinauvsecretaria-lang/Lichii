// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <string_view>

#include "common/common_types.h"

namespace Core {
class System;
}

// The Legend of Zelda: Ocarina of Time (0400486024DEC000) looks its resources up by name with a
// binary search over a sorted table (main+0x58EEA0, two variants of the loop: a lower and an upper
// bound). In Hyrule Castle Town it is the hottest guest function, about an eighth of the two
// busiest guest cores even with the 8-byte strcmp patch. EDEN_NX2_OOT_NATIVE_LOOKUP=1 replaces the
// first instruction of each loop (add w8, lo, hi) with an SVC Horizon doesn't have. Handle runs the
// search steps on the host with the same reads and comparisons, while at least three entries are
// left, and always leaves the last steps to the game's own code, so the registers and stack the
// loop leaves are the game's. Anything it doesn't recognise (another comparator or key type, an
// address that isn't mapped, a very long name) is left to the game at once.
// EDEN_NX2_EXP50=1 (live) turns the host steps off for A/B tests (the SVC stays).
namespace Core::OotLookup {

/// An SVC number outside Horizon's range (next to Core::OotZstd::SvcNumber).
constexpr u32 SvcNumber = 0x7f5b;

/// Patches the module image when it is Ocarina's main of the known build.
void PatchModule(u64 program_id, std::span<const u8> build_id, std::string_view name,
                 std::span<u8> image, u64 load_base);

/// Handles SvcNumber for the current thread.
void Handle(Core::System& system);

} // namespace Core::OotLookup
