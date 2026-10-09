// Sampling profiler for finding where the emulator spends a frame, off unless APS5_SAMPLE_MS=<interval> is set.
// Every interval it suspends each thread of the process in turn, unwinds a few frames of its stack and lets it go;
// nothing is allocated while a thread is suspended, since it may hold the heap lock. Every 15 s the counts go to
// sampler.txt in the working directory: per thread, the innermost frames and every frame seen on its stacks, as
// module+offset, for symbolizing with the modules' symbol tables afterwards.
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <tlhelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace {

constexpr int Depth = 16;

struct Stack {
    DWORD tid;
    int count;
    std::array<DWORD64, Depth> frames;
};

struct ThreadCounts {
    std::uint64_t samples = 0;
    std::uint64_t idle = 0;
    std::unordered_map<DWORD64, std::uint64_t> leaf;
    std::unordered_map<DWORD64, std::uint64_t> inclusive;
    std::map<std::vector<DWORD64>, std::uint64_t> stacks;
    std::map<std::vector<DWORD64>, std::uint64_t> idleStacks;
};

// System calls that block: a thread whose innermost frame sits in one of them is waiting, not working.
std::vector<DWORD64> blockingCalls() {
    std::vector<DWORD64> calls;
    for (const auto& [module, names] : {std::pair<const char*, std::vector<const char*>>{"ntdll.dll", {"NtWaitForSingleObject", "NtWaitForMultipleObjects", "NtDelayExecution", "NtRemoveIoCompletion", "NtRemoveIoCompletionEx", "NtWaitForAlertByThreadId", "NtWaitForWorkViaWorkerFactory", "NtWaitForKeyedEvent", "NtSignalAndWaitForSingleObject", "NtReadFile", "NtDeviceIoControlFile"}},
                                        std::pair<const char*, std::vector<const char*>>{"win32u.dll", {"NtUserGetMessage", "NtUserMsgWaitForMultipleObjectsEx", "NtUserWaitMessage"}}}) {
        HMODULE handle = GetModuleHandleA(module);
        if (handle == nullptr) handle = LoadLibraryA(module);
        if (handle == nullptr) continue;
        for (const char* name : names)
            if (const auto address = reinterpret_cast<DWORD64>(GetProcAddress(handle, name))) calls.push_back(address);
    }
    return calls;
}

int unwind(HANDLE thread, std::array<DWORD64, Depth>& frames) {
    CONTEXT context{};
    context.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (!GetThreadContext(thread, &context)) return 0;
    int count = 0;
    for (; count < Depth && context.Rip != 0; ++count) {
        frames[count] = context.Rip;
        DWORD64 base = 0;
        const auto* entry = RtlLookupFunctionEntry(context.Rip, &base, nullptr);
        if (entry == nullptr) {
            // A leaf without unwind data, or guest code: take the return address at the stack pointer once.
            if (count != 0) { ++count; break; }
            MEMORY_BASIC_INFORMATION info{};
            if (VirtualQuery(reinterpret_cast<void*>(context.Rsp), &info, sizeof(info)) == 0 || info.State != MEM_COMMIT) { ++count; break; }
            context.Rip = *reinterpret_cast<DWORD64*>(context.Rsp);
            context.Rsp += 8;
            continue;
        }
        void* handler = nullptr;
        DWORD64 frame = 0;
        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, context.Rip, const_cast<PRUNTIME_FUNCTION>(entry), &context, &handler, &frame, nullptr);
    }
    return count;
}

std::string moduleOffset(DWORD64 address, std::map<DWORD64, std::pair<DWORD64, std::string>>& modules) {
    auto found = modules.upper_bound(address);
    if (found != modules.begin()) {
        --found;
        if (address < found->first + found->second.first) {
            char text[64];
            std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(address - found->first));
            return found->second.second + text;
        }
    }
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(address), &module)) {
        char path[MAX_PATH] = {};
        GetModuleFileNameA(module, path, MAX_PATH);
        std::string name = path;
        name = name.substr(name.find_last_of("\\/") + 1);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(module);
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const char*>(module) + dos->e_lfanew);
        const auto base = reinterpret_cast<DWORD64>(module);
        modules[base] = {nt->OptionalHeader.SizeOfImage, name};
        char text[64];
        std::snprintf(text, sizeof(text), "+0x%llx", static_cast<unsigned long long>(address - base));
        return name + text;
    }
    char text[32];
    std::snprintf(text, sizeof(text), "?0x%llx", static_cast<unsigned long long>(address));
    return text;
}

std::string threadName(DWORD tid) {
    std::string name;
    if (HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, tid)) {
        PWSTR description = nullptr;
        if (SUCCEEDED(GetThreadDescription(thread, &description)) && description != nullptr) {
            for (const wchar_t* c = description; *c != 0; ++c) name += static_cast<char>(*c < 128 ? *c : '?');
            LocalFree(description);
        }
        CloseHandle(thread);
    }
    return name;
}

void dump(const std::unordered_map<DWORD, ThreadCounts>& counts, std::uint64_t rounds, double seconds) {
    std::map<DWORD64, std::pair<DWORD64, std::string>> modules;
    std::vector<std::pair<std::uint64_t, DWORD>> order;
    for (const auto& [tid, data] : counts) order.emplace_back(data.samples, tid);
    std::sort(order.rbegin(), order.rend());
    std::FILE* file = std::fopen("sampler.txt", "w");
    if (file == nullptr) return;
    std::fprintf(file, "rounds %llu in %.1f s\n", static_cast<unsigned long long>(rounds), seconds);
    for (const auto& [samples, tid] : order) {
        const auto& data = counts.at(tid);
        std::fprintf(file, "\n== thread 0x%lx \"%s\" samples %llu busy %llu (%.0f%%)\n", tid, threadName(tid).c_str(), static_cast<unsigned long long>(samples), static_cast<unsigned long long>(samples - data.idle), samples != 0 ? 100.0 * static_cast<double>(samples - data.idle) / static_cast<double>(samples) : 0.0);
        for (const auto* table : {&data.leaf, &data.inclusive}) {
            std::vector<std::pair<std::uint64_t, DWORD64>> top;
            for (const auto& [address, count] : *table) top.emplace_back(count, address);
            std::sort(top.rbegin(), top.rend());
            std::fprintf(file, table == &data.leaf ? "  leaf:\n" : "  inclusive:\n");
            for (std::size_t i = 0; i < top.size() && i < (table == &data.leaf ? 25u : 40u); ++i)
                std::fprintf(file, "    %6llu %s\n", static_cast<unsigned long long>(top[i].first), moduleOffset(top[i].second, modules).c_str());
        }
        for (const auto* table : {&data.stacks, &data.idleStacks}) {
            std::vector<std::pair<std::uint64_t, const std::vector<DWORD64>*>> stacksByCount;
            for (const auto& [frames, count] : *table) stacksByCount.emplace_back(count, &frames);
            std::sort(stacksByCount.begin(), stacksByCount.end(), [](const auto& left, const auto& right) { return left.first > right.first; });
            std::fprintf(file, table == &data.stacks ? "  stacks:\n" : "  idle stacks:\n");
            for (std::size_t i = 0; i < stacksByCount.size() && i < (table == &data.stacks ? 30u : 15u); ++i) {
                std::fprintf(file, "    %6llu", static_cast<unsigned long long>(stacksByCount[i].first));
                for (const auto address : *stacksByCount[i].second) std::fprintf(file, " %s", moduleOffset(address, modules).c_str());
                std::fprintf(file, "\n");
            }
        }
    }
    std::fclose(file);
}

void run(int intervalMs) {
    const auto blocking = blockingCalls();
    const DWORD self = GetCurrentThreadId();
    const DWORD process = GetCurrentProcessId();
    std::vector<DWORD> threads;
    std::vector<Stack> stacks;
    std::unordered_map<DWORD, ThreadCounts> counts;
    std::uint64_t rounds = 0;
    const auto start = std::chrono::steady_clock::now();
    auto listed = start - std::chrono::seconds(10);
    auto dumped = start;
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now - listed > std::chrono::seconds(1)) {
            listed = now;
            threads.clear();
            HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
            if (snapshot != INVALID_HANDLE_VALUE) {
                THREADENTRY32 entry{sizeof(entry)};
                for (BOOL more = Thread32First(snapshot, &entry); more; more = Thread32Next(snapshot, &entry))
                    if (entry.th32OwnerProcessID == process && entry.th32ThreadID != self) threads.push_back(entry.th32ThreadID);
                CloseHandle(snapshot);
            }
            stacks.resize(threads.size());
        }
        std::size_t taken = 0;
        for (const DWORD tid : threads) {
            HANDLE thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
            if (thread == nullptr) continue;
            if (SuspendThread(thread) != static_cast<DWORD>(-1)) {
                auto& stack = stacks[taken];
                stack.tid = tid;
                stack.count = unwind(thread, stack.frames);
                ResumeThread(thread);
                if (stack.count != 0) ++taken;
            }
            CloseHandle(thread);
        }
        for (std::size_t i = 0; i < taken; ++i) {
            auto& data = counts[stacks[i].tid];
            ++data.samples;
            const auto leaf = stacks[i].frames[0];
            if (std::any_of(blocking.begin(), blocking.end(), [leaf](DWORD64 call) { return leaf >= call && leaf < call + 0x20; })) {
                ++data.idle;
                ++data.idleStacks[std::vector<DWORD64>(stacks[i].frames.begin(), stacks[i].frames.begin() + stacks[i].count)];
                continue;
            }
            ++data.leaf[stacks[i].frames[0]];
            ++data.stacks[std::vector<DWORD64>(stacks[i].frames.begin(), stacks[i].frames.begin() + stacks[i].count)];
            for (int f = 0; f < stacks[i].count; ++f) {
                bool seen = false;
                for (int g = 0; g < f; ++g) seen = seen || stacks[i].frames[g] == stacks[i].frames[f];
                if (!seen) ++data.inclusive[stacks[i].frames[f]];
            }
        }
        ++rounds;
        if (now - dumped > std::chrono::seconds(15)) {
            dumped = now;
            dump(counts, rounds, std::chrono::duration<double>(now - start).count());
        }
        Sleep(static_cast<DWORD>(intervalMs));
    }
}

struct Starter {
    Starter() {
        const char* text = std::getenv("APS5_SAMPLE_MS");
        if (text == nullptr) return;
        const int interval = std::max(1, std::atoi(text));
        std::thread([interval] {
            // Give the game time to reach its frames before counting.
            const char* delay = std::getenv("APS5_SAMPLE_DELAY_S");
            Sleep(static_cast<DWORD>((delay != nullptr ? std::atoi(delay) : 0) * 1000));
            run(interval);
        }).detach();
    }
} starter;

}
#endif
