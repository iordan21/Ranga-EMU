#include "Systems.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <cstring>
#include <cwchar>
#include <initializer_list>

namespace Ranga {

namespace {

// Player 1 on the first SDL gamepad, in the binding names PCSX2 and DuckStation share (SDLInputSource): any gamepad SDL
// knows, DualSense, Xbox or generic, maps the same way, so the binding never names a device.
std::vector<Tweak> SdlPad(const std::wstring& file) {
    const char* binds[][2] = {
        {"Up", "DPadUp"}, {"Down", "DPadDown"}, {"Left", "DPadLeft"}, {"Right", "DPadRight"},
        {"Cross", "FaceSouth"}, {"Circle", "FaceEast"}, {"Square", "FaceWest"}, {"Triangle", "FaceNorth"},
        {"Select", "Back"}, {"Start", "Start"}, {"Analog", "Guide"},
        {"L1", "LeftShoulder"}, {"R1", "RightShoulder"}, {"L2", "+LeftTrigger"}, {"R2", "+RightTrigger"},
        {"L3", "LeftStick"}, {"R3", "RightStick"},
        {"LUp", "-LeftY"}, {"LDown", "+LeftY"}, {"LLeft", "-LeftX"}, {"LRight", "+LeftX"},
        {"RUp", "-RightY"}, {"RDown", "+RightY"}, {"RLeft", "-RightX"}, {"RRight", "+RightX"},
        {"LargeMotor", "LargeMotor"}, {"SmallMotor", "SmallMotor"},
    };
    std::vector<Tweak> tweaks = {{file, "InputSources", "SDL", "true"}};
    for (const auto& [key, input] : binds) tweaks.push_back({file, "Pad1", key, std::string("SDL-0/") + input});
    return tweaks;
}

std::vector<Tweak> Join(std::vector<Tweak> first, const std::vector<Tweak>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// RetroArch with one of its cores, which the user installs inside RetroArch (Online Updater, Core Downloader). Vulkan,
// and SDL for gamepads, whose autoconfig profiles cover DualSense, Xbox and generic ones; XInput, its default on
// Windows, sees only Xbox gamepads.
Preset RetroArch(const std::wstring& core, const std::wstring& bios = L"") {
    return {L"RetroArch (" + core + L")", L"retroarch", L"{fullscreen} -L \"{exedir}\\cores\\" + core + L"_libretro.dll\" \"{rom}\"", L"-f",
            {{L"retroarch.cfg", "", "video_driver", "\"vulkan\""},
             {L"retroarch.cfg", "", "input_joypad_driver", "\"sdl2\""},
             {L"retroarch.cfg", "", "system_directory", "\"{root}\\bios\\retroarch\""}},
            bios};
}

// Dolphin's GameCube and Wii pads name the device in their bindings, so they stay for the user to map once in Dolphin.
const Preset kDolphin = {L"Dolphin", L"dolphin", L"-b {fullscreen} -e \"{rom}\"", L"-C Dolphin.Display.Fullscreen=True",
                         {{L"User\\Config\\Dolphin.ini", "Core", "GFXBackend", "Vulkan"},
                          {L"User\\Config\\Dolphin.ini", "Interface", "ConfirmStop", "False"},
                          {L"User\\Config\\GFX.ini", "Settings", "InternalResolution", "3"}}};
const Preset kMgba = {L"mGBA", L"mgba", L"{fullscreen} \"{rom}\"", L"-f"};
const Preset kMesen = {L"Mesen", L"mesen", L"{fullscreen} \"{rom}\"", L"--fullscreen"};

std::vector<System> MakeSystems() {
    std::vector<System> systems = {
        // No PS4 here: of the PS4, Ranga runs only Bloodborne, as an experiment, through its port (Library.cpp).
        {L"PS3", "PlayStation 3", {L".iso"}, true,
         {{L"RPCS3", L"rpcs3", L"--no-gui {fullscreen} \"{rom}\"", L"--fullscreen",
           {{L"GuiConfigs\\CurrentSettings.ini", "main_window", "infoBoxEnabledWelcome", "false"}}, L"", true}},
         "Instale no RPCS3 o firmware do PS3 (PS3UPDAT.PUP, do site da Sony). Jogo em pasta (com PS3_GAME ou USRDIR) ou ISO. "
         "No nativo (core do RPCS3 feito para o Ranga) o firmware e os saves são os do RPCS3 portátil, só com o controle 1 e "
         "sem save state; a primeira abertura compila os shaders e demora."},
        {L"PS2", "PlayStation 2", {L".iso", L".chd", L".cso", L".zso", L".bin", L".cue", L".gz", L".mdf", L".img", L".m3u", L".elf"}, false,
         {{L"PCSX2", L"pcsx2", L"-batch {fullscreen} -- \"{rom}\"", L"-fullscreen",
           Join({{L"inis\\PCSX2.ini", "UI", "SetupWizardIncomplete", "false"},
                 {L"inis\\PCSX2.ini", "UI", "ConfirmShutdown", "false"},
                 {L"inis\\PCSX2.ini", "EmuCore/GS", "Renderer", "14"},
                 {L"inis\\PCSX2.ini", "EmuCore/GS", "upscale_multiplier", "3"},
                 {L"inis\\PCSX2.ini", "Folders", "Bios", "{root}\\bios\\ps2"}},
                SdlPad(L"inis\\PCSX2.ini")),
           L"ps2"},
          RetroArch(L"pcsx2", L"retroarch")},
         "Precisa da BIOS tirada do seu PS2 (arquivo de 4 MB): no nativo em bios\\retroarch\\pcsx2\\bios, no PCSX2 em bios\\ps2."},
        {L"PS1", "PlayStation", {L".cue", L".chd", L".pbp", L".bin", L".iso", L".img", L".ccd", L".mds", L".ecm", L".m3u"}, false,
         {{L"DuckStation", L"duckstation", L"-batch {fullscreen} -- \"{rom}\"", L"-fullscreen",
           Join({{L"settings.ini", "Main", "SetupWizardIncomplete", "false"},
                 {L"settings.ini", "Main", "ConfirmPowerOff", "false"},
                 {L"settings.ini", "GPU", "Renderer", "Vulkan"},
                 {L"settings.ini", "GPU", "ResolutionScale", "4"},
                 {L"settings.ini", "BIOS", "SearchDirectory", "{root}\\bios\\ps1"}},
                SdlPad(L"settings.ini")),
           L"ps1"},
          RetroArch(L"swanstation", L"retroarch"), RetroArch(L"mednafen_psx_hw", L"retroarch")},
         "No nativo não precisa de BIOS: com a da Sony (ps1_rom.bin, que vem no firmware do PS3, em bios\\retroarch) roda "
         "mais jogos; sem ela o Ranga usa a BIOS própria dele, que não roda Azure Dreams nem Wipeout (EUA). O DuckStation "
         "precisa da BIOS tirada do seu console em bios\\ps1."},
        {L"PSP", "PSP", {L".iso", L".cso", L".chd", L".pbp", L".elf", L".prx"}, false,
         {{L"PPSSPP", L"ppsspp", L"{fullscreen} \"{rom}\"", L"--fullscreen",
           {{L"memstick\\PSP\\SYSTEM\\ppsspp.ini", "Graphics", "GraphicsBackend", "3 (VULKAN)"},
            {L"memstick\\PSP\\SYSTEM\\ppsspp.ini", "Graphics", "InternalResolution", "4"}}},
          RetroArch(L"ppsspp")},
         "Não precisa de BIOS."},
        {L"SWITCH", "Nintendo Switch", {L".nsp", L".xci", L".nca", L".nro", L".nso"}, false,
         {{L"Ryujinx", L"ryujinx", L"{fullscreen} \"{rom}\"", L"--fullscreen", {}, L"", true},
          {L"Eden / Citron (família yuzu)", L"eden;citron;sudachi;suyu;yuzu;torzu", L"{fullscreen} -g \"{rom}\"", L"-f", {}, L"", true}},
         "Precisa das prod.keys e do firmware tirados do seu Switch. No nativo (core do Eden feito para o Ranga) ficam em "
         "bios\\retroarch\\eden (keys e nand), sem save state."},
        {L"WIIU", "Wii U", {L".wua", L".wud", L".wux", L".rpx", L".wuhb"}, true,
         {{L"Cemu", L"cemu", L"{fullscreen} -g \"{rom}\"", L"-f", {}, L"", true}},
         "Nativo (core do Cemu): WUD e WUX precisam das chaves do seu console em bios\\retroarch\\Cemu\\keys.txt; não tem "
         "save state. Jogo em pasta: a que tem code, content e meta."},
        {L"WII", "Wii", {L".wbfs", L".rvz", L".wia", L".iso", L".gcz", L".ciso", L".wad", L".dol", L".elf"}, false,
         {kDolphin, RetroArch(L"dolphin")},
         "Não precisa de BIOS."},
        {L"GC", "GameCube", {L".iso", L".gcm", L".rvz", L".wia", L".gcz", L".ciso", L".dol", L".elf"}, false,
         {kDolphin, RetroArch(L"dolphin")},
         "Não precisa de BIOS."},
        {L"3DS", "Nintendo 3DS", {L".3ds", L".cci", L".cxi", L".3dsx", L".app", L".z3dsx", L".zcci", L".zcxi"}, false,
         {{L"Azahar", L"azahar;lime3ds;citra", L"{fullscreen} \"{rom}\"", L"-f",
           {{L"user\\config\\qt-config.ini", "Renderer", "graphics_api\\default", "false"},
            {L"user\\config\\qt-config.ini", "Renderer", "graphics_api", "2"},
            {L"user\\config\\qt-config.ini", "Renderer", "resolution_factor\\default", "false"},
            {L"user\\config\\qt-config.ini", "Renderer", "resolution_factor", "3"}}}},
         "O jogo tem que estar descriptografado."},
        {L"NDS", "Nintendo DS", {L".nds", L".dsi", L".srl"}, false,
         {{L"melonDS", L"melonds", L"{fullscreen} \"{rom}\"", L"-f",
           {{L"melonDS.toml", "3D", "Renderer", "1"},
            {L"melonDS.toml", "3D.GL", "ScaleFactor", "4"},
            {L"melonDS.toml", "Screen", "UseGL", "true"}}},
          RetroArch(L"melondsds"), RetroArch(L"desmume")},
         "O melonDS roda sem BIOS, mas a do seu DS deixa mais fiel."},
        {L"GBA", "Game Boy Advance", {L".gba", L".agb"}, false,
         {kMgba, RetroArch(L"mgba")},
         "Não precisa de BIOS."},
        {L"GB", "Game Boy / Color", {L".gb", L".gbc", L".sgb"}, false,
         {kMgba, kMesen, RetroArch(L"gambatte")},
         "Não precisa de BIOS."},
        {L"N64", "Nintendo 64", {L".n64", L".z64", L".v64", L".ndd"}, false,
         {RetroArch(L"mupen64plus_next"), {L"Project64", L"project64", L"\"{rom}\"", L""}},
         "Não precisa de BIOS."},
        {L"SNES", "Super Nintendo", {L".sfc", L".smc", L".bs", L".st"}, false,
         {RetroArch(L"snes9x"), kMesen, {L"Snes9x", L"snes9x", L"{fullscreen} \"{rom}\"", L"-fullscreen"}, {L"bsnes", L"bsnes", L"{fullscreen} \"{rom}\"", L"--fullscreen"}},
         "Não precisa de BIOS."},
        {L"NES", "Nintendo (NES)", {L".nes", L".fds", L".unf", L".unif"}, false,
         {RetroArch(L"mesen"), kMesen, RetroArch(L"nestopia")},
         "Jogos de Famicom Disk System (.fds) precisam da BIOS disksys.rom."},
        {L"MD", "Mega Drive", {L".md", L".gen", L".smd", L".bin", L".68k", L".sgd"}, false,
         {RetroArch(L"genesis_plus_gx"), RetroArch(L"picodrive")},
         "Não precisa de BIOS."},
        {L"SMS", "Master System / Game Gear", {L".sms", L".gg", L".sg"}, false,
         {RetroArch(L"genesis_plus_gx"), RetroArch(L"picodrive")},
         "Não precisa de BIOS."},
        {L"SEGACD", "Sega CD", {L".cue", L".chd", L".iso", L".m3u"}, false,
         {RetroArch(L"genesis_plus_gx", L"retroarch"), RetroArch(L"picodrive", L"retroarch")},
         "Precisa da BIOS tirada do seu Sega CD em bios\\retroarch: bios_CD_U.bin (EUA), bios_CD_E.bin (Europa) ou "
         "bios_CD_J.bin (Japão), a da região do jogo."},
        {L"SATURN", "Sega Saturn", {L".cue", L".chd", L".ccd", L".mds", L".m3u"}, false,
         {RetroArch(L"mednafen_saturn", L"retroarch"), RetroArch(L"yabasanshiro", L"retroarch")},
         "Precisa da BIOS tirada do seu Saturn em bios\\retroarch: mpr-17933.bin (EUA e Europa) ou sega_101.bin (Japão)."},
        {L"DC", "Dreamcast", {L".gdi", L".cdi", L".chd", L".cue", L".m3u"}, false,
         {{L"Flycast", L"flycast", L"\"{rom}\"", L"",
           {{L"emu.cfg", "config", "pvr.rend", "4"},
            {L"emu.cfg", "config", "rend.Resolution", "1440"}}},
          RetroArch(L"flycast")},
         "A BIOS do seu Dreamcast deixa mais fiel; o Flycast também roda sem ela."},
        {L"XBOX", "Xbox", {L".iso", L".xiso"}, false,
         {{L"xemu", L"xemu", L"{fullscreen} -dvd_path \"{rom}\"", L"-full-screen",
           {{L"xemu.toml", "display", "renderer", "'VULKAN'"},
            {L"xemu.toml", "display.quality", "surface_scale", "2"},
            {L"xemu.toml", "display.ui", "show_menubar", "false"}},
           L"", true}},
         "Precisa da BIOS, da MCPX, da EEPROM e do HD do seu Xbox; no nativo (core do xemu, experimental) ficam em "
         "bios\\retroarch\\xemu, sem save state e só com o controle 1. O jogo tem que ser ISO ou XISO."},
        {L"X360", "Xbox 360", {L".iso", L".xex", L".zar"}, true,
         {{L"Xenia Canary", L"xenia", L"{fullscreen} \"{rom}\"", L"--fullscreen=true", {}, L"", true}},
         "Jogo em ISO, ou pasta com default.xex. No nativo (core do Xenia feito para o Ranga) os saves ficam no content do "
         "Xenia portátil, sem save state."},
        {L"PCE", "PC Engine / TurboGrafx-16", {L".pce", L".sgx"}, false,
         {RetroArch(L"mednafen_pce_fast"), RetroArch(L"mednafen_pce")},
         "Não precisa de BIOS (só os jogos em CD)."},
        {L"A2600", "Atari 2600", {L".a26"}, false,
         {RetroArch(L"stella")},
         "Não precisa de BIOS."},
        {L"ARCADE", "Arcade", {L".zip", L".7z"}, false,
         {RetroArch(L"fbneo"), RetroArch(L"mame"), {L"MAME", L"mame", L"-rompath \"{romdir}\" \"{romname}\"", L""}},
         "O .zip tem que ser da versão de romset que o core ou o MAME pede."},
    };
    // What Ranga Retro runs itself, by the core it loads; cores that draw with the GPU get Vulkan or OpenGL. Wii U (Cemu),
    // Xbox (xemu), Switch (Eden), Xbox 360 (Xenia) and PS3 (RPCS3) have no save states natively; the last three are
    // Ranga's own cores (eden-libretro, xenia-libretro, rpcs3-libretro), whose emulator draws in the game window itself.
    // mode=external gives any of them back to the full emulator.
    const std::pair<const wchar_t*, const wchar_t*> native[] = {
        {L"SNES", L"snes9x"}, {L"NES", L"mesen"}, {L"MD", L"genesis_plus_gx"}, {L"SMS", L"genesis_plus_gx"},
        {L"SEGACD", L"genesis_plus_gx"}, {L"SATURN", L"mednafen_saturn"}, {L"PCE", L"mednafen_pce_fast"},
        {L"A2600", L"stella"}, {L"ARCADE", L"fbneo"}, {L"GBA", L"mgba"}, {L"GB", L"gambatte"}, {L"PS1", L"swanstation"},
        {L"NDS", L"melondsds"}, {L"PSP", L"ppsspp"}, {L"DC", L"flycast"}, {L"PS2", L"pcsx2"}, {L"GC", L"dolphin"}, {L"WII", L"dolphin"},
        {L"3DS", L"azahar"}, {L"N64", L"mupen64plus_next"}, {L"WIIU", L"cemu"}, {L"XBOX", L"xemu"},
        {L"SWITCH", L"eden"}, {L"X360", L"xenia"}, {L"PS3", L"rpcs3"},
    };
    for (auto& system : systems) {
        for (const auto& [id, core] : native) {
            if (system.id == id) system.nativeCore = core;
        }
    }
    return systems;
}

std::wstring EnginesIni(const std::wstring& root) { return root + L"\\engines.ini"; }

const Preset* MatchPreset(const System& system, const std::wstring& exe) {
    const std::wstring name = Lower(FileName(exe));
    for (const auto& preset : system.presets) {
        size_t start = 0;
        while (start <= preset.exePrefixes.size()) {
            size_t end = preset.exePrefixes.find(L';', start);
            if (end == std::wstring::npos) end = preset.exePrefixes.size();
            const std::wstring prefix = preset.exePrefixes.substr(start, end - start);
            if (!prefix.empty() && name.rfind(prefix, 0) == 0) return &preset;
            start = end + 1;
        }
    }
    return nullptr;
}

void Replace(std::wstring& text, const std::wstring& from, const std::wstring& to) {
    for (size_t at = text.find(from); at != std::wstring::npos; at = text.find(from, at + to.size())) text.replace(at, from.size(), to);
}

std::wstring Expand(std::wstring text, const std::wstring& exe, const std::wstring& rom, const std::wstring& fullscreen) {
    std::wstring romName = FileName(rom);
    if (const size_t dot = romName.find_last_of(L'.'); dot != std::wstring::npos) romName.resize(dot);
    Replace(text, L"{fullscreen}", fullscreen);
    Replace(text, L"{exedir}", ParentFolder(exe));
    Replace(text, L"{romdir}", ParentFolder(rom));
    Replace(text, L"{romname}", romName);
    Replace(text, L"{rom}", rom);
    std::wstring tidy;
    for (const wchar_t character : text) {
        if (character == L' ' && (tidy.empty() || tidy.back() == L' ')) continue;
        tidy += character;
    }
    while (!tidy.empty() && tidy.back() == L' ') tidy.pop_back();
    return tidy;
}

// Raw reads of a game file at any offset; short or empty past its end.
class Reader {
public:
    explicit Reader(const std::wstring& path) { _wfopen_s(&file, path.c_str(), L"rb"); }
    ~Reader() { if (file != nullptr) fclose(file); }
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    bool Open() const { return file != nullptr; }
    std::string At(long long offset, size_t size) {
        std::string data(size, '\0');
        if (file == nullptr || _fseeki64(file, offset, SEEK_SET) != 0) return {};
        data.resize(fread(data.data(), 1, size, file));
        return data;
    }

private:
    FILE* file = nullptr;
};

unsigned Le32(const std::string& data, size_t at) {
    if (at + 4 > data.size()) return 0;
    return static_cast<unsigned char>(data[at]) | static_cast<unsigned char>(data[at + 1]) << 8 | static_cast<unsigned char>(data[at + 2]) << 16 | static_cast<unsigned>(static_cast<unsigned char>(data[at + 3])) << 24;
}

unsigned Be32(const std::string& data, size_t at) {
    if (at + 4 > data.size()) return 0;
    return static_cast<unsigned>(static_cast<unsigned char>(data[at])) << 24 | static_cast<unsigned char>(data[at + 1]) << 16 | static_cast<unsigned char>(data[at + 2]) << 8 | static_cast<unsigned char>(data[at + 3]);
}

// Printable text from a fixed field of a disc header, Latin-1, trimmed.
std::wstring HeaderText(const std::string& data, size_t at, size_t size) {
    std::wstring text;
    for (size_t i = at; i < at + size && i < data.size() && data[i] != '\0'; ++i) text += static_cast<wchar_t>(static_cast<unsigned char>(data[i]));
    while (!text.empty() && text.back() == L' ') text.pop_back();
    return text;
}

// An ISO 9660 disc, as a plain 2048-byte image or a raw 2352-byte CD track (mode 1 or mode 2).
class Iso {
public:
    explicit Iso(const std::wstring& path) : reader(path) {
        for (const auto& [size, data] : {std::pair{2048LL, 0LL}, std::pair{2352LL, 16LL}, std::pair{2352LL, 24LL}}) {
            const std::string descriptor = reader.At(16 * size + data, 2048);
            if (descriptor.size() == 2048 && descriptor.compare(0, 6, std::string("\x01" "CD001", 6)) == 0) {
                sectorSize = size;
                dataOffset = data;
                root = descriptor.substr(156, 34);
                valid = true;
                return;
            }
        }
    }
    bool Valid() const { return valid; }

    // The contents of a file by its path ("PSP_GAME/PARAM.SFO"), at most limit bytes; empty when it is not there.
    std::string ReadFile(const std::string& path, size_t limit = 1 << 20) {
        std::string record = root;
        size_t start = 0;
        while (start < path.size()) {
            size_t end = path.find('/', start);
            if (end == std::string::npos) end = path.size();
            record = FindIn(record, path.substr(start, end - start));
            if (record.empty()) return {};
            start = end + 1;
        }
        return Extent(Le32(record, 2), std::min<size_t>(Le32(record, 10), limit));
    }

private:
    std::string Extent(unsigned lba, size_t size) {
        std::string data;
        for (unsigned sector = lba; data.size() < size; ++sector) {
            const std::string block = reader.At(static_cast<long long>(sector) * sectorSize + dataOffset, 2048);
            if (block.empty()) break;
            data += block.substr(0, std::min(block.size(), size - data.size()));
        }
        return data;
    }

    // The record of name inside the folder of record, compared without case and without the ";1" version.
    std::string FindIn(const std::string& folder, const std::string& name) {
        const std::string entries = Extent(Le32(folder, 2), std::min<size_t>(Le32(folder, 10), 1 << 20));
        size_t at = 0;
        while (at < entries.size()) {
            const unsigned char length = static_cast<unsigned char>(entries[at]);
            if (length == 0) {
                at = (at / 2048 + 1) * 2048;
                continue;
            }
            if (at + 33 <= entries.size()) {
                std::string entry = entries.substr(at + 33, static_cast<unsigned char>(entries[at + 32]));
                if (const size_t version = entry.find(';'); version != std::string::npos) entry.resize(version);
                if (entry.size() == name.size() && _strnicmp(entry.c_str(), name.c_str(), name.size()) == 0) return entries.substr(at, length);
            }
            at += length;
        }
        return {};
    }

    Reader reader;
    long long sectorSize = 2048, dataOffset = 0;
    std::string root;
    bool valid = false;
};

// SLUS_204.68 in SYSTEM.CNF is the serial SLUS-20468.
std::wstring BootSerial(const std::string& cnf) {
    const size_t colon = cnf.find(":\\");
    if (colon == std::string::npos) return {};
    std::wstring serial;
    for (size_t i = colon + 2; i < cnf.size() && cnf[i] != ';' && cnf[i] != '\r' && cnf[i] != '\n'; ++i) {
        if (cnf[i] == '_') serial += L'-';
        else if (cnf[i] != '.') serial += static_cast<wchar_t>(static_cast<unsigned char>(cnf[i]));
    }
    return serial;
}

// What a disc image holds, read from its file system and its first sector. The file can be a cue sheet's track.
bool DetectDisc(const std::wstring& path, Detected& found) {
    Reader reader(path);
    for (const long long offset : {0LL, 16LL, 24LL}) {
        const std::string head = reader.At(offset, 16);
        if (head.rfind("SEGA SEGASATURN", 0) == 0) { found.systems = {L"SATURN"}; return true; }
        if (head.rfind("SEGADISCSYSTEM", 0) == 0) { found.systems = {L"SEGACD"}; return true; }
        if (head.rfind("SEGA SEGAKATANA", 0) == 0) { found.systems = {L"DC"}; return true; }
    }
    // Xbox discs: the XDVDFS volume sits at one of these places, and the root lists default.xbe (Xbox) or default.xex (360).
    for (const long long volume : {0x10000LL, 0xFDA0000LL, 0x2090000LL, 0x18310000LL}) {
        const std::string descriptor = reader.At(volume, 32);
        if (descriptor.rfind("MICROSOFT*XBOX*MEDIA", 0) != 0) continue;
        const long long base = volume - 0x10000;
        std::string folder = reader.At(base + static_cast<long long>(Le32(descriptor, 20)) * 2048, std::min<size_t>(Le32(descriptor, 24), 1 << 16));
        for (auto& character : folder) character = static_cast<char>(tolower(static_cast<unsigned char>(character)));
        if (folder.find("default.xex") != std::string::npos) found.systems = {L"X360"};
        else if (folder.find("default.xbe") != std::string::npos) found.systems = {L"XBOX"};
        else found.systems = volume == 0xFDA0000LL || volume == 0x2090000LL ? std::vector<std::wstring>{L"X360"} : std::vector<std::wstring>{L"XBOX", L"X360"};
        return true;
    }
    Iso iso(path);
    if (!iso.Valid()) return false;
    if (const std::string sfo = iso.ReadFile("PS3_GAME/PARAM.SFO"); !sfo.empty()) {
        found.systems = {L"PS3"};
        found.serial = Wide(SfoString(sfo, "TITLE_ID"));
        found.title = Wide(SfoString(sfo, "TITLE"));
        return true;
    }
    if (const std::string sfo = iso.ReadFile("PSP_GAME/PARAM.SFO"); !sfo.empty()) {
        found.systems = {L"PSP"};
        found.serial = Wide(SfoString(sfo, "DISC_ID"));
        found.title = Wide(SfoString(sfo, "TITLE"));
        return true;
    }
    if (const std::string cnf = iso.ReadFile("SYSTEM.CNF", 4096); !cnf.empty()) {
        found.systems = {cnf.find("BOOT2") != std::string::npos ? L"PS2" : L"PS1"};
        found.serial = BootSerial(cnf);
        return true;
    }
    return false;
}

// GameCube and Wii discs carry their kind as a magic number in the disc header, at 0 in a plain image, after the WBFS or
// CISO header, or inside the WIA/RVZ header.
bool DetectNintendoDisc(const std::wstring& path, const std::wstring& extension, Detected& found) {
    Reader reader(path);
    long long offset = 0;
    if (extension == L".wbfs") offset = 0x200;
    else if (extension == L".ciso") offset = 0x8000;
    std::string header;
    if (extension == L".rvz" || extension == L".wia") {
        const std::string wia = reader.At(0, 0x58 + 0x80);
        if (wia.size() < 0x58 + 0x80 || (wia.rfind("RVZ\x01", 0) != 0 && wia.rfind("WIA\x01", 0) != 0)) return false;
        header = wia.substr(0x58);
    } else {
        header = reader.At(offset, 0x80);
    }
    if (Be32(header, 0x18) == 0x5D1C9EA3u) found.systems = {L"WII"};
    else if (Be32(header, 0x1C) == 0xC2339F3Du) found.systems = {L"GC"};
    else return false;
    found.serial = HeaderText(header, 0, 6);
    found.title = HeaderText(header, 0x20, 0x40);
    return true;
}

// CHD keeps a CD or a DVD; its first metadata entry says which (v5 header).
std::vector<std::wstring> ChdSystems(const std::wstring& path) {
    Reader reader(path);
    const std::string header = reader.At(0, 0x7C);
    if (header.rfind("MComprHD", 0) == 0 && Be32(header, 12) == 5) {
        const long long meta = static_cast<long long>(Be32(header, 0x30)) << 32 | Be32(header, 0x34);
        const std::string tag = reader.At(meta, 4);
        if (tag == "DVD ") return {L"PS2", L"PSP"};
        if (tag == "CHGD") return {L"DC"};
        if (tag == "CHT2" || tag == "CHTR" || tag == "CHCD") return {L"PS1", L"SATURN", L"SEGACD", L"DC"};
    }
    return {L"PS1", L"PS2", L"PSP", L"SATURN", L"SEGACD", L"DC"};
}

// The first file a cue sheet or an m3u playlist names, beside it.
std::wstring FirstListedFile(const std::wstring& path, bool cue) {
    const std::string text = ReadBytes(path);
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (cue) {
            const size_t file = line.find("FILE");
            const size_t open = line.find('"'), close = line.rfind('"');
            if (file == std::string::npos || open == std::string::npos || close <= open) continue;
            line = line.substr(open + 1, close - open - 1);
        } else if (line.empty() || line[0] == '#') {
            continue;
        }
        const std::wstring name = Wide(line);
        return name.find(L':') != std::wstring::npos ? name : ParentFolder(path) + L"\\" + name;
    }
    return {};
}

std::wstring Fnv(const std::wstring& text) {
    unsigned hash = 2166136261u;
    for (const wchar_t character : Lower(text)) {
        hash ^= static_cast<unsigned>(character);
        hash *= 16777619u;
    }
    wchar_t hex[12];
    std::swprintf(hex, 12, L"%08X", hash);
    return hex;
}

std::wstring FirstMatch(const std::wstring& folder, const wchar_t* pattern) {
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\" + pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return L"";
    FindClose(search);
    return folder + L"\\" + found.cFileName;
}

}

const std::vector<System>& Systems() {
    static const std::vector<System> systems = MakeSystems();
    return systems;
}

const System* FindSystem(const std::wstring& id) {
    for (const auto& system : Systems()) {
        if (system.id == id) return &system;
    }
    return nullptr;
}

std::string SystemName(const std::wstring& id) {
    if (id == L"PS5") return "PlayStation 5";
    const System* system = FindSystem(id);
    return system != nullptr ? system->name : Utf8(id);
}

std::wstring NativeCorePath(const std::wstring& root, const System& system) { return root + L"\\cores\\" + system.nativeCore + L"_libretro.dll"; }

std::wstring ControlsFile(const std::wstring& root, const std::wstring& system) { return root + L"\\controles\\" + system + L".ini"; }

namespace {

// Every place of old in text, by ASCII letters in any case (Windows paths), becomes replacement; returns how many.
template <typename String> size_t ReplaceAllNoCase(String& text, const String& old, const String& replacement) {
    if (old.empty()) return 0;
    const auto lower = [](auto c) { return c >= 'A' && c <= 'Z' ? static_cast<decltype(c)>(c - 'A' + 'a') : c; };
    size_t count = 0;
    for (size_t at = 0; at + old.size() <= text.size();) {
        bool same = true;
        for (size_t i = 0; i < old.size() && same; ++i) same = lower(text[at + i]) == lower(old[i]);
        if (!same) {
            ++at;
            continue;
        }
        text.replace(at, old.size(), replacement);
        at += replacement.size();
        ++count;
    }
    return count;
}

// The old folder as each kind of file writes it, followed by what ends a folder name in a path (so C:\ps5 never
// matches inside C:\ps55), each paired with the new folder written the same way.
template <typename String> std::vector<std::pair<String, String>> PathForms(const String& from, const String& to) {
    const auto with = [](String path, typename String::value_type separator, bool doubled) {
        String out;
        for (const auto c : path) {
            if (c == '\\') {
                out += separator;
                if (doubled) out += separator;
            } else {
                out += c;
            }
        }
        return out;
    };
    std::vector<std::pair<String, String>> forms;
    for (const auto& [separator, doubled] : {std::pair{static_cast<typename String::value_type>('\\'), false},
                                             std::pair{static_cast<typename String::value_type>('/'), false},
                                             std::pair{static_cast<typename String::value_type>('\\'), true}}) {
        const String a = with(from, separator, doubled), b = with(to, separator, doubled);
        String end(1, separator);
        if (doubled) end += separator;
        forms.push_back({a + end, b + end});
        for (const char close : {'"', '\'', '\r', '\n'}) forms.push_back({a + static_cast<typename String::value_type>(close), b + static_cast<typename String::value_type>(close)});
    }
    return forms;
}

}

int RelocateRoot(const std::wstring& root) {
    const std::wstring ini = EnginesIni(root);
    std::wstring old = ReadIni(ini, L"Ranga", L"raiz", L"");
    // A folder from before this was kept: where the full emulators were says where it was.
    if (old.empty()) {
        const std::string text = ReadBytes(ini);
        const std::wstring wide = text.size() >= 2 && static_cast<unsigned char>(text[0]) == 0xFF && static_cast<unsigned char>(text[1]) == 0xFE
                                      ? std::wstring(reinterpret_cast<const wchar_t*>(text.data() + 2), (text.size() - 2) / 2)
                                      : Wide(text);
        const size_t at = Lower(wide).find(L"\\emuladores\\");
        if (at != std::wstring::npos) {
            const size_t start = wide.rfind(L'=', at);
            if (start != std::wstring::npos) old = wide.substr(start + 1, at - start - 1);
        }
    }
    const auto clean = [](std::wstring path) {
        while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) path.pop_back();
        return path;
    };
    old = clean(old);
    const std::wstring now = clean(root);
    int changed = 0;
    if (!old.empty() && _wcsicmp(old.c_str(), now.c_str()) != 0) {
        // Every text file that may hold a path: the client's own lists, the games' options, the full emulators' and the
        // cores' settings. Logs and caches are left alone; links are not followed. Each file changed is kept first
        // in backups\mudou-de-pasta.
        SYSTEMTIME clock;
        GetLocalTime(&clock);
        wchar_t stamp[32];
        swprintf(stamp, 32, L"%04u-%02u-%02u-%02u%02u", clock.wYear, clock.wMonth, clock.wDay, clock.wHour, clock.wMinute);
        const std::wstring keep = root + L"\\backups\\mudou-de-pasta-" + stamp;
        static const wchar_t* const kinds[] = {L".ini", L".cfg", L".toml", L".yml", L".yaml", L".json", L".xml", L".conf", L".settings", L".opt"};
        std::vector<std::wstring> files = {ini, root + L"\\library.ini"};
        std::vector<std::wstring> pending = {root + L"\\emuladores", root + L"\\bios", root + L"\\saves"};
        while (!pending.empty()) {
            const std::wstring folder = pending.back();
            pending.pop_back();
            WIN32_FIND_DATAW found;
            const HANDLE search = FindFirstFileW((folder + L"\\*").c_str(), &found);
            if (search == INVALID_HANDLE_VALUE) continue;
            do {
                const std::wstring name = found.cFileName;
                if (name == L"." || name == L".." || (found.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) continue;
                const std::wstring path = folder + L"\\" + name;
                if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
                    const std::wstring lower = Lower(name);
                    if (lower != L"log" && lower != L"logs" && lower.find(L"cache") == std::wstring::npos && lower != L"_downloads") pending.push_back(path);
                } else if (found.nFileSizeHigh == 0 && found.nFileSizeLow <= 4u * 1024 * 1024) {
                    const std::wstring extension = Extension(path);
                    for (const wchar_t* kind : kinds) {
                        if (extension == kind) files.push_back(path);
                    }
                }
            } while (FindNextFileW(search, &found));
            FindClose(search);
        }
        std::wstring log;
        for (const auto& path : files) {
            const std::string data = ReadBytes(path);
            if (data.empty()) continue;
            std::string out;
            size_t count = 0;
            if (data.size() >= 2 && static_cast<unsigned char>(data[0]) == 0xFF && static_cast<unsigned char>(data[1]) == 0xFE) {
                std::wstring text(reinterpret_cast<const wchar_t*>(data.data() + 2), (data.size() - 2) / 2);
                for (const auto& [from, to] : PathForms(old, now)) count += ReplaceAllNoCase(text, from, to);
                out = data.substr(0, 2) + std::string(reinterpret_cast<const char*>(text.data()), text.size() * sizeof(wchar_t));
            } else {
                out = data;
                for (const auto& [from, to] : PathForms(Utf8(old), Utf8(now))) count += ReplaceAllNoCase(out, from, to);
            }
            if (count == 0) continue;
            // No copy kept, no change: the old file must be there to go back to.
            const std::wstring copy = keep + path.substr(root.size());
            std::error_code error;
            std::filesystem::create_directories(std::filesystem::path(ParentFolder(copy)), error);
            if (!CopyFileW(path.c_str(), copy.c_str(), FALSE)) continue;
            FILE* file = nullptr;
            if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) continue;
            std::fwrite(out.data(), 1, out.size(), file);
            std::fclose(file);
            ++changed;
            log += path.substr(root.size()) + L"  (" + std::to_wstring(count) + L")\r\n";
        }
        if (changed > 0) {
            const std::string text = Utf8(L"De " + old + L" para " + now + L"\r\n" + log);
            FILE* file = nullptr;
            if (_wfopen_s(&file, (keep + L"\\o-que-mudou.txt").c_str(), L"wb") == 0 && file != nullptr) {
                std::fwrite(text.data(), 1, text.size(), file);
                std::fclose(file);
            }
        }
    }
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(L"Ranga", L"raiz", now.c_str(), ini.c_str());
    return changed;
}

void RepairLinks(const std::wstring& root) {
    // The Switch, PS3 and Xbox 360 cores keep their system files in bios\retroarch, which point at the full emulators'
    // folders so both see the same keys, firmware and saves. A copy, a zip or a move of the Ranga folder loses these
    // links (a zip keeps no junctions; one copied whole would still point at the old place): they are made again here.
    static const wchar_t* const rpcs3[] = {L"dev_bdvd", L"dev_flash", L"dev_flash2", L"dev_flash3", L"dev_hdd0", L"dev_hdd1",
                                           L"dev_usb000", L"fonts", L"games", L"Icons", L"patches", L"sounds"};
    std::vector<std::pair<std::wstring, std::wstring>> links = {
        {L"bios\\retroarch\\eden", L"emuladores\\eden\\user"},
        {L"bios\\retroarch\\xenia\\content", L"emuladores\\xenia\\content"},
    };
    for (const wchar_t* name : rpcs3) links.push_back({std::wstring(L"bios\\retroarch\\rpcs3\\") + name, std::wstring(L"emuladores\\rpcs3\\") + name});
    const std::wstring log = root + L"\\bios\\retroarch\\links.log";
    for (const auto& [link, target] : links) {
        // Only where the emulator's folder is there; a real folder already in the link's place is left as it is.
        if (IsDirectory(root + L"\\" + target)) LinkFolder(root + L"\\" + link, root + L"\\" + target, log);
    }
}

bool ShowPs4Ps5(const std::wstring& root) { return ReadIni(EnginesIni(root), L"Ranga", L"ps4ps5", L"0") == L"1"; }

void SetShowPs4Ps5(const std::wstring& root, bool on) {
    EnsureUnicodeIni(EnginesIni(root));
    WritePrivateProfileStringW(L"Ranga", L"ps4ps5", on ? L"1" : nullptr, EnginesIni(root).c_str());
}

std::wstring Quality(const std::wstring& root) {
    const std::wstring level = ReadIni(EnginesIni(root), L"Ranga", L"qualidade", L"bom");
    return level == L"ruim" || level == L"fraco" || level == L"otimo" ? level : L"bom";
}

void SetQuality(const std::wstring& root, const std::wstring& level) {
    EnsureUnicodeIni(EnginesIni(root));
    WritePrivateProfileStringW(L"Ranga", L"qualidade", level.c_str(), EnginesIni(root).c_str());
}

Engine LoadEngine(const std::wstring& root, const std::wstring& id) {
    Engine engine;
    const System* system = FindSystem(id);
    if (system == nullptr) {
        engine.problem = L"O Ranga não conhece o console " + id + L".";
        return engine;
    }
    const std::wstring ini = EnginesIni(root);
    // Native by default where Ranga Retro has the console's core; mode=external hands it to the chosen emulator.
    engine.nativeAvailable = !system->nativeCore.empty();
    if (engine.nativeAvailable && ReadIni(ini, id.c_str(), L"mode", L"native") != L"external") {
        engine.native = true;
        engine.exe = root + L"\\ranga_retro.exe";
        engine.presetName = L"Ranga (nativo, " + system->nativeCore + L")";
        engine.args = L"--core \"" + NativeCorePath(root, *system) + L"\" --rom \"{rom}\" --title \"{title}\" --system \"" + root +
                      L"\\bios\\retroarch\" --saves \"{workspace}\\saves\" --options \"{workspace}\\core-options.ini\" --controls \"" + ControlsFile(root, id) +
                      L"\" --quality " + Quality(root) + L" {fullscreen}";
        engine.fullscreen = L"--fullscreen";
        for (const auto& preset : system->presets) {
            if (!preset.bios.empty()) engine.bios = root + L"\\bios\\retroarch";
        }
        if (!Exists(engine.exe)) engine.problem = L"Falta o ranga_retro.exe ao lado do RangaEmu.exe. Extraia o download inteiro de novo.";
        else if (!Exists(NativeCorePath(root, *system))) engine.problem = L"Falta o core " + system->nativeCore + L"_libretro.dll em " + root + L"\\cores.";
        else engine.ready = true;
        return engine;
    }
    engine.exe = ReadIni(ini, id.c_str(), L"exe", L"");
    const Preset* preset = MatchPreset(*system, engine.exe);
    if (preset != nullptr) {
        engine.presetName = preset->name;
        engine.tunable = !preset->tweaks.empty();
        engine.embed = preset->embed;
        if (!preset->bios.empty()) engine.bios = root + L"\\bios\\" + preset->bios;
    }
    engine.args = ReadIni(ini, id.c_str(), L"args", preset != nullptr ? preset->args.c_str() : L"\"{rom}\"");
    engine.fullscreen = ReadIni(ini, id.c_str(), L"fullscreen", preset != nullptr ? preset->fullscreen.c_str() : L"");
    if (engine.exe.empty()) {
        engine.problem = L"Falta escolher o emulador de " + Wide(system->name) + L" em Emuladores, na barra lateral.";
        return engine;
    }
    if (!Exists(engine.exe)) {
        engine.problem = L"O emulador de " + Wide(system->name) + L" não está mais em " + engine.exe + L". Escolha de novo em Emuladores.";
        return engine;
    }
    // A RetroArch core is a file the user downloads inside RetroArch; without it RetroArch opens and shows nothing.
    const std::wstring expanded = Expand(engine.args, engine.exe, L"", L"");
    if (const size_t core = expanded.find(L"_libretro.dll"); core != std::wstring::npos) {
        const size_t open = expanded.rfind(L'"', core);
        const std::wstring path = expanded.substr(open + 1, core + 13 - open - 1);
        if (open != std::wstring::npos && !Exists(path)) {
            engine.problem = L"Falta o core " + FileName(path) + L" no RetroArch. Baixe por Menu principal > Online Updater > Core Downloader.";
            return engine;
        }
    }
    engine.ready = true;
    return engine;
}

void ChooseEngine(const std::wstring& root, const std::wstring& id, const std::wstring& exe) {
    const std::wstring ini = EnginesIni(root);
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(id.c_str(), L"exe", exe.c_str(), ini.c_str());
    // A new emulator brings its own arguments; the ones written for the last would not fit it.
    WritePrivateProfileStringW(id.c_str(), L"args", nullptr, ini.c_str());
    WritePrivateProfileStringW(id.c_str(), L"fullscreen", nullptr, ini.c_str());
}

void SaveEngineArgs(const std::wstring& root, const std::wstring& id, const std::wstring& args, const std::wstring& fullscreen) {
    const std::wstring ini = EnginesIni(root);
    EnsureUnicodeIni(ini);
    WritePrivateProfileStringW(id.c_str(), L"args", args.c_str(), ini.c_str());
    WritePrivateProfileStringW(id.c_str(), L"fullscreen", fullscreen.c_str(), ini.c_str());
}

void ResetEngineArgs(const std::wstring& root, const std::wstring& id) {
    WritePrivateProfileStringW(id.c_str(), L"args", nullptr, EnginesIni(root).c_str());
    WritePrivateProfileStringW(id.c_str(), L"fullscreen", nullptr, EnginesIni(root).c_str());
}

void ClearEngine(const std::wstring& root, const std::wstring& id) { WritePrivateProfileStringW(id.c_str(), nullptr, nullptr, EnginesIni(root).c_str()); }

std::wstring EngineCommand(const Engine& engine, const std::wstring& rom, bool fullscreen, const std::wstring& title, const std::wstring& workspace) {
    std::wstring args = engine.args;
    // A title with quotes would break the command line; it is only shown, so they go.
    std::wstring safeTitle = title;
    safeTitle.erase(std::remove(safeTitle.begin(), safeTitle.end(), L'"'), safeTitle.end());
    Replace(args, L"{title}", safeTitle);
    Replace(args, L"{workspace}", workspace);
    args = Expand(args, engine.exe, rom, fullscreen ? engine.fullscreen : L"");
    return L"\"" + engine.exe + L"\"" + (args.empty() ? L"" : L" " + args);
}

void SetNative(const std::wstring& root, const std::wstring& id, bool native) {
    EnsureUnicodeIni(EnginesIni(root));
    WritePrivateProfileStringW(id.c_str(), L"mode", native ? nullptr : L"external", EnginesIni(root).c_str());
}

namespace {

// A file matching pattern (a folder and a wildcard name); with sizes, only one of those sizes, since a BIOS dump keeps
// the size of the chip whatever the user named it.
bool HasMatch(const std::wstring& pattern, std::initializer_list<long long> sizes = {}) {
    WIN32_FIND_DATAW data;
    const HANDLE find = FindFirstFileW(pattern.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do {
        if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const long long size = (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        found = sizes.size() == 0 || std::find(sizes.begin(), sizes.end(), size) != sizes.end();
    } while (!found && FindNextFileW(find, &data));
    FindClose(find);
    return found;
}

bool HasAny(const std::wstring& folder, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* name : names) {
        if (Exists(folder + L"\\" + name)) return true;
    }
    return false;
}

std::wstring Missing(const std::wstring& what, const std::wstring& folder) {
    return L"Falta " + what + L" em " + folder + L". Tire do seu próprio console, coloque lá e tente de novo.";
}

// The DLLs a Windows program or DLL imports, read from its import table (lower-case). Read once per file.
std::vector<std::string> Imports(const std::wstring& path) {
    static std::map<std::wstring, std::vector<std::string>> cache;
    if (const auto found = cache.find(path); found != cache.end()) return found->second;
    std::vector<std::string> names;
    Reader reader(path);
    const auto le16 = [](const std::string& data, size_t at) { return at + 2 > data.size() ? 0u : static_cast<unsigned>(static_cast<unsigned char>(data[at]) | static_cast<unsigned char>(data[at + 1]) << 8); };
    const std::string dos = reader.At(0, 64);
    if (dos.size() == 64 && dos.compare(0, 2, "MZ") == 0) {
        const unsigned pe = Le32(dos, 0x3c);
        const std::string header = reader.At(pe, 24 + 240);
        if (header.size() >= 24 + 112 && header.compare(0, 4, std::string("PE\0\0", 4)) == 0) {
            const unsigned sections = le16(header, 6), optionalSize = le16(header, 20);
            const bool plus = le16(header, 24) == 0x20b;
            const unsigned importRva = Le32(header, 24 + (plus ? 120 : 104));
            const std::string table = reader.At(pe + 24 + optionalSize, static_cast<size_t>(sections) * 40);
            // Where an address of the loaded image is in the file.
            const auto offset = [&](unsigned rva) -> long long {
                for (unsigned s = 0; s < sections; ++s) {
                    const unsigned virtualAddress = Le32(table, s * 40 + 12), rawSize = Le32(table, s * 40 + 16), raw = Le32(table, s * 40 + 20);
                    const unsigned size = std::max(Le32(table, s * 40 + 8), rawSize);
                    if (rva >= virtualAddress && rva < virtualAddress + size) return static_cast<long long>(raw) + (rva - virtualAddress);
                }
                return -1;
            };
            const long long first = importRva != 0 ? offset(importRva) : -1;
            for (int i = 0; first >= 0 && i < 256; ++i) {
                const std::string entry = reader.At(first + i * 20, 20);
                if (entry.size() < 20 || Le32(entry, 12) == 0) break;
                const long long at = offset(Le32(entry, 12));
                if (at < 0) continue;
                std::string name = reader.At(at, 64);
                name.resize(std::min(name.size(), name.find('\0')));
                for (auto& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                names.push_back(name);
            }
        }
    }
    cache[path] = names;
    return names;
}

// The first piece of Microsoft's Visual C++ runtime the program imports and this PC lacks (not in Windows' system
// folder nor beside the program); empty when nothing is missing.
std::wstring MissingVcRuntime(const std::wstring& program) {
    wchar_t system[MAX_PATH] = {};
    GetSystemDirectoryW(system, MAX_PATH);
    for (const auto& name : Imports(program)) {
        if (name.rfind("msvcp140", 0) != 0 && name.rfind("vcruntime140", 0) != 0 && name.rfind("concrt140", 0) != 0) continue;
        const std::wstring dll = Wide(name);
        if (!Exists(std::wstring(system) + L"\\" + dll) && !Exists(ParentFolder(program) + L"\\" + dll)) return dll;
    }
    return L"";
}

}

std::wstring MissingFiles(const std::wstring& root, const std::wstring& id, const Engine& engine, const std::wstring& rom) {
    const System* system = FindSystem(id);
    if (system == nullptr || engine.exe.empty()) return L"";
    // The PS1 and Xbox 360 cores and several full emulators are built with Visual C++ and need its runtime, which
    // Windows does not always have; without it they would not even load.
    {
        const std::wstring program = engine.native ? NativeCorePath(root, *system) : engine.exe;
        if (const std::wstring dll = MissingVcRuntime(program); !dll.empty()) {
            return L"Falta o Microsoft Visual C++ 2015-2022 (x64) neste PC (" + dll + L" não está instalado), e o " +
                   (engine.native ? L"core do " + Wide(system->name) : FileName(engine.exe)) +
                   L" precisa dele. Instale o \"Visual C++ Redistributable\" do site da Microsoft (aka.ms/vs/17/release/vc_redist.x64.exe) e tente de novo.";
        }
    }
    constexpr long long kMiB = 1024 * 1024;
    // The core Ranga Retro or RetroArch loads; a standalone emulator has none and is told apart by its BIOS folder.
    std::wstring core;
    if (engine.native) {
        core = system->nativeCore;
    } else {
        const std::wstring args = Expand(engine.args, engine.exe, L"", L"");
        if (const size_t end = args.find(L"_libretro.dll"); end != std::wstring::npos) {
            const size_t slash = args.find_last_of(L"\\/", end);
            if (slash != std::wstring::npos) core = args.substr(slash + 1, end - slash - 1);
        }
    }
    const std::wstring exeDir = ParentFolder(engine.exe);
    // Ranga Retro reads bios\retroarch; RetroArch reads it once fitted to this PC (TuneEngine), its own system folder
    // before that, so a file in either counts.
    std::vector<std::wstring> folders = {root + L"\\bios\\retroarch"};
    if (!engine.native && !core.empty()) folders.push_back(exeDir + L"\\system");
    const auto inSystem = [&](const auto& check) {
        for (const auto& folder : folders) {
            if (check(folder)) return true;
        }
        return false;
    };
    const std::wstring shown = folders.front();
    const std::wstring ext = Extension(rom);

    if (core == L"pcsx2" && !inSystem([&](const std::wstring& f) { return HasMatch(f + L"\\pcsx2\\bios\\*", {4 * kMiB}); }))
        return Missing(L"a BIOS do PS2 (um arquivo de 4 MB)", shown + L"\\pcsx2\\bios");
    if (core.empty() && id == L"PS2" && !engine.bios.empty() && !HasMatch(engine.bios + L"\\*", {4 * kMiB}))
        return Missing(L"a BIOS do PS2 (um arquivo de 4 MB)", engine.bios);
    // DuckStation also boots from a PS2 BIOS; in native mode Ranga brings its own PS1 BIOS.
    if (core.empty() && id == L"PS1" && !engine.bios.empty() && !HasMatch(engine.bios + L"\\*", {kMiB / 2, 4 * kMiB}))
        return Missing(L"a BIOS do PS1", engine.bios) + L" No modo nativo não precisa: o Ranga tem uma BIOS própria de PS1.";
    if (core == L"mednafen_psx_hw" && !inSystem([](const std::wstring& f) { return HasAny(f, {L"scph5500.bin", L"scph5501.bin", L"scph5502.bin"}); }))
        return Missing(L"a BIOS do PS1 (scph5500.bin, scph5501.bin ou scph5502.bin)", shown);
    if (id == L"SEGACD" && (core == L"genesis_plus_gx" || core == L"picodrive") &&
        !inSystem([](const std::wstring& f) { return HasAny(f, {L"bios_CD_U.bin", L"bios_CD_E.bin", L"bios_CD_J.bin"}); }))
        return Missing(L"a BIOS do Sega CD (bios_CD_U.bin, bios_CD_E.bin ou bios_CD_J.bin)", shown);
    if (core == L"mednafen_saturn" && !inSystem([](const std::wstring& f) { return HasAny(f, {L"mpr-17933.bin", L"sega_101.bin"}); }))
        return Missing(L"a BIOS do Saturn (mpr-17933.bin ou sega_101.bin)", shown);
    if (core == L"xemu") {
        std::wstring absent;
        for (const wchar_t* name : {L"mcpx_1.0.bin", L"Complex_4627v1.03.bin", L"xbox_hdd.qcow2"}) {
            if (!Exists(shown + L"\\xemu\\" + name)) absent += (absent.empty() ? L"" : L", ") + std::wstring(name);
        }
        if (!absent.empty()) return Missing(L"o que o xemu precisa do seu Xbox (" + absent + L")", shown + L"\\xemu");
    }
    if (core == L"eden") {
        if (!Exists(shown + L"\\eden\\keys\\prod.keys")) return Missing(L"a prod.keys do Switch", shown + L"\\eden\\keys");
        if (!HasMatch(shown + L"\\eden\\nand\\system\\Contents\\registered\\*.nca"))
            return Missing(L"o firmware do Switch", shown + L"\\eden\\nand\\system\\Contents\\registered");
    }
    // The PS3 firmware is the one Sony publishes on its site; RPCS3 installs it (File > Install Firmware).
    const std::wstring vsh = L"\\dev_flash\\vsh\\module\\vsh.self";
    if ((core == L"rpcs3" && !Exists(shown + L"\\rpcs3" + vsh)) || (core.empty() && id == L"PS3" && !Exists(exeDir + vsh)))
        return L"Falta o firmware do PS3. Baixe o PS3UPDAT.PUP no site da Sony e instale pelo RPCS3 (File > Install Firmware).";
    if ((ext == L".wud" || ext == L".wux") &&
        ((core == L"cemu" && !Exists(shown + L"\\Cemu\\keys.txt")) || (core.empty() && id == L"WIIU" && !Exists(exeDir + L"\\keys.txt"))))
        return Missing(L"a chave do seu Wii U para jogos WUD e WUX (keys.txt)", core.empty() ? exeDir : shown + L"\\Cemu");
    if (ext == L".fds" && !core.empty() && !inSystem([](const std::wstring& f) { return Exists(f + L"\\disksys.rom"); }))
        return Missing(L"a BIOS do Famicom Disk System (disksys.rom)", shown);
    return L"";
}

namespace {

// Sets key in [section] of an INI, TOML or RetroArch .cfg text, keeping every other line, the file's line ending and its
// "key = value" or "key=value" spacing. A key that is not there goes after the last line of its section, and a missing
// section at the end. Returns whether the value changed.
bool SetKey(std::string& text, const std::string& section, const std::string& key, const std::string& value) {
    const std::string newline = text.find("\r\n") != std::string::npos ? "\r\n" : "\n";
    const std::string separator = text.find(" = ") != std::string::npos || text.empty() ? " = " : "=";
    std::vector<std::string> lines;
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        start = end + 1;
    }
    const auto trim = [](std::string value) {
        const size_t first = value.find_first_not_of(" \t");
        if (first == std::string::npos) return std::string();
        return value.substr(first, value.find_last_not_of(" \t") - first + 1);
    };
    std::string current;
    bool sectionFound = section.empty();
    size_t insertAt = section.empty() ? lines.size() : std::string::npos;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string line = trim(lines[i]);
        if (line.size() > 2 && line.front() == '[' && line.back() == ']') {
            current = line.substr(1, line.size() - 2);
            if (current == section) {
                sectionFound = true;
                insertAt = i + 1;
            }
            continue;
        }
        if (current != section) continue;
        if (!line.empty()) insertAt = i + 1;
        const size_t equals = line.find('=');
        if (equals == std::string::npos || trim(line.substr(0, equals)) != key) continue;
        const std::string replaced = key + separator + value;
        if (trim(line.substr(equals + 1)) == value) return false;
        lines[i] = replaced;
        text.clear();
        for (const auto& kept : lines) text += kept + newline;
        return true;
    }
    if (!sectionFound) {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        insertAt = lines.size();
    }
    lines.insert(lines.begin() + static_cast<std::ptrdiff_t>(insertAt), key + separator + value);
    text.clear();
    for (const auto& kept : lines) text += kept + newline;
    return true;
}

}

std::wstring TuneEngine(const std::wstring& root, const std::wstring& id) {
    const Engine engine = LoadEngine(root, id);
    const System* system = FindSystem(id);
    const Preset* preset = system != nullptr ? MatchPreset(*system, engine.exe) : nullptr;
    if (preset == nullptr || preset->tweaks.empty()) return L"O Ranga não tem ajustes para este emulador.";
    if (!Exists(engine.exe)) return engine.problem;
    const std::wstring folder = ParentFolder(engine.exe);
    std::wstring report;
    if (!preset->bios.empty()) {
        CreateDirectoryW((root + L"\\bios").c_str(), nullptr);
        CreateDirectoryW((root + L"\\bios\\" + preset->bios).c_str(), nullptr);
    }
    std::wstring file;
    std::string text;
    int changed = 0;
    const auto flush = [&] {
        if (file.empty() || changed == 0) return;
        FILE* out = nullptr;
        if (_wfopen_s(&out, (folder + L"\\" + file).c_str(), L"wb") != 0 || out == nullptr) {
            report += L"FALHOU ao gravar " + file + L"\r\n";
            return;
        }
        fwrite(text.data(), 1, text.size(), out);
        fclose(out);
        report += file + L": " + std::to_wstring(changed) + L" ajuste(s)\r\n";
    };
    for (const auto& tweak : preset->tweaks) {
        if (tweak.file != file) {
            flush();
            file = tweak.file;
            text = ReadBytes(folder + L"\\" + file);
            changed = 0;
            // The emulator makes its folders on first start; one never opened gets them here.
            for (size_t slash = file.find(L'\\'); slash != std::wstring::npos; slash = file.find(L'\\', slash + 1)) CreateDirectoryW((folder + L"\\" + file.substr(0, slash)).c_str(), nullptr);
        }
        std::string value = tweak.value;
        if (const size_t at = value.find("{root}"); at != std::string::npos) value.replace(at, 6, Utf8(root));
        if (SetKey(text, tweak.section, tweak.key, value)) ++changed;
    }
    flush();
    if (report.empty()) report = L"Já estava ajustado.\r\n";
    if (!preset->bios.empty()) report += L"BIOS: coloque em " + root + L"\\bios\\" + preset->bios + L"\r\n";
    return report;
}

Detected DetectFile(const std::wstring& path) {
    Detected found;
    const std::wstring extension = Extension(path);
    found.title = FileName(path);
    if (const size_t dot = found.title.find_last_of(L'.'); dot != std::wstring::npos) found.title.resize(dot);
    const std::wstring stem = found.title;
    for (const auto& system : Systems()) {
        if (std::find(system.extensions.begin(), system.extensions.end(), extension) != system.extensions.end()) found.systems.push_back(system.id);
    }
    // The contents decide when the extension alone fits several consoles; a disc image read anyway gives its title.
    if (found.systems.empty() || (found.systems.size() == 1 && extension != L".wbfs")) return found;
    Detected disc;
    bool known = false;
    if (extension == L".cue" || extension == L".m3u") {
        const std::wstring track = FirstListedFile(path, extension == L".cue");
        const std::wstring inner = Extension(track);
        if (inner == L".cue") known = DetectDisc(FirstListedFile(track, true), disc);
        if (inner == L".chd") disc.systems = ChdSystems(track);
        else if (inner == L".gdi" || inner == L".cdi") disc.systems = {L"DC"};
        else if (inner != L".cue" && !track.empty()) known = DetectDisc(track, disc);
        if (!disc.systems.empty()) known = true;
    } else if (extension == L".chd") {
        disc.systems = ChdSystems(path);
        known = true;
    } else if (extension == L".pbp") {
        // A PBP holds a PS1 disc when its PSAR section starts with PSISOIMG (one disc) or PSTITLEIMG (several);
        // otherwise it is a PSP program.
        Reader reader(path);
        const std::string header = reader.At(0, 0x28);
        if (header.size() == 0x28 && header.compare(0, 4, std::string("\0PBP", 4)) == 0) {
            const std::string psar = reader.At(Le32(header, 0x24), 12);
            disc.systems = {psar.rfind("PSISOIMG", 0) == 0 || psar.rfind("PSTITLEIMG", 0) == 0 ? L"PS1" : L"PSP"};
            known = true;
        }
    } else if (extension == L".rvz" || extension == L".wia" || extension == L".gcz" || extension == L".ciso" || extension == L".gcm" || extension == L".wbfs") {
        known = DetectNintendoDisc(path, extension, disc);
    } else if (extension == L".iso" || extension == L".bin" || extension == L".img") {
        known = DetectNintendoDisc(path, extension, disc) || DetectDisc(path, disc);
        // A .bin that is no disc at all is a Mega Drive cartridge.
        if (!known && extension == L".bin") {
            disc.systems = {L"MD"};
            known = true;
        }
    }
    if (!known) return found;
    // Only consoles that take this extension count, so a disc never lands on a console that cannot open the file.
    std::vector<std::wstring> systems;
    for (const auto& id : disc.systems) {
        if (std::find(found.systems.begin(), found.systems.end(), id) != found.systems.end()) systems.push_back(id);
    }
    if (systems.empty()) return found;
    found.systems = systems;
    found.serial = disc.serial;
    if (!disc.title.empty()) found.title = disc.title;
    if (found.title.empty()) found.title = stem;
    return found;
}

Detected DetectFolder(const std::wstring& folder) {
    Detected found;
    found.title = FileName(folder);
    for (const wchar_t* sfoPath : {L"\\PS3_GAME\\PARAM.SFO", L"\\PARAM.SFO"}) {
        const std::string sfo = ReadBytes(folder + sfoPath);
        if (sfo.empty() || GameRom(L"PS3", folder).empty()) continue;
        found.systems = {L"PS3"};
        found.serial = Wide(SfoString(sfo, "TITLE_ID"));
        if (const std::string title = SfoString(sfo, "TITLE"); !title.empty()) found.title = Wide(title);
        return found;
    }
    if (Exists(folder + L"\\default.xex")) {
        found.systems = {L"X360"};
        return found;
    }
    if (!GameRom(L"WIIU", folder).empty()) {
        found.systems = {L"WIIU"};
        const std::string meta = ReadBytes(folder + L"\\meta\\meta.xml");
        const size_t open = meta.find("<longname_en");
        const size_t start = open == std::string::npos ? open : meta.find('>', open);
        const size_t end = start == std::string::npos ? start : meta.find('<', start);
        if (end != std::string::npos) {
            std::wstring title = Wide(meta.substr(start + 1, end - start - 1));
            std::replace(title.begin(), title.end(), L'\n', L' ');
            if (!title.empty()) found.title = title;
        }
        return found;
    }
    return found;
}

std::wstring GameRom(const std::wstring& system, const std::wstring& path) {
    if (!IsDirectory(path)) return Exists(path) ? path : L"";
    if (system == L"PS3") {
        for (const wchar_t* eboot : {L"\\PS3_GAME\\USRDIR\\EBOOT.BIN", L"\\USRDIR\\EBOOT.BIN"}) {
            if (Exists(path + eboot)) return path + eboot;
        }
        return L"";
    }
    if (system == L"X360") return Exists(path + L"\\default.xex") ? path + L"\\default.xex" : L"";
    if (system == L"WIIU") return FirstMatch(path + L"\\code", L"*.rpx");
    return L"";
}

std::vector<std::pair<std::wstring, std::wstring>> GameFileFilters() {
    std::vector<std::pair<std::wstring, std::wstring>> filters;
    std::wstring all;
    for (const auto& system : Systems()) {
        std::wstring patterns;
        for (const auto& extension : system.extensions) {
            patterns += (patterns.empty() ? L"*" : L";*") + extension;
            if (all.find(L"*" + extension + L";") == std::wstring::npos) all += L"*" + extension + L";";
        }
        if (!patterns.empty()) filters.push_back({Wide(system.name), patterns});
    }
    if (!all.empty()) all.pop_back();
    filters.insert(filters.begin(), {L"Jogos de todos os consoles", all});
    filters.push_back({L"Todos os arquivos", L"*.*"});
    return filters;
}

void ExtractArt(const std::wstring& system, const std::wstring& path, const std::wstring& folder) {
    if (system != L"PSP" || IsDirectory(path)) return;
    Iso iso(path);
    if (!iso.Valid()) return;
    // folder is games\<key>\art; the workspace above it may not exist yet.
    CreateDirectoryW(ParentFolder(ParentFolder(folder)).c_str(), nullptr);
    CreateDirectoryW(ParentFolder(folder).c_str(), nullptr);
    CreateDirectoryW(folder.c_str(), nullptr);
    for (const char* name : {"ICON0.PNG", "PIC1.PNG"}) {
        const std::string data = iso.ReadFile(std::string("PSP_GAME/") + name, 8 << 20);
        if (data.empty()) continue;
        FILE* file = nullptr;
        if (_wfopen_s(&file, (folder + L"\\" + Wide(name)).c_str(), L"wb") != 0 || file == nullptr) continue;
        fwrite(data.data(), 1, data.size(), file);
        fclose(file);
    }
}

std::wstring FileKey(const std::wstring& system, const std::wstring& path) { return system + L"-" + Fnv(path); }

SwitchContent DetectSwitchContent(const std::wstring& path) {
    const auto isHex = [](const std::wstring& text, size_t from, size_t count) {
        if (from + count > text.size()) return false;
        for (size_t i = from; i < from + count; ++i) if (!iswxdigit(text[i])) return false;
        return true;
    };
    std::wstring id;
    // An NSP is a PFS0: a count of files, the size of the name table, 24 bytes per file, then the names. The ticket is
    // named after its rights id, which starts with the title id.
    Reader reader(path);
    const std::string header = reader.At(0, 16);
    if (header.size() == 16 && header.compare(0, 4, "PFS0") == 0) {
        const unsigned count = Le32(header, 4), names = Le32(header, 8);
        if (count <= 8192 && names <= (1u << 20)) {
            const std::string table = reader.At(16 + static_cast<long long>(count) * 24, names);
            for (size_t start = 0; start < table.size() && id.empty();) {
                const size_t end = std::min(table.find('\0', start), table.size());
                const std::wstring name = Wide(table.substr(start, end - start));
                if (name.size() == 36 && Lower(name).compare(32, 4, L".tik") == 0 && isHex(name, 0, 32)) id = name.substr(0, 16);
                start = end + 1;
            }
        }
    }
    const std::wstring name = FileName(path);
    for (size_t at = name.find(L'['); at != std::wstring::npos && id.empty(); at = name.find(L'[', at + 1)) {
        if (at + 17 < name.size() && name[at + 17] == L']' && name[at + 1] == L'0' && name[at + 2] == L'1' && isHex(name, at + 1, 16)) id = name.substr(at + 1, 16);
    }
    SwitchContent content;
    if (!id.empty()) {
        for (auto& character : id) character = static_cast<wchar_t>(towupper(character));
        const unsigned long long value = _wcstoui64(id.c_str(), nullptr, 16);
        const unsigned low = static_cast<unsigned>(value & 0xFFF);
        content.kind = low == 0 ? SwitchKind::Game : low == 0x800 ? SwitchKind::Update : SwitchKind::Addon;
        const unsigned long long base = content.kind == SwitchKind::Addon ? (value & ~0xFFFULL) - 0x1000 : value & ~0xFFFULL;
        wchar_t text[17];
        swprintf(text, 17, L"%016llX", base);
        content.titleId = id;
        content.baseId = text;
        return content;
    }
    const std::wstring lower = Lower(name);
    if (lower.find(L"update") != std::wstring::npos || lower.find(L"[upd]") != std::wstring::npos) content.kind = SwitchKind::Update;
    else if (lower.find(L"dlc") != std::wstring::npos) content.kind = SwitchKind::Addon;
    return content;
}

}
