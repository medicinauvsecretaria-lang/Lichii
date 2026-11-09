// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "qt_common/windows_scheduling.h"

#ifdef _WIN32
#include <cstdlib>
#include <cstring>

#include <windows.h>

#include "common/logging.h"

void TuneWindowsScheduling() {
    // The emulator is CPU bound. Windows power throttling (EcoQoS) may run a process it takes for
    // background work at lower clocks or on efficiency cores, and on Windows 11 it ignores the
    // 0.5 ms timer resolution of a window that is hidden or out of focus. Opt out of both.
    PROCESS_POWER_THROTTLING_STATE throttling{};
    throttling.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    throttling.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
#ifdef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
    throttling.ControlMask |= PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
#endif
    throttling.StateMask = 0; // 0 = not throttled, timer requests honored
    const bool unthrottled = SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                                                   &throttling, sizeof(throttling)) != FALSE;

    // ABOVE_NORMAL so a browser, Discord or an overlay does not take CPU time from the emulation
    // and GPU threads. EDEN_NX2_PRIORITY=normal keeps the default class.
    const char* const priority = std::getenv("EDEN_NX2_PRIORITY");
    const bool keep_normal = priority != nullptr && std::strcmp(priority, "normal") == 0;
    const bool raised =
        !keep_normal && SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS) != FALSE;
    LOG_INFO(Frontend, "Nx2Perf scheduling: power throttling {}, process priority {}",
             unthrottled ? "off" : "unchanged", raised ? "above normal" : "normal");
}

#else

void TuneWindowsScheduling() {}

#endif
