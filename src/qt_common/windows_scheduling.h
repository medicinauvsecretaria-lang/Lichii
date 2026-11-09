// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// Windows: turns power throttling (EcoQoS) off for the process and raises it to ABOVE_NORMAL
/// priority (EDEN_NX2_PRIORITY=normal keeps the default). Logs what it changed. No-op elsewhere.
void TuneWindowsScheduling();
