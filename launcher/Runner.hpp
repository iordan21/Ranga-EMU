#ifndef LAUNCHER_RUNNER_HPP
#define LAUNCHER_RUNNER_HPP

#include "Library.hpp"

#include <atomic>
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

private:
    void Run(std::wstring root, Game game, Options options);
    void Report(State next, const std::wstring& text);

    std::atomic<State> state{State::Idle};
    mutable std::mutex lock;
    std::wstring message;
    std::wstring titleId;
    std::thread worker;
};

}

#endif
