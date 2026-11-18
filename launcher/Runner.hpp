#ifndef LAUNCHER_RUNNER_HPP
#define LAUNCHER_RUNNER_HPP

#include "Library.hpp"

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace Ranga {

// Prepares a game in its workspace and runs it, on a thread of its own so the client window keeps drawing.
class Runner {
public:
    enum class State { Idle, Preparing, Running, Finished, Failed };

    ~Runner();
    bool Busy() const { const auto now = state.load(); return now == State::Preparing || now == State::Running; }
    State Current() const { return state.load(); }
    std::wstring Message() const;
    std::wstring TitleId() const;
    // root is the folder of this program, with libs and tools.
    void Start(const std::wstring& root, const Game& game, const Options& options);
    // The console's own system with no game (the PS2's menu, the Xbox dashboard on its hard disk, the Switch's Mii
    // editor), through its native core; its saves and logs go to games\<console>-SISTEMA.
    void StartSystem(const std::wstring& root, const std::wstring& system);

    // Told on the runner's thread when a game closes: the game, when it started (Unix time) and for how many seconds.
    std::function<void(const Game&, long long, long long)> ended;

private:
    void Run(std::wstring root, Game game, Options options);
    // Bloodborne goes through the native port's run.bat instead of the PS5 runtime.
    void RunBloodborne(const std::wstring& root, const Game& game, const Options& options);
    // Any other console goes to the emulator chosen for it on the Emulators page.
    void RunExternal(const std::wstring& root, const Game& game, const Options& options);
    // Waits for the started game, journals how it ended and reports it.
    void Await(PROCESS_INFORMATION& process, const std::wstring& logs, const Game& game, const wchar_t* errorLog);
    void Report(State next, const std::wstring& text);

    std::atomic<State> state{State::Idle};
    mutable std::mutex lock;
    std::wstring message;
    std::wstring titleId;
    std::thread worker;
};

}

#endif
