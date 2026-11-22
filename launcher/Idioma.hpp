// The languages of Ranga's own screens: Portuguese (the code's), English, Russian and Spanish.
//
// The Portuguese text is the key. The translations live in idiomas.txt (embedded as the IDIOMAS resource): for each
// text four lines, Portuguese, English, Russian and Spanish, C-escaped as in the code, and a blank line between texts.
// Client.cpp's ImGui calls go through ImGuiTr.hpp, which translates each label, text and format string on the way;
// text that is built in the code from pieces ("Instalado  ·  versão " + v) is translated by its pieces.
//
// The choice is engines.ini [Ranga] idioma (pt, en, ru, es); without one, Windows' language (outside those four,
// English). The Apps store passes it on to the apps (--idioma=).
#pragma once

#include <string>

namespace Idioma {

enum Lang { Pt = 0, En, Ru, Es, Count };

// Reads the choice from engines.ini and the table from the resource. Once, before the first frame.
void Init(const std::wstring& root);
Lang Current();
void Set(const std::wstring& root, Lang lang); // saves it in engines.ini
const char* Code(Lang lang);                    // "pt", "en", "ru", "es"
const char* Name(Lang lang);                    // "Português", "English", "Русский", "Español"

// The text in the current language (the same pointer when there is nothing to change). The result stays valid until
// the next NewFrame.
const char* Tr(const char* pt);
std::string Tr(const std::string& pt);
std::wstring TrW(const std::wstring& pt);
// A label that is also an ImGui id ("Abrir", "Abrir##3"): the text translated, the id kept from the Portuguese one
// ("Open###Abrir##3"), so a language never shares an id between two buttons nor loses one.
const char* Label(const char* label);
// printf with the format translated first ("há %lld h" -> "%lld h ago"): for text with a number inside.
std::string Format(const char* ptFormat, ...);
// Each frame: drops what was translated for text that changes all the time (counters, progress).
void NewFrame();

}
