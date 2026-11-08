// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

/// On Windows, writes <eden dir>/crash_dumps/lichii-crash-<pid>.txt (faulting module+offset and
/// stack, resolvable with llvm-symbolizer) and a minidump next to it when the process crashes.
/// Fastmem faults are handled earlier by a vectored handler, so only real crashes reach it.
void InstallCrashReport();
