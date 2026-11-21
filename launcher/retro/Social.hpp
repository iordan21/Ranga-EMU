#ifndef RANGA_RETRO_SOCIAL_HPP
#define RANGA_RETRO_SOCIAL_HPP

// Friends and chat inside the game, for the menu (RetroMain.cpp, pages Amigos and Chat): the same central the client
// uses (launcher/net/cloudflare), with the session the client keeps in perfil\perfil.txt beside the program. A thread
// asks for the friends and the new messages every few seconds; a message that arrives while playing shows over the
// game for a few seconds, like an achievement. Without a session (or with engines.ini [Ranga] central=nenhuma) it does
// nothing. It never writes the client's files.

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include <string>
#include <vector>

namespace Social {

struct Friend {
    std::string id, name; // name: "Nome#1234"
    std::string playing, system;
    long long seen = 0;   // last time their Ranga said hello (Unix time)
    bool Online() const;
};

struct Message {
    long long number = 0, when = 0;
    std::string from, to, text;
};

void Start(const std::wstring& programFolder, HWND window);
bool Active();          // signed in
bool Ready();           // the friends came at least once
std::string Me();
std::vector<Friend> Friends(); // online first, then by name
std::vector<Message> Conversation(const std::string& friendId, size_t last);
int Unread(const std::string& friendId); // arrived since the game opened and not seen in the chat page
void MarkSeen(const std::string& friendId);
// Sends in the background; the answer (or why not) comes back as a notice.
void Send(const std::string& to, const std::string& text);
// How a message reads: an invite ("[[convite|...]]") or a picture ("[[imagem|...]]") by what it is.
std::wstring Shown(const std::string& text);
// Something changed since the last call (for the menu to repaint).
bool Changed();
// Every turn of the game's loop: the notices over the game.
void Idle();
void Stop();

}

#endif
