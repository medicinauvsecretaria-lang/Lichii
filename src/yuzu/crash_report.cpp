// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "crash_report.h"

#ifdef _WIN32
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <string>

#include <windows.h>

#include "common/fs/path_util.h"

namespace {

std::wstring dump_dir;

// MSVC C++ exception internals on x64 (offsets are relative to the throwing module's base). An
// unhandled C++ exception reaches the filter as code 0xE06D7363 with the thrown object, its
// ThrowInfo and the module base; reading them names the exception type and, for std::exception,
// its what() text, so a report tells which error escaped instead of only KERNELBASE.
constexpr DWORD CPP_EXCEPTION_CODE = 0xE06D7363;

struct ThrowInfoRaw {
    unsigned attributes;
    int unwind;
    int forward_compat;
    int catchable_type_array;
};
struct CatchableTypeArrayRaw {
    int count;
    int types[1];
};
struct CatchableTypeRaw {
    unsigned properties;
    int type;
    int mdisp;
    int pdisp;
    int vdisp;
    int size;
    int copy_function;
};
struct TypeDescriptorRaw {
    const void* vftable;
    void* spare;
    char name[1];
};

// No C++ objects with destructors here: structured exception handling cannot unwind them, and the
// guard keeps a damaged exception from crashing the report itself.
void DescribeCppException(const EXCEPTION_RECORD& record, char* out, size_t size) {
    out[0] = '\0';
    if (record.ExceptionCode != CPP_EXCEPTION_CODE || record.NumberParameters < 4) {
        return;
    }
    __try {
        const char* const object = reinterpret_cast<const char*>(record.ExceptionInformation[1]);
        const auto* const info = reinterpret_cast<const ThrowInfoRaw*>(record.ExceptionInformation[2]);
        const char* const base = reinterpret_cast<const char*>(record.ExceptionInformation[3]);
        if (object == nullptr || info == nullptr || base == nullptr) {
            return;
        }
        const auto* const types =
            reinterpret_cast<const CatchableTypeArrayRaw*>(base + info->catchable_type_array);
        const char* first_name = "?";
        const std::exception* standard = nullptr;
        for (int i = 0; i < types->count && i < 32; ++i) {
            const auto* const type = reinterpret_cast<const CatchableTypeRaw*>(base + types->types[i]);
            const auto* const descriptor = reinterpret_cast<const TypeDescriptorRaw*>(base + type->type);
            if (i == 0) {
                first_name = descriptor->name;
            }
            if (std::strcmp(descriptor->name, ".?AVexception@std@@") == 0) {
                standard = reinterpret_cast<const std::exception*>(object + type->mdisp);
            }
        }
        std::snprintf(out, size, "C++ exception %s: %s\r\n", first_name,
                      standard != nullptr ? standard->what() : "(not a std::exception)");
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        std::snprintf(out, size, "C++ exception (details unreadable)\r\n");
    }
}


void AppendAddress(std::string& out, const char* label, const void* address) {
    HMODULE module{};
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module)) {
        GetModuleFileNameA(module, name, sizeof(name));
    }
    const char* base_name = std::strrchr(name, '\\');
    char line[MAX_PATH + 96];
    std::snprintf(line, sizeof(line), "  %s %p %s+%#llx\r\n", label, address,
                  base_name ? base_name + 1 : name,
                  static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                  reinterpret_cast<uintptr_t>(module)));
    out += line;
}

void WriteText(const std::wstring& path, const std::string& text) {
    const HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return;
    }
    DWORD written{};
    WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    CloseHandle(file);
}

LONG WINAPI OnCrash(EXCEPTION_POINTERS* exception) {
    static std::atomic<bool> done{};
    if (done.exchange(true) || dump_dir.empty()) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CreateDirectoryW(dump_dir.c_str(), nullptr);
    const std::wstring base = dump_dir + L"\\lichii-crash-" + std::to_wstring(GetCurrentProcessId());

    // The text report first: it survives even if the minidump fails.
    const EXCEPTION_RECORD& record = *exception->ExceptionRecord;
    std::string text;
    char head[256];
    std::snprintf(head, sizeof(head), "Exception %#lx on thread %lu, info %#llx %#llx\r\n",
                  record.ExceptionCode, GetCurrentThreadId(),
                  static_cast<unsigned long long>(
                      record.NumberParameters > 0 ? record.ExceptionInformation[0] : 0),
                  static_cast<unsigned long long>(
                      record.NumberParameters > 1 ? record.ExceptionInformation[1] : 0));
    text += head;
    char cpp_exception[512];
    DescribeCppException(record, cpp_exception, sizeof(cpp_exception));
    text += cpp_exception;
    AppendAddress(text, "at", record.ExceptionAddress);
    void* frames[62];
    const USHORT count = RtlCaptureStackBackTrace(0, 62, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        AppendAddress(text, "frame", frames[i]);
    }
    WriteText(base + L".txt", text);

    struct ExceptionInformation {
        DWORD thread_id;
        EXCEPTION_POINTERS* exception_pointers;
        BOOL client_pointers;
    };
    using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, int,
                                              const ExceptionInformation*, const void*,
                                              const void*);
    const HMODULE dbghelp = LoadLibraryA("dbghelp.dll");
    const auto write = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFn>(
                                     GetProcAddress(dbghelp, "MiniDumpWriteDump"))
                               : nullptr;
    if (write != nullptr) {
        const HANDLE file = CreateFileW((base + L".dmp").c_str(), GENERIC_WRITE, 0, nullptr,
                                        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) {
            const ExceptionInformation information{GetCurrentThreadId(), exception, FALSE};
            // MiniDumpWithThreadInfo, as the CLI frontend uses.
            write(GetCurrentProcess(), GetCurrentProcessId(), file, 0x1000, &information, nullptr,
                  nullptr);
            CloseHandle(file);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

} // Anonymous namespace

void InstallCrashReport() {
    dump_dir = (Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir) / "crash_dumps").wstring();
    SetUnhandledExceptionFilter(OnCrash);
}

#else

void InstallCrashReport() {}

#endif
