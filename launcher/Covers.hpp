#ifndef LAUNCHER_COVERS_HPP
#define LAUNCHER_COVERS_HPP

#include "Library.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace Ranga {

// Box art for games that carry none of their own, from the libretro thumbnail server (thumbnails.libretro.com, the
// RetroArch project's public set). The game's name is matched against its console's list of Named_Boxarts, kept in
// covers\<console>.txt beside this program and fetched again after 30 days. The art goes to the game's workspace as
// art\boxart.png; art\boxart.miss marks a game the list has no match for, so it is not looked up again. A network
// failure marks nothing: the game is tried again the next time the client starts.
class CoverFetcher {
public:
    explicit CoverFetcher(std::wstring root);
    ~CoverFetcher();
    CoverFetcher(const CoverFetcher&) = delete;
    CoverFetcher& operator=(const CoverFetcher&) = delete;

    // Queues the game once, when its console has box art on the server and the game has none yet.
    void Request(const Game& game);
    // The games whose box art arrived since the last call.
    std::vector<std::wstring> TakeDone();

private:
    struct Job {
        std::wstring titleId, system, workspace, folder, title;
    };
    void Work();
    void Fetch(const Job& job);
    const std::vector<std::wstring>* List(const std::wstring& folder);

    std::wstring root;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Job> jobs;
    std::set<std::wstring> asked;
    std::vector<std::wstring> done;
    bool stop = false;
    // Only the worker touches the lists.
    std::vector<std::pair<std::wstring, std::vector<std::wstring>>> lists;
    std::set<std::wstring> failedLists;
    std::thread worker;
};

std::wstring BoxartFile(const Game& game);
// The thumbnail server's folder for a game of this console and file type; empty when it has none.
std::wstring CoverFolder(const std::wstring& system, const std::wstring& extension);
// The name with its (tags) and [tags] dropped, an article moved from ", The" put back, only letters and digits kept,
// lower-case: "Legend of Zelda, The - A Link to the Past (USA)" and "The Legend of Zelda: A Link to the Past" match.
std::wstring CoverKey(const std::wstring& name);
// The list entry that is the same game as name, preferring the region name itself has, then Brazil, USA, World,
// Europe, then the rest, and never a beta, prototype or demo when a release is there; empty when none is.
std::wstring BestCover(const std::vector<std::wstring>& list, const std::wstring& name);

}

#endif
