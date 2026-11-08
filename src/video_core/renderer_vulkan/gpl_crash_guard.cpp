// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <map>
#include <string>
#include <unordered_set>
#include <utility>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/gpl_crash_guard.h"
#include "video_core/vulkan_common/driver_fault_guard.h"

namespace Vulkan::GplCrashGuard {
namespace {

u64 Combine(Kind kind, u64 id) {
    // Library and pipeline ids come from different hashes; keep them apart in one set.
    return kind == Kind::Library ? id : ~id;
}

struct State {
    bool enabled{};
    bool serialize{};
    bool allowed{true};
    u32 crashes{};
    std::unordered_set<u64> blocked;
    std::ofstream journal;
    std::mutex journal_mutex;
    std::mutex serial_mutex;
    std::atomic<bool> session_disabled{};
    std::atomic<u32> session_faults{};
    std::mutex blocked_mutex;

    State() {
        const char* const env = std::getenv("EDEN_NX2_GPL_GUARD");
        enabled = env == nullptr || *env != '0';
        if (!enabled) {
            LOG_INFO(Render_Vulkan, "Nx2Perf gpl guard: off (EDEN_NX2_GPL_GUARD=0)");
            return;
        }
        std::error_code ec;
        const auto dir = Common::FS::GetEdenPath(Common::FS::EdenPath::ShaderDir);
        std::filesystem::create_directories(dir, ec);
        const auto journal_path = dir / "gpl_guard_journal.txt";
        const auto blocklist_path = dir / "gpl_guard_blocklist.txt";
        const auto state_path = dir / "gpl_guard_state.txt";

        if (std::ifstream state_file{state_path}) {
            state_file >> crashes;
        }
        if (std::ifstream list{blocklist_path}) {
            char kind{};
            std::string hex;
            while (list >> kind >> hex) {
                blocked.insert(Combine(static_cast<Kind>(kind), std::stoull(hex, nullptr, 16)));
            }
        }

        // Calls that began in the last session and never ended: the driver crashed in them.
        std::map<std::pair<char, u64>, int> open_calls;
        if (std::ifstream old_journal{journal_path}) {
            char mark{}, kind{};
            std::string hex;
            while (old_journal >> mark >> kind >> hex) {
                u64 id{};
                try {
                    id = std::stoull(hex, nullptr, 16);
                } catch (...) {
                    continue; // a line cut short by the crash
                }
                // F: a fault the driver guard caught; blocked like an unfinished call.
                open_calls[{kind, id}] += mark == 'B' ? 1 : (mark == 'F' ? 1000 : -1);
            }
        }
        size_t newly_blocked{};
        {
            std::ofstream list{blocklist_path, std::ios::app};
            for (const auto& [call, count] : open_calls) {
                if (count <= 0) {
                    continue;
                }
                const Kind kind = static_cast<Kind>(call.first);
                if (blocked.insert(Combine(kind, call.second)).second) {
                    list << call.first << ' ' << fmt::format("{:016x}", call.second) << '\n';
                    ++newly_blocked;
                    LOG_WARNING(Render_Vulkan,
                                "Nx2Perf gpl guard: the driver crashed last session while "
                                "building {} {:016x}; it is built monolithic from now on",
                                kind == Kind::Library ? "library" : "pipeline", call.second);
                }
            }
        }
        if (newly_blocked != 0) {
            ++crashes;
            std::ofstream{state_path} << crashes << '\n';
        }
        serialize = crashes != 0;
        allowed = crashes < MAX_CRASHES;
        LOG_INFO(Render_Vulkan,
                 "Nx2Perf gpl guard: {} driver crashes recorded, {} blocked, calls {}{}", crashes,
                 blocked.size(), serialize ? "one at a time" : "in parallel",
                 allowed ? "" : "; shader libraries switched off after too many crashes");
        journal.open(journal_path, std::ios::trunc);
    }

    void Write(char mark, Kind kind, u64 id) {
        std::scoped_lock lock{journal_mutex};
        if (!journal.is_open()) {
            return;
        }
        // Flushed at once so the line is on disk if the driver crashes in the call.
        journal << mark << ' ' << static_cast<char>(kind) << ' ' << fmt::format("{:016x}", id)
                << std::endl;
    }
};

State& Get() {
    static State state;
    return state;
}

} // Anonymous namespace

bool LibrariesAllowed() {
    return Get().allowed;
}

bool SessionDisabled() {
    return Get().session_disabled.load(std::memory_order_relaxed);
}

bool IsBlocked(Kind kind, u64 id) {
    State& state = Get();
    if (!state.enabled) {
        return false;
    }
    std::scoped_lock lock{state.blocked_mutex};
    return state.blocked.contains(Combine(kind, id));
}

Scope::Scope(Kind kind_, u64 id_) : kind{kind_}, id{id_} {
    State& state = Get();
    if (!state.enabled) {
        return;
    }
    if (state.serialize) {
        serial.emplace(state.serial_mutex);
    }
    active = true;
    vk::TakeThreadDriverFault(); // a fault outside this scope belongs to someone else
    state.Write('B', kind, id);
}

Scope::~Scope() {
    if (!active) {
        return;
    }
    State& state = Get();
    if (vk::TakeThreadDriverFault()) {
        // Caught in the driver: this library or pipeline is built monolithic from now on, in
        // this session too. The others keep using libraries: in a 51 minute session the driver
        // kept working after a caught fault. Only repeated faults switch libraries off.
        state.Write('F', kind, id);
        {
            std::scoped_lock lock{state.blocked_mutex};
            state.blocked.insert(Combine(kind, id));
        }
        const u32 faults = state.session_faults.fetch_add(1, std::memory_order_relaxed) + 1;
        if (faults >= MAX_CRASHES) {
            state.session_disabled.store(true, std::memory_order_relaxed);
        }
        LOG_WARNING(Render_Vulkan,
                    "Nx2Perf gpl guard: driver fault in {} {:016x}; built monolithic from now on "
                    "({} this session{})",
                    kind == Kind::Library ? "library" : "pipeline", id, faults,
                    faults >= MAX_CRASHES ? ", shader libraries off until restart" : "");
    }
    state.Write('E', kind, id);
}

} // namespace Vulkan::GplCrashGuard
