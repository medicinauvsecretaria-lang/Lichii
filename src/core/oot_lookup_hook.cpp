// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>

#include "common/hex_util.h"
#include "common/logging.h"
#include "core/arm/arm_interface.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/kernel/svc_types.h"
#include "core/memory.h"
#include "core/oot_lookup_hook.h"
#include "video_core/nx2_debug_control.h"

namespace Core::OotLookup {
namespace {

constexpr u64 ProgramId = 0x0400486024DEC000ULL;
constexpr std::string_view MainBuildId = "E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348";

/// One loop of the search. At its head the game computes `add w8, w<lo>, w<hi>`, then
/// mid = w8 >> 1, reads the entry (index mid, or 0 past the count), checks that the comparator's
/// method (vtable + 0x78) is one of the two it inlines and that the table and key hold names,
/// compares the names byte by byte (unsigned) and moves lo or hi.
struct Site {
    u64 offset;   ///< Loop head in main, replaced by the SVC.
    u32 original; ///< The replaced instruction, add w8, w<lo>, w<hi>.
    int lo;       ///< Register holding lo.
    int hi;       ///< Register holding hi (exclusive).
    int method;   ///< Register holding the first inlined comparator method (the second is x24).
    int key_kind; ///< Register holding the key kind the inlined path expects.
    int minus_one; ///< Register the comparison's sign comes from (cneg wN): must hold -1.
    bool upper;   ///< Upper bound (entry <= key moves lo) instead of lower bound (entry < key).
};

constexpr std::array<Site, 2> Sites{{
    // main+0x58FBD4: lower bound; lo w25, hi w26; methods x7 / x24; key kind in w22; -1 in w27.
    {0x58FBD4, 0x0B1A0328, 25, 26, 7, 22, 27, false},
    // main+0x591D98: upper bound; lo w23, hi w3 (w26 holds a copy at the head); methods x5 / x24;
    // key kind in w7; -1 in w22.
    {0x591D98, 0x0B0302E8, 23, 3, 5, 7, 22, true},
}};

constexpr u32 NameKind = 8;              ///< Key kind of a name (cmp w, #8 in the game).
constexpr u64 MaxNameBytes = 64 * 1024;  ///< Longer names are left to the game.
constexpr u64 PageSize = 0x1000;

std::atomic<u64> module_base{};
std::atomic<bool> hooked{};
std::atomic<u64> calls{};
std::atomic<u64> host_steps{};

/// Whether the guest range [address, address + size) can be read.
bool Readable(Core::Memory::Memory& memory, u64 address, u64 size) {
    return address != 0 && memory.IsValidVirtualAddressRange(address, size);
}

/// strcmp of two guest names, as the game's loop does it (unsigned bytes; the entry ending first
/// is the smaller). False when a byte can't be read or the names are too long.
bool CompareNames(Core::Memory::Memory& memory, u64 entry, u64 key, int& order) {
    const u8* a = nullptr;
    const u8* b = nullptr;
    u64 left_a = 0;
    u64 left_b = 0;
    for (u64 i = 0; i < MaxNameBytes; ++i) {
        if (left_a == 0) {
            const u64 address = entry + i;
            if (!memory.IsValidVirtualAddress(address)) {
                return false;
            }
            a = memory.GetPointer(address);
            if (a == nullptr) {
                return false;
            }
            left_a = PageSize - (address & (PageSize - 1));
        }
        if (left_b == 0) {
            const u64 address = key + i;
            if (!memory.IsValidVirtualAddress(address)) {
                return false;
            }
            b = memory.GetPointer(address);
            if (b == nullptr) {
                return false;
            }
            left_b = PageSize - (address & (PageSize - 1));
        }
        const u8 x = *a++;
        const u8 y = *b++;
        --left_a;
        --left_b;
        if (x == 0) {
            order = y == 0 ? 0 : -1;
            return true;
        }
        if (x != y) {
            order = x < y ? -1 : 1;
            return true;
        }
    }
    return false;
}

/// Runs search steps on the host while at least three entries are left (so the game's code
/// always runs at least one more step from a range it would also have reached). Updates lo/hi.
u64 HostSteps(Core::Memory::Memory& memory, const Kernel::Svc::ThreadContext& ctx,
              const Site& site, u32& lo, u32& hi) {
    if (static_cast<u32>(ctx.r[site.minus_one]) != 0xFFFFFFFFu ||
        static_cast<u32>(ctx.r[site.key_kind]) != NameKind) {
        return 0;
    }
    const u64 table = ctx.r[20];
    const u64 key_holder = ctx.r[19];
    const u64 key_kind_address = ctx.r[17];
    const u64 method_a = ctx.r[site.method];
    const u64 method_b = ctx.r[24];
    if (!Readable(memory, table, 0x1C) || !Readable(memory, key_kind_address, 4) ||
        !Readable(memory, key_holder + 0xD8, 8)) {
        return 0;
    }
    u64 steps = 0;
    while (true) {
        const s64 low = static_cast<s32>(lo);
        const s64 high = static_cast<s32>(hi);
        if (low < 0 || high - low < 3) {
            break;
        }
        // The game reads these every step; so does this.
        const u32 count = memory.Read32(table);
        const u64 entries = memory.Read64(table + 0x8);
        const u64 comparator = memory.Read64(table + 0x10);
        const u32 table_kind = memory.Read32(table + 0x18);
        if (table_kind != 0 || !Readable(memory, comparator, 8)) {
            break;
        }
        const u64 vtable = memory.Read64(comparator);
        if (!Readable(memory, vtable + 0x78, 8)) {
            break;
        }
        const u64 method = memory.Read64(vtable + 0x78);
        if (method != method_a && method != method_b) {
            break;
        }
        if (memory.Read32(key_kind_address) != NameKind) {
            break;
        }
        const u32 mid = (lo + hi) >> 1;
        const u32 index = mid < count ? mid : 0;
        const u64 slot = entries + u64{index} * 8;
        if (!Readable(memory, slot, 8)) {
            break;
        }
        const u64 entry = memory.Read64(slot);
        if (!Readable(memory, entry, 8)) {
            break;
        }
        const u64 entry_name = memory.Read64(entry);
        const u64 key_name = memory.Read64(key_holder + 0xD8);
        if (entry_name == key_name) {
            break; // the game has its own path for the same name pointer
        }
        int order = 0;
        if (!CompareNames(memory, entry_name, key_name, order)) {
            break;
        }
        const bool entry_first = site.upper ? order <= 0 : order < 0;
        if (entry_first) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
        ++steps;
    }
    return steps;
}

bool CodeMatches(std::span<const u8> image, u64 offset, u32 word) {
    if (offset + sizeof(word) > image.size()) {
        return false;
    }
    u32 current{};
    std::memcpy(&current, image.data() + offset, sizeof(current));
    return current == word;
}

} // Anonymous namespace

void PatchModule(u64 program_id, std::span<const u8> build_id, std::string_view name,
                 std::span<u8> image, u64 load_base) {
    if (program_id != ProgramId || name != "main") {
        return;
    }
    const char* value = std::getenv("EDEN_NX2_OOT_NATIVE_LOOKUP");
    if (value == nullptr || value[0] != '1') {
        return; // off unless asked for
    }
    if (build_id.size() < 20 || Common::HexToString(build_id.first(20)) != MainBuildId) {
        LOG_INFO(Core, "Ocarina lookup: other build of main, the game's search stays");
        return;
    }
    for (const Site& site : Sites) {
        if (!CodeMatches(image, site.offset, site.original)) {
            LOG_WARNING(Core, "Ocarina lookup: unexpected code at main+{:#x}, the game's search "
                              "stays",
                        site.offset);
            return;
        }
    }
    const u32 svc = 0xd4000001 | (SvcNumber << 5);
    for (const Site& site : Sites) {
        std::memcpy(image.data() + site.offset, &svc, sizeof(svc));
    }
    module_base.store(load_base, std::memory_order_relaxed);
    hooked.store(true, std::memory_order_relaxed);
    LOG_INFO(Core, "Ocarina lookup: name search loops at {:#x} and {:#x} step on the host "
                   "(EDEN_NX2_EXP50=1 turns that off live)",
             load_base + Sites[0].offset, load_base + Sites[1].offset);
}

void Handle(Core::System& system) {
    auto& kernel = system.Kernel();
    auto* const process = Kernel::GetCurrentProcessPointer(kernel);
    if (process == nullptr) {
        return;
    }
    auto* const arm = process->GetArmInterface(kernel.CurrentPhysicalCoreIndex());
    if (arm == nullptr) {
        return;
    }
    Kernel::Svc::ThreadContext ctx{};
    arm->GetContext(ctx);
    // The JIT reports the instruction after the SVC, where the loop body goes on.
    const u64 offset = ctx.pc - 4 - module_base.load(std::memory_order_relaxed);
    const Site* site = nullptr;
    for (const Site& candidate : Sites) {
        if (candidate.offset == offset) {
            site = &candidate;
        }
    }
    if (site == nullptr || !hooked.load(std::memory_order_relaxed)) {
        LOG_ERROR(Core, "Ocarina lookup: SVC {:#x} from unexpected address {:#x}", SvcNumber,
                  ctx.pc - 4);
        return;
    }
    u32 lo = static_cast<u32>(ctx.r[site->lo]);
    u32 hi = static_cast<u32>(ctx.r[site->hi]);
    if (VideoCore::Nx2Debug::Experiment(50) == 0) {
        auto& memory = process->GetMemory();
        const u64 steps = HostSteps(memory, ctx, *site, lo, hi);
        host_steps.fetch_add(steps, std::memory_order_relaxed);
        ctx.r[site->lo] = lo;
        ctx.r[site->hi] = hi;
        if (site->upper) {
            ctx.r[26] = hi; // the loop head's copy of hi (mov w3, w26 before it)
        }
    }
    // The replaced instruction: add w8, w<lo>, w<hi> (a 32-bit result, upper half cleared).
    ctx.r[8] = static_cast<u32>(lo + hi);
    arm->SetContext(ctx);
    const u64 n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
    if ((n & (n - 1)) == 0 && n >= (1ULL << 16)) {
        LOG_INFO(Core, "Ocarina lookup: {} searches, {} steps on the host", n,
                 host_steps.load(std::memory_order_relaxed));
    }
}

} // namespace Core::OotLookup
