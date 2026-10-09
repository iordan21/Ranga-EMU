#include <cstdint>
#include "prx/common/StderrLog.hpp"
#include "prx/common/SlowLog.hpp"
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>
#include <string>
#include <algorithm>
#include <map>
#include <mutex>
#include <utility>
#include <vector>
#include "SceTypes.hpp"
#include "prx/libkernel/Pthread/include/Pthread.hpp"
#include "prx/libc/include/General.hpp"
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
using GuestHandler = void (APS5_VABI *)(int, void*);
constexpr int MaxSignals = 128;
std::atomic<GuestHandler> handlers[MaxSignals];

constexpr std::size_t XStateBytes = 1024;
struct alignas(64) ExceptionFrame {
    alignas(64) char xstate[XStateBytes];
    CONTEXT context;
    int signum;
    void* mcontext;
    char mcontextStorage[0x400];
};
struct NtAlert { using Fn = LONG (NTAPI *)(HANDLE); };
bool IsGuestAddress(DWORD64 address) {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module) || !module) return false;
    if (module == GetModuleHandleW(nullptr)) return true;
    wchar_t name[MAX_PATH] = {};
    GetModuleFileNameW(module, name, MAX_PATH);
    const std::wstring path(name);
    return path.find(L".guest.prx") != std::wstring::npos;
}
void NudgeThread(Pthread thread) {
    static const auto alertById = reinterpret_cast<LONG (NTAPI *)(ULONG_PTR)>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtAlertThreadByThreadId"));
    if (alertById) alertById(GetThreadId(static_cast<HANDLE>(thread->nativeHandle)));
}

unsigned XMaskLow() {
    unsigned a = 1, b = 0, c = 0, d = 0;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d));
    if ((c & (1u << 27)) == 0) return 0;
    unsigned low = 0, high = 0;
    __asm__ volatile("xgetbv" : "=a"(low), "=d"(high) : "c"(0));
    return low & 7u;
}
}
// Stutter diagnosis (APS5_SLOW_FRAME_MS): while threads stand still in a guest signal handler, which is how the guest's
// garbage collector stops the world, a sampler looks every millisecond at where the thread that stopped them runs. When
// the last stopped thread goes on, it prints where that time went, by module and by 256-byte block of code.
namespace GcProfile {
std::atomic<DWORD> collector{0};
std::atomic<int> stopped{0};
constexpr double ReportFromMs = 20.0;

struct Stopped {
    Stopped() { stopped.fetch_add(1); }
    ~Stopped() { stopped.fetch_sub(1); }
};

std::string ModuleName(HMODULE module) {
    wchar_t name[MAX_PATH] = L"?";
    if (module != nullptr) GetModuleFileNameW(module, name, MAX_PATH);
    const wchar_t* base = wcsrchr(name, L'\\');
    std::string text;
    for (const wchar_t* character = base != nullptr ? base + 1 : name; *character != L'\0'; ++character) text += *character < 128 ? static_cast<char>(*character) : '?';
    return text;
}

void Report(const std::vector<DWORD64>& samples, double startMs, double endMs) {
    std::map<HMODULE, unsigned> modules;
    std::map<std::pair<HMODULE, DWORD64>, unsigned> blocks;
    for (const auto rip : samples) {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(rip), &module);
        const DWORD64 offset = module != nullptr ? rip - reinterpret_cast<DWORD64>(module) : rip;
        ++modules[module];
        ++blocks[{module, offset & ~static_cast<DWORD64>(0xff)}];
    }
    std::vector<std::pair<unsigned, HMODULE>> byModule;
    for (const auto& [module, count] : modules) byModule.emplace_back(count, module);
    std::sort(byModule.rbegin(), byModule.rend());
    std::vector<std::pair<unsigned, std::pair<HMODULE, DWORD64>>> byBlock;
    for (const auto& [block, count] : blocks) byBlock.emplace_back(count, block);
    std::sort(byBlock.rbegin(), byBlock.rend());
    std::string text = "modules:";
    for (std::size_t index = 0; index < byModule.size() && index < 6; ++index) text += " " + ModuleName(byModule[index].second) + "=" + std::to_string(byModule[index].first);
    text += " hot:";
    char block[96];
    for (std::size_t index = 0; index < byBlock.size() && index < 10; ++index) {
        std::snprintf(block, sizeof(block), " %s+0x%llx=%u", ModuleName(byBlock[index].second.first).c_str(), static_cast<unsigned long long>(byBlock[index].second.second), byBlock[index].first);
        text += block;
    }
    aps5::LogErr("[slow] t_ms=%.3f gc-profile %.3f ms tid=%lu samples=%zu %s\n", endMs, endMs - startMs, collector.load(), samples.size(), text.c_str());
}

void Sample() {
    std::vector<DWORD64> samples;
    samples.reserve(8192);
    HANDLE target = nullptr;
    DWORD targetId = 0;
    double startMs = 0;
    for (;;) {
        Sleep(1);
        const DWORD id = collector.load();
        if (stopped.load() > 0 && id != 0) {
            if (samples.empty()) startMs = aps5::SteadyMs();
            if (id != targetId) {
                if (target != nullptr) CloseHandle(target);
                target = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, id);
                targetId = id;
            }
            if (target == nullptr || samples.size() == samples.capacity()) continue;
            // Nothing allocates while the thread is suspended, as it may hold the heap lock.
            alignas(16) CONTEXT context{};
            context.ContextFlags = CONTEXT_CONTROL;
            if (SuspendThread(target) == static_cast<DWORD>(-1)) continue;
            const bool read = GetThreadContext(target, &context) != 0;
            ResumeThread(target);
            if (read) samples.push_back(context.Rip);
        } else if (!samples.empty()) {
            const double endMs = aps5::SteadyMs();
            if (endMs - startMs >= ReportFromMs) Report(samples, startMs, endMs);
            samples.clear();
        }
    }
}

void Start() {
    static std::once_flag once;
    std::call_once(once, [] { std::thread(Sample).detach(); });
}
}

extern "C" unsigned DeliverXMaskLow;
extern "C" unsigned DeliverXMaskHigh;
unsigned DeliverXMaskLow = XMaskLow();
unsigned DeliverXMaskHigh = 0;
namespace {
void DeliverExceptionImpl(ExceptionFrame* frame) {
    auto handler = handlers[frame->signum].load();
    if (handler) {
        GcProfile::Stopped standing;
        handler(frame->signum, frame->mcontext);
    }
    if (DeliverXMaskLow != 0) {
        const unsigned low = DeliverXMaskLow, high = DeliverXMaskHigh;
        __asm__ volatile("xrstor64 (%0)" : : "r"(frame->xstate), "a"(low), "d"(high) : "memory");
    }
    RtlRestoreContext(&frame->context, nullptr);
}

}

extern "C" void DeliverExceptionThunk();
extern "C" void* DeliverExceptionTarget;
void* DeliverExceptionTarget = reinterpret_cast<void*>(&DeliverExceptionImpl);
__asm__(".text\n"
        ".globl DeliverExceptionThunk\n"
        "DeliverExceptionThunk:\n"
        "movl DeliverXMaskLow(%rip), %eax\n"
        "testl %eax, %eax\n"
        "jz 1f\n"
        "movl DeliverXMaskHigh(%rip), %edx\n"
        "xsave64 (%rbx)\n"
        "1:\n"
        "movq %rbx, %rcx\n"
        "jmp *DeliverExceptionTarget(%rip)\n");

static std::atomic<unsigned> gcedHanded, gcedInjected, gcedSkipped, gcedRun;

static unsigned gcWaitMillis() {
    static const unsigned value = [] { const char* text = std::getenv("APS5_GC_WAIT_MS"); return text ? static_cast<unsigned>(std::strtoul(text, nullptr, 10)) : 1000u; }();
    return value;
}

void RunExceptionHandlerInline(int signum) {
    const auto handler = handlers[signum].load();
    if (!handler) return;
    aps5::LogErr("[gced] ran signum=%d run=%u\n", signum, gcedRun.fetch_add(1) + 1);
    alignas(16) char mcontext[0x400] = {};
    *reinterpret_cast<void**>(mcontext + 0xf8) = mcontext;
    const auto started = std::chrono::steady_clock::now();
    {
        GcProfile::Stopped standing;
        handler(signum, mcontext);
    }
    // The guest handler returns when the collector lets the thread go, so this is how long the thread stood still.
    if (aps5::SlowLogEnabled()) {
        const double stopped = aps5::ElapsedMs(started);
        if (stopped >= aps5::SlowStallMs) aps5::LogErr("[slow] t_ms=%.3f gc-stop signum=%d tid=%lu %.3f ms\n", aps5::SteadyMs(), signum, GetCurrentThreadId(), stopped);
    }
}

extern "C" {

int APS5_VABI sceKernelInstallExceptionHandler(int signum, void* handler) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    aps5::LogErr( "[EXC] install handler signum=%d handler=%p\n", signum, handler);
    handlers[signum].store(reinterpret_cast<GuestHandler>(handler));
    return 0;
}

int APS5_VABI sceKernelRemoveExceptionHandler(int signum) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    handlers[signum].store(nullptr);
    return 0;
}

int APS5_VABI sceKernelRaiseException(Pthread thread, int signum) {
    if (signum < 0 || signum >= MaxSignals) return static_cast<int>(0x80020016);
    const auto handler = handlers[signum].load();
    if (!thread || thread->threadId == std::this_thread::get_id()) {
        if (handler) { alignas(16) char mcontext[0x400] = {}; *reinterpret_cast<void**>(mcontext + 0xf8) = mcontext; handler(signum, mcontext); }
        return 0;
    }
    if (!handler) return 0;
    if (aps5::SlowLogEnabled()) {
        GcProfile::collector.store(GetCurrentThreadId());
        GcProfile::Start();
    }
    HANDLE native = static_cast<HANDLE>(thread->nativeHandle);

    CONTEXT context{};
    bool interrupted = false;
    bool handed = false;

    const auto raised = std::chrono::steady_clock::now();
    const auto giveUpAt = raised + std::chrono::milliseconds(gcWaitMillis());
    int attempts = 0;
    // How long the raising thread waited for the target to stop somewhere a signal can be delivered.
    struct ReportDelivery {
        std::chrono::steady_clock::time_point raised;
        int& attempts;
        HANDLE native;
        int signum;
        ~ReportDelivery() {
            if (!aps5::SlowLogEnabled()) return;
            const double waited = aps5::ElapsedMs(raised);
            if (waited >= aps5::SlowStallMs) aps5::LogErr("[slow] t_ms=%.3f gc-raise signum=%d tid=%lu %.3f ms attempts=%d\n", aps5::SteadyMs(), signum, GetThreadId(native), waited, attempts);
        }
    } report{raised, attempts, native, signum};
    for (int attempt = 0;; ++attempt, attempts = attempt) {
        if (attempt >= 4096 && std::chrono::steady_clock::now() >= giveUpAt) break;
        if (SuspendThread(native) == static_cast<DWORD>(-1)) return static_cast<int>(0x80020003);
        context = {};
        context.ContextFlags = CONTEXT_ALL;
        const bool readable = GetThreadContext(native, &context) != 0;

        thread->pendingException.store(signum);
        if (thread->inWait.load()) {
            handed = true;
            break;
        }
        thread->pendingException.store(0);
        if (readable && IsGuestAddress(context.Rip)) {
            interrupted = true;
            break;
        }
        ResumeThread(native);
        if (!readable) return static_cast<int>(0x80020003);
        if (attempt < 64) SwitchToThread(); else if (attempt < 4096) Sleep(0); else Sleep(1);
    }

    if (handed) {
        ResumeThread(native);
        if (thread->wakeEvent) SetEvent(static_cast<HANDLE>(thread->wakeEvent));
        aps5::LogErr("[gced] hand tid=%u h=%u inj=%u skip=%u\n", GetThreadId(native),
                     gcedHanded.fetch_add(1) + 1, gcedInjected.load(), gcedSkipped.load());
        return 0;
    }
    if (!interrupted) {

        {
            char where[400] = {};
            int used = 0;
            DWORD64 candidates[8] = {context.Rip};
            int found = 1;
            const auto* stack = reinterpret_cast<const DWORD64*>(context.Rsp);
            for (int slot = 0; slot < 128 && found < 8; ++slot) {
                DWORD64 value = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), stack + slot, &value, sizeof(value), nullptr)) break;
                HMODULE owner = nullptr;
                if (value > 0x10000 && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(value), &owner) && owner != nullptr) candidates[found++] = value;
            }
            for (int index = 0; index < found && used < static_cast<int>(sizeof(where)) - 80; ++index) {
                HMODULE owner = nullptr;
                wchar_t name[MAX_PATH] = {};
                if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(candidates[index]), &owner) && owner != nullptr) GetModuleFileNameW(owner, name, MAX_PATH);
                const wchar_t* base = wcsrchr(name, L'\\');
                used += std::snprintf(where + used, sizeof(where) - used, " %ls+0x%llx", base ? base + 1 : name, static_cast<unsigned long long>(candidates[index] - reinterpret_cast<DWORD64>(owner)));
            }
            aps5::LogErr("[gced] skipped tid=%u at%s\n", GetThreadId(native), where);
        }
        aps5::LogErr("[gced] skip tid=%u h=%u inj=%u skip=%u\n", GetThreadId(native),
                     gcedHanded.load(), gcedInjected.load(), gcedSkipped.fetch_add(1) + 1);
        return static_cast<int>(0x80020003);
    }

    auto top = (context.Rsp - 0x200 - sizeof(ExceptionFrame)) & ~static_cast<DWORD64>(0x3f);
    auto* frame = reinterpret_cast<ExceptionFrame*>(top);
    std::memset(&frame->context, 0, sizeof(*frame) - offsetof(ExceptionFrame, context));
    std::memset(frame->xstate, 0, 576);
    frame->context = context;
    frame->signum = signum;
    frame->mcontext = frame->mcontextStorage;

    *reinterpret_cast<void**>(frame->mcontextStorage + 0xf8) = frame;
    CONTEXT redirected = context;
    redirected.Rsp = top - 0x28;
    redirected.Rip = reinterpret_cast<DWORD64>(&DeliverExceptionThunk);
    redirected.Rbx = reinterpret_cast<DWORD64>(frame);
    const bool ok = SetThreadContext(native, &redirected) != 0;
    ResumeThread(native);

    if (ok) NudgeThread(thread);
    const unsigned injections = gcedInjected.fetch_add(1) + 1;
    if ((injections & 63) == 1)
        aps5::LogErr("[gced] inj n=%u h=%u skip=%u\n", injections, gcedHanded.load(), gcedSkipped.load());
    return ok ? 0 : static_cast<int>(0x80020003);
}

void APS5_VABI sceKernelDebugRaiseException(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseException c1=%d c2=%d", c1, c2);
}

void APS5_VABI sceKernelDebugRaiseExceptionOnReleaseMode(int c1, int c2) {
  APS5_LOG_OUT("sceKernelDebugRaiseExceptionOnReleaseMode c1=%d c2=%d", c1, c2);
}

}
