#include "Systems.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cwchar>

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
    const char* bios = "Precisa da BIOS tirada do seu console, configurada no emulador.";
    return {
        {L"PS4", "PlayStation 4", {}, true,
         {{L"shadPS4", L"shadps4", L"{fullscreen} -g \"{rom}\"", L"--fullscreen true", {}, L"", true}},
         "Aponte o shadPS4.exe (o emulador), não o shadPS4QtLauncher. O Bloodborne continua no port nativo do Ranga. A pasta do jogo é a que tem eboot.bin e sce_sys."},
        {L"PS3", "PlayStation 3", {L".iso"}, true,
         {{L"RPCS3", L"rpcs3", L"--no-gui {fullscreen} \"{rom}\"", L"--fullscreen",
           {{L"GuiConfigs\\CurrentSettings.ini", "main_window", "infoBoxEnabledWelcome", "false"}}, L"", true}},
         "Instale no RPCS3 o firmware do PS3 (PS3UPDAT.PUP, do site da Sony). Jogo em pasta (com PS3_GAME ou USRDIR) ou ISO."},
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
         bios},
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
         bios},
        {L"PSP", "PSP", {L".iso", L".cso", L".chd", L".pbp", L".elf", L".prx"}, false,
         {{L"PPSSPP", L"ppsspp", L"{fullscreen} \"{rom}\"", L"--fullscreen",
           {{L"memstick\\PSP\\SYSTEM\\ppsspp.ini", "Graphics", "GraphicsBackend", "3 (VULKAN)"},
            {L"memstick\\PSP\\SYSTEM\\ppsspp.ini", "Graphics", "InternalResolution", "4"}}},
          RetroArch(L"ppsspp")},
         "Não precisa de BIOS."},
        {L"SWITCH", "Nintendo Switch", {L".nsp", L".xci", L".nca", L".nro", L".nso"}, false,
         {{L"Ryujinx", L"ryujinx", L"{fullscreen} \"{rom}\"", L"--fullscreen", {}, L"", true},
          {L"Eden / Citron (família yuzu)", L"eden;citron;sudachi;suyu;yuzu;torzu", L"{fullscreen} -g \"{rom}\"", L"-f", {}, L"", true}},
         "Precisa das prod.keys e do firmware tirados do seu Switch, instalados no emulador."},
        {L"WIIU", "Wii U", {L".wua", L".wud", L".wux", L".rpx", L".wuhb"}, true,
         {{L"Cemu", L"cemu", L"{fullscreen} -g \"{rom}\"", L"-f", {}, L"", true}},
         "WUD e WUX precisam das chaves do seu console no keys.txt do Cemu. Jogo em pasta: a que tem code, content e meta."},
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
         bios},
        {L"SATURN", "Sega Saturn", {L".cue", L".chd", L".ccd", L".mds", L".m3u"}, false,
         {RetroArch(L"mednafen_saturn", L"retroarch"), RetroArch(L"yabasanshiro", L"retroarch")},
         bios},
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
         "O xemu precisa da BIOS, da MCPX e do HD do seu Xbox. O jogo tem que ser XISO."},
        {L"X360", "Xbox 360", {L".iso", L".xex", L".zar"}, true,
         {{L"Xenia Canary", L"xenia", L"{fullscreen} \"{rom}\"", L"--fullscreen=true", {}, L"", true}},
         "Jogo em ISO, ou pasta com default.xex."},
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

Engine LoadEngine(const std::wstring& root, const std::wstring& id) {
    Engine engine;
    const System* system = FindSystem(id);
    if (system == nullptr) {
        engine.problem = L"O Ranga não conhece o console " + id + L".";
        return engine;
    }
    const std::wstring ini = EnginesIni(root);
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

std::wstring EngineCommand(const Engine& engine, const std::wstring& rom, bool fullscreen) {
    const std::wstring args = Expand(engine.args, engine.exe, rom, fullscreen ? engine.fullscreen : L"");
    return L"\"" + engine.exe + L"\"" + (args.empty() ? L"" : L" " + args);
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
    if (system == L"PS4") return Exists(path + L"\\eboot.bin") ? path + L"\\eboot.bin" : L"";
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

}
