#include "Queda.hpp"

#include <windows.h>
#include <dbghelp.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <cwchar>

// Everything here runs while the process is dying, maybe with its heap broken (0xC0000374): no allocation, only
// fixed buffers, the stack and plain Win32 calls.

namespace Ranga::Retro {

namespace {

using WriteDump = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                PMINIDUMP_CALLBACK_INFORMATION);

WriteDump g_writeDump = nullptr;
wchar_t g_dumpPath[MAX_PATH * 2] = {};
volatile LONG g_done = 0;
char g_text[8192];

// "pcsx2_libretro.dll+0x1A2B3C", or "codigo-gerado@0x..." for code a core made while running (a recompiler), which
// belongs to no file.
void Place(DWORD64 address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) && module != nullptr) {
        wchar_t path[MAX_PATH] = {};
        GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = path;
        for (const wchar_t* at = path; *at; ++at) {
            if (*at == L'\\' || *at == L'/') name = at + 1;
        }
        char narrow[MAX_PATH] = {};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, narrow, sizeof(narrow), nullptr, nullptr);
        std::snprintf(out, size, "%s+0x%llX", narrow, static_cast<unsigned long long>(address - reinterpret_cast<DWORD64>(module)));
    } else {
        std::snprintf(out, size, "codigo-gerado@0x%llX", static_cast<unsigned long long>(address));
    }
}

bool InModule(DWORD64 address) {
    HMODULE module = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                              reinterpret_cast<LPCWSTR>(address), &module) && module != nullptr;
}

void Append(size_t& used, const char* format, ...) {
    if (used >= sizeof(g_text)) return;
    va_list args;
    va_start(args, format);
    const int wrote = std::vsnprintf(g_text + used, sizeof(g_text) - used, format, args);
    va_end(args);
    if (wrote > 0) used = used + static_cast<size_t>(wrote) < sizeof(g_text) ? used + static_cast<size_t>(wrote) : sizeof(g_text) - 1;
}

// Windows' own files: a crash inside them (a broken heap is found by ntdll) was caused by whoever called them.
bool SystemFile(const char* place) {
    static const char* const kSystem[] = {"ntdll.dll+", "KERNELBASE.dll+", "KERNEL32.DLL+", "kernel32.dll+", "ucrtbase.dll+", "msvcrt.dll+",
                                          "vcruntime140.dll+", "msvcp140.dll+"};
    for (const char* name : kSystem) {
        if (_strnicmp(place, name, std::strlen(name)) == 0) return true;
    }
    return false;
}

void Describe(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD& record = *info->ExceptionRecord;
    // The stack as Windows unwinds it; where a frame has no unwind data (code a core made while running), the rest
    // is the return addresses into real files found on the stack, which is a guess and said so.
    static char frames[32][MAX_PATH + 40];
    int count = 0;
    Place(reinterpret_cast<DWORD64>(record.ExceptionAddress), frames[count++], sizeof(frames[0]));
    CONTEXT context = *info->ContextRecord;
    // The crashing thread's stack (handlers run on the thread that faulted), from its thread block.
    const NT_TIB* tib = reinterpret_cast<const NT_TIB*>(NtCurrentTeb());
    const ULONG_PTR low = reinterpret_cast<ULONG_PTR>(tib->StackLimit), high = reinterpret_cast<ULONG_PTR>(tib->StackBase);
    while (count < 24) {
        DWORD64 base = 0;
        PRUNTIME_FUNCTION entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
        if (entry == nullptr) break;
        void* handlerData = nullptr;
        DWORD64 establisher = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, entry, &context, &handlerData, &establisher, nullptr);
        if (context.Rip == 0 || context.Rsp < low || context.Rsp >= high) break;
        Place(context.Rip, frames[count++], sizeof(frames[0]));
    }
    const int unwound = count;
    if (unwound < 4) {
        const DWORD64 rsp = info->ContextRecord->Rsp;
        if (rsp >= low && rsp < high) {
            for (DWORD64 slot = rsp & ~7ull; slot + 8 <= high && count < 32 && slot < rsp + 64 * 1024; slot += 8) {
                const DWORD64 value = *reinterpret_cast<const DWORD64*>(slot);
                if (value >= 0x10000 && InModule(value)) Place(value, frames[count++], sizeof(frames[0]));
            }
        }
    }
    // Who to blame: the first frame that is not Windows' own, else where it crashed.
    const char* culprit = frames[0];
    for (int i = 0; i < count; ++i) {
        if (!SystemFile(frames[i])) {
            culprit = frames[i];
            break;
        }
    }

    size_t used = 0;
    Append(used, "\n[queda] codigo=0x%08lX em %s", static_cast<unsigned long>(record.ExceptionCode), culprit);
    if (culprit != frames[0]) Append(used, " (caiu dentro de %s)", frames[0]);
    if (record.ExceptionCode == EXCEPTION_ACCESS_VIOLATION && record.NumberParameters >= 2) {
        const ULONG_PTR kind = record.ExceptionInformation[0];
        Append(used, " (%s em 0x%016llX)", kind == 0 ? "leitura" : kind == 1 ? "escrita" : "execucao",
               static_cast<unsigned long long>(record.ExceptionInformation[1]));
    }
    Append(used, "\n[queda] pilha: %s", frames[0]);
    for (int i = 1; i < unwound; ++i) Append(used, " < %s", frames[i]);
    if (count > unwound) {
        Append(used, "\n[queda] provaveis chamadores:");
        for (int i = unwound; i < count; ++i) Append(used, " %s", frames[i]);
    }
    Append(used, "\n");

    HANDLE err = GetStdHandle(STD_ERROR_HANDLE);
    if (err != nullptr && err != INVALID_HANDLE_VALUE) {
        DWORD put = 0;
        WriteFile(err, g_text, static_cast<DWORD>(used), &put, nullptr);
        FlushFileBuffers(err);
    }
}

struct DumpJob {
    EXCEPTION_POINTERS* info;
    DWORD thread;
};

DWORD WINAPI DumpThread(void* parameter) {
    const DumpJob& job = *static_cast<DumpJob*>(parameter);
    HANDLE file = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return 1;
    MINIDUMP_EXCEPTION_INFORMATION exception{job.thread, job.info, FALSE};
    g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file,
                static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &exception, nullptr, nullptr);
    CloseHandle(file);
    return 0;
}

void Capture(EXCEPTION_POINTERS* info) {
    if (InterlockedExchange(&g_done, 1) != 0) return;
    Describe(info);
    if (g_writeDump == nullptr || g_dumpPath[0] == 0) return;
    // Its own thread, and at most 5 s: with the heap broken the dump can get stuck, and then the game must still close.
    DumpJob job{info, GetCurrentThreadId()};
    HANDLE thread = CreateThread(nullptr, 0, DumpThread, &job, 0, nullptr);
    if (thread != nullptr) {
        WaitForSingleObject(thread, 5000);
        CloseHandle(thread);
    }
}

LONG WINAPI LastChance(EXCEPTION_POINTERS* info) {
    Capture(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

// A broken heap is reported as an exception no core handles and that Windows then ends the process for, often
// without reaching the last-chance handler: caught here, on the way, and passed on untouched.
LONG WINAPI Vectored(EXCEPTION_POINTERS* info) {
    if (info->ExceptionRecord->ExceptionCode == static_cast<DWORD>(0xC0000374)) Capture(info);
    return EXCEPTION_CONTINUE_SEARCH;
}

}  // namespace

void InstallCrashCapture(const std::wstring& logsFolder) {
    // Loaded now: at the crash, loading a DLL may not work.
    if (HMODULE help = LoadLibraryW(L"dbghelp.dll"); help != nullptr)
        g_writeDump = reinterpret_cast<WriteDump>(reinterpret_cast<void*>(GetProcAddress(help, "MiniDumpWriteDump")));
    if (!logsFolder.empty() && logsFolder.size() + 12 < MAX_PATH * 2)
        std::swprintf(g_dumpPath, MAX_PATH * 2, L"%ls\\queda.dmp", logsFolder.c_str());
    AddVectoredExceptionHandler(0, Vectored);
    SetUnhandledExceptionFilter(LastChance);
}

void ReinstallCrashCapture() { SetUnhandledExceptionFilter(LastChance); }

}  // namespace Ranga::Retro
