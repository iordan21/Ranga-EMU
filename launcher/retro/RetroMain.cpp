// Ranga Retro: runs a libretro core inside a window of Ranga's own, so a console a core covers plays in Ranga itself
// rather than in another emulator. Direct3D 11 draws the core's frames, SDL2 plays its sound and reads the gamepads
// (DualSense, Xbox and generic ones alike, as a RetroPad), and its saves and states go to the game's workspace.
//
// ranga_retro.exe --core <core.dll> --rom <game> --title <name> --system <bios folder> --saves <folder>
//                 [--options <core options file>] [--fullscreen]
//
// Keys: F11 fullscreen, F5 save state, F7 load state. Ranga's client starts it; it can also be run by hand.
#define SDL_MAIN_HANDLED
#include <SDL.h>

#include "libretro.h"
#include "RangaRetroExt.h"
#include "VulkanVideo.hpp"
#include "Netplay.hpp"
#include "Achievements.hpp"

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <dwmapi.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring Wide(const std::string& text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

std::vector<unsigned char> ReadFile(const std::wstring& path) {
    std::vector<unsigned char> data;
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return data;
    _fseeki64(file, 0, SEEK_END);
    data.resize(static_cast<size_t>(_ftelli64(file)));
    _fseeki64(file, 0, SEEK_SET);
    data.resize(fread(data.data(), 1, data.size(), file));
    fclose(file);
    return data;
}

bool WriteFile(const std::wstring& path, const void* data, size_t size) {
    // Written beside and then moved over, so a crash in the middle never leaves half a save.
    const std::wstring temporary = path + L".tmp";
    FILE* file = nullptr;
    if (_wfopen_s(&file, temporary.c_str(), L"wb") != 0 || file == nullptr) return false;
    const bool written = fwrite(data, 1, size, file) == size;
    fclose(file);
    return written && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}

void Log(const char* format, ...) {
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------------------------------------------- core

struct Core {
    HMODULE library = nullptr;
    void (*init)() = nullptr;
    void (*deinit)() = nullptr;
    void (*getSystemInfo)(retro_system_info*) = nullptr;
    void (*getAvInfo)(retro_system_av_info*) = nullptr;
    void (*setEnvironment)(retro_environment_t) = nullptr;
    void (*setVideo)(retro_video_refresh_t) = nullptr;
    void (*setAudio)(retro_audio_sample_t) = nullptr;
    void (*setAudioBatch)(retro_audio_sample_batch_t) = nullptr;
    void (*setInputPoll)(retro_input_poll_t) = nullptr;
    void (*setInputState)(retro_input_state_t) = nullptr;
    void (*setControllerPort)(unsigned, unsigned) = nullptr;
    void (*run)() = nullptr;
    bool (*loadGame)(const retro_game_info*) = nullptr;
    void (*unloadGame)() = nullptr;
    size_t (*serializeSize)() = nullptr;
    bool (*serialize)(void*, size_t) = nullptr;
    bool (*unserialize)(const void*, size_t) = nullptr;
    void (*reset)() = nullptr;
    void* (*memoryData)(unsigned) = nullptr;
    size_t (*memorySize)(unsigned) = nullptr;

    template <typename T> bool Bind(T& function, const char* name) {
        function = reinterpret_cast<T>(reinterpret_cast<void*>(GetProcAddress(library, name)));
        if (function == nullptr) Log("[retro] o core não tem %s\n", name);
        return function != nullptr;
    }

    bool Load(const std::wstring& path) {
        library = LoadLibraryW(path.c_str());
        if (library == nullptr) return false;
        return Bind(init, "retro_init") & Bind(deinit, "retro_deinit") & Bind(getSystemInfo, "retro_get_system_info") &
               Bind(getAvInfo, "retro_get_system_av_info") & Bind(setEnvironment, "retro_set_environment") &
               Bind(setVideo, "retro_set_video_refresh") & Bind(setAudio, "retro_set_audio_sample") &
               Bind(setAudioBatch, "retro_set_audio_sample_batch") & Bind(setInputPoll, "retro_set_input_poll") &
               Bind(setInputState, "retro_set_input_state") & Bind(setControllerPort, "retro_set_controller_port_device") &
               Bind(run, "retro_run") & Bind(loadGame, "retro_load_game") & Bind(unloadGame, "retro_unload_game") &
               Bind(serializeSize, "retro_serialize_size") & Bind(serialize, "retro_serialize") &
               Bind(unserialize, "retro_unserialize") & Bind(reset, "retro_reset") & Bind(memoryData, "retro_get_memory_data") & Bind(memorySize, "retro_get_memory_size");
    }
};

// Everything the callbacks reach; the libretro API has no user pointer, so it is one global.
struct State {
    Core core;
    std::string systemDir, saveDir;
    retro_pixel_format pixelFormat = RETRO_PIXEL_FORMAT_0RGB1555;
    retro_system_av_info av{};
    // Core options: the core's defaults, then what the options file sets.
    std::map<std::string, std::string> options, overrides;
    // What the in-game menu shows of each option: its description and its values with their labels, in the core's order.
    struct OptionInfo {
        std::string description;
        std::vector<std::pair<std::string, std::string>> values;
    };
    std::map<std::string, OptionInfo> optionInfo;
    std::vector<std::string> optionOrder;
    std::wstring optionsPath;
    bool optionsChanged = false;
    // The core said it can start without a game (SET_SUPPORT_NO_GAME); --no-game is refused otherwise.
    bool supportsNoGame = false;
    retro_frame_time_callback frameTime{};
    // The latest frame, as BGRA.
    std::vector<uint32_t> frame;
    unsigned frameWidth = 0, frameHeight = 0;
    bool frameNew = false;
    std::vector<int16_t> audio;
    std::wstring message;
    ULONGLONG messageUntil = 0;
} g;

uintptr_t RETRO_CALLCONV HwCurrentFramebuffer();
retro_proc_address_t RETRO_CALLCONV HwProcAddress(const char* name);
extern retro_hw_render_callback g_hw;
extern bool g_hwActive;
// Vulkan is offered first (--gpu opengl offers OpenGL instead); a core that draws with Vulkan gets RetroVulkan.
bool g_preferVulkan = true, g_hwVulkan = false;
// The Qualidade chosen for this PC (--quality): 0 ruim, 1 fraco, 2 bom, 3 ótimo.
int g_quality = 2;
const retro_hw_render_context_negotiation_interface_vulkan* g_negotiation = nullptr;
unsigned g_hwFrameWidth = 0, g_hwFrameHeight = 0;
// A core that draws in the game window itself (RANGA_ENVIRONMENT_GET_WINDOW), and the frame rate it reports.
bool g_coreDraws = false;
float g_coreFps = -1;

// ------------------------------------------------------------------------------------------------------------- input

constexpr int kPorts = 2;
// The core is xemu, whose players come on other ports (InputState).
bool g_xemuPorts = false;
SDL_GameController* g_pads[kPorts] = {};
HWND g_window = nullptr;

void OpenPads() {
    for (int i = 0; i < SDL_NumJoysticks(); ++i) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
        bool open = false;
        for (auto* pad : g_pads) open |= pad != nullptr && SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id;
        if (open) continue;
        for (auto*& pad : g_pads) {
            if (pad != nullptr) continue;
            pad = SDL_GameControllerOpen(i);
            if (pad != nullptr) Log("[retro] controle: %s\n", SDL_GameControllerName(pad));
            break;
        }
    }
}

void ClosePad(SDL_JoystickID id) {
    for (auto*& pad : g_pads) {
        if (pad == nullptr || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) != id) continue;
        SDL_GameControllerClose(pad);
        pad = nullptr;
    }
}

bool Key(int virtualKey) { return (GetAsyncKeyState(virtualKey) & 0x8000) != 0; }

// RANGA_RETRO_PAD_SCRIPT="4000:A,6500:DOWN": player 1's buttons pressed by the clock (milliseconds since the program
// started, 200 ms each), so a test drives a game or a console's menu without the real keyboard or a window in front.
// Names: A B X Y UP DOWN LEFT RIGHT START SELECT L R.
const ULONGLONG g_started = GetTickCount64();
// With a script, it alone plays player 1: a real pad or keyboard in use meanwhile would change what the test sees.
bool g_scripted = false;
bool ScriptedButton(unsigned id) {
    static const std::vector<std::pair<ULONGLONG, unsigned>> script = [] {
        std::vector<std::pair<ULONGLONG, unsigned>> steps;
        const char* text = std::getenv("RANGA_RETRO_PAD_SCRIPT");
        if (text == nullptr) return steps;
        static const std::pair<const char*, unsigned> names[] = {
            {"A", RETRO_DEVICE_ID_JOYPAD_A}, {"B", RETRO_DEVICE_ID_JOYPAD_B}, {"X", RETRO_DEVICE_ID_JOYPAD_X},
            {"Y", RETRO_DEVICE_ID_JOYPAD_Y}, {"UP", RETRO_DEVICE_ID_JOYPAD_UP}, {"DOWN", RETRO_DEVICE_ID_JOYPAD_DOWN},
            {"LEFT", RETRO_DEVICE_ID_JOYPAD_LEFT}, {"RIGHT", RETRO_DEVICE_ID_JOYPAD_RIGHT}, {"START", RETRO_DEVICE_ID_JOYPAD_START},
            {"SELECT", RETRO_DEVICE_ID_JOYPAD_SELECT}, {"L", RETRO_DEVICE_ID_JOYPAD_L}, {"R", RETRO_DEVICE_ID_JOYPAD_R},
        };
        std::string all = text;
        for (size_t at = 0; at < all.size();) {
            size_t end = all.find(',', at);
            if (end == std::string::npos) end = all.size();
            const std::string step = all.substr(at, end - at);
            at = end + 1;
            const size_t colon = step.find(':');
            if (colon == std::string::npos) continue;
            for (const auto& [name, button] : names) {
                if (step.substr(colon + 1) == name) steps.push_back({std::strtoull(step.c_str(), nullptr, 10), button});
            }
        }
        return steps;
    }();
    const ULONGLONG now = GetTickCount64() - g_started;
    g_scripted = !script.empty();
    for (const auto& [at, button] : script) {
        if (button == id && now >= at && now < at + 200) return true;
    }
    return false;
}

// Which button of the pad plays each RetroPad button, by SDL's own names ("a", "lefttrigger", "dpup"...); empty is
// none. The RetroPad follows the Super Nintendo (B below, A right, Y left, X above), which is where SDL's south, east,
// west and north buttons are on every pad it knows. The console's controls file (--controls, written by Ranga's
// Controle tab) changes any of them.
const char* const kRetroNames[] = {"B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R", "L2", "R2", "L3", "R3"};
std::string g_padMap[16] = {"a", "x", "back", "start", "dpup", "dpdown", "dpleft", "dpright", "b", "y",
                            "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick", "rightstick"};

void ReadControls(const std::wstring& path) {
    const auto data = ReadFile(path);
    std::string text(data.begin(), data.end());
    for (size_t start = 0; start < text.size();) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || equals == std::string::npos) continue;
        std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        for (int b = 0; b < 16; ++b) {
            if (key == kRetroNames[b]) g_padMap[b] = value;
        }
    }
    std::string summary;
    for (int b = 0; b < 16; ++b) summary += std::string(b ? " " : "") + kRetroNames[b] + "=" + (g_padMap[b].empty() ? "-" : g_padMap[b]);
    Log("[retro] botões do controle: %s\n", summary.c_str());
}

// How far the pad's source for a RetroPad button is pressed, 0 to 0x7fff: a trigger by its travel, a button all or
// nothing.
int PadSource(SDL_GameController* pad, unsigned id) {
    if (pad == nullptr || id >= 16 || g_padMap[id].empty()) return 0;
    const SDL_GameControllerAxis axis = SDL_GameControllerGetAxisFromString(g_padMap[id].c_str());
    if (axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT || axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return std::max<int>(0, SDL_GameControllerGetAxis(pad, axis));
    const SDL_GameControllerButton button = SDL_GameControllerGetButtonFromString(g_padMap[id].c_str());
    return button != SDL_CONTROLLER_BUTTON_INVALID && SDL_GameControllerGetButton(pad, button) ? 0x7fff : 0;
}

int16_t PadButton(int port, unsigned id) {
    if (port == 0 && ScriptedButton(id)) return 1;
    if (g_scripted) return 0;
    // The game reads input only while its window is the one in front.
    if (GetForegroundWindow() != g_window) return 0;
    bool down = PadSource(g_pads[port], id) > 12000;
    // Player 1 also plays on the keyboard: arrows, Z X A S for B A Y X, Q W for L R, Enter Start, Backspace Select.
    if (!down && port == 0) {
        switch (id) {
            case RETRO_DEVICE_ID_JOYPAD_B: down = Key('Z'); break;
            case RETRO_DEVICE_ID_JOYPAD_A: down = Key('X'); break;
            case RETRO_DEVICE_ID_JOYPAD_Y: down = Key('A'); break;
            case RETRO_DEVICE_ID_JOYPAD_X: down = Key('S'); break;
            case RETRO_DEVICE_ID_JOYPAD_SELECT: down = Key(VK_BACK); break;
            case RETRO_DEVICE_ID_JOYPAD_START: down = Key(VK_RETURN); break;
            case RETRO_DEVICE_ID_JOYPAD_UP: down = Key(VK_UP); break;
            case RETRO_DEVICE_ID_JOYPAD_DOWN: down = Key(VK_DOWN); break;
            case RETRO_DEVICE_ID_JOYPAD_LEFT: down = Key(VK_LEFT); break;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT: down = Key(VK_RIGHT); break;
            case RETRO_DEVICE_ID_JOYPAD_L: down = Key('Q'); break;
            case RETRO_DEVICE_ID_JOYPAD_R: down = Key('W'); break;
        }
    }
    return down ? 1 : 0;
}

int16_t PadAnalog(int port, unsigned index, unsigned id) {
    SDL_GameController* pad = g_pads[port];
    if (pad == nullptr || GetForegroundWindow() != g_window || g_scripted) return 0;
    if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
        // A trigger keeps its travel wherever it was placed; a button is all or nothing.
        if (id == RETRO_DEVICE_ID_JOYPAD_L2 || id == RETRO_DEVICE_ID_JOYPAD_R2) return static_cast<int16_t>(PadSource(pad, id));
        return PadButton(port, id) ? 0x7fff : 0;
    }
    const bool left = index == RETRO_DEVICE_INDEX_ANALOG_LEFT;
    const SDL_GameControllerAxis axis = id == RETRO_DEVICE_ID_ANALOG_X ? (left ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX)
                                                                     : (left ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY);
    return SDL_GameControllerGetAxis(pad, axis);
}

void RETRO_CALLCONV InputPoll() {}

Netplay::Input LocalInput() {
    Netplay::Input input;
    for (unsigned button = 0; button <= RETRO_DEVICE_ID_JOYPAD_R3; ++button) input.mask |= static_cast<uint16_t>(PadButton(0, button) ? 1u << button : 0u);
    input.axis[0] = PadAnalog(0, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X);
    input.axis[1] = PadAnalog(0, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y);
    input.axis[2] = PadAnalog(0, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X);
    input.axis[3] = PadAnalog(0, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y);
    return input;
}

int16_t RETRO_CALLCONV InputState(unsigned port, unsigned device, unsigned index, unsigned id) {
    // RANGA_RETRO_DEBUG=1: each kind of question the core asks about the controls, once.
    static const bool debug = std::getenv("RANGA_RETRO_DEBUG") != nullptr;
    if (debug) {
        static std::set<unsigned> asked;
        if (asked.insert((port << 24) | (device << 16) | (index << 12) | id).second)
            Log("[retro] o core perguntou: porta %u, aparelho %u, índice %u, botão %u\n", port, device, index, id);
    }
    // The xemu core asks for player 1 as port 2 and player 2 as port 3 (the Xbox's USB port less one, xemu's
    // port_map { 3, 4, 1, 2 }), so its dashboard and games saw no controller on ports 0 and 1.
    if (g_xemuPorts) port = (port + 2) % 4;
    if (Netplay::Active()) {
        const Netplay::Input& input = Netplay::InputOf(port);
        switch (device & RETRO_DEVICE_MASK) {
            case RETRO_DEVICE_JOYPAD:
                if (id == RETRO_DEVICE_ID_JOYPAD_MASK) return static_cast<int16_t>(input.mask);
                return id < 16 ? (input.mask >> id) & 1 : 0;
            case RETRO_DEVICE_ANALOG:
                if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) return id < 16 && ((input.mask >> id) & 1) ? 0x7fff : 0;
                if (index > RETRO_DEVICE_INDEX_ANALOG_RIGHT || id > RETRO_DEVICE_ID_ANALOG_Y) return 0;
                return input.axis[index * 2 + id];
        }
        return 0;
    }
    if (port >= kPorts) return 0;
    switch (device & RETRO_DEVICE_MASK) {
        case RETRO_DEVICE_JOYPAD:
            if (id == RETRO_DEVICE_ID_JOYPAD_MASK) {
                int16_t mask = 0;
                for (unsigned button = 0; button <= RETRO_DEVICE_ID_JOYPAD_R3; ++button) mask |= static_cast<int16_t>(PadButton(static_cast<int>(port), button) << button);
                return mask;
            }
            return PadButton(static_cast<int>(port), id);
        case RETRO_DEVICE_ANALOG:
            return PadAnalog(static_cast<int>(port), index, id);
    }
    return 0;
}

bool RETRO_CALLCONV SetRumble(unsigned port, retro_rumble_effect effect, uint16_t strength) {
    static uint16_t strong[kPorts], weak[kPorts];
    if (port >= kPorts || g_pads[port] == nullptr) return false;
    (effect == RETRO_RUMBLE_STRONG ? strong : weak)[port] = strength;
    SDL_GameControllerRumble(g_pads[port], strong[port], weak[port], strong[port] || weak[port] ? 5000 : 0);
    return true;
}

// ------------------------------------------------------------------------------------------------------- environment

void RETRO_CALLCONV CoreLog(retro_log_level level, const char* format, ...) {
    static const char* names[] = {"debug", "info", "aviso", "erro"};
    static const bool debug = std::getenv("RANGA_RETRO_DEBUG") != nullptr;
    if (level == RETRO_LOG_DEBUG && !debug) return;
    std::fprintf(stderr, "[core %s] ", names[std::min<int>(level, 3)]);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

template <typename Value> void DescribeOption(const char* key, const char* description, const Value* values) {
    if (key == nullptr) return;
    auto& info = g.optionInfo[key];
    if (info.description.empty() && info.values.empty()) g.optionOrder.push_back(key);
    info.description = description != nullptr ? description : "";
    info.values.clear();
    for (const Value* value = values; value->value != nullptr; ++value) info.values.push_back({value->value, value->label != nullptr ? value->label : ""});
}

void SetOption(const char* key, const char* defaultValue) {
    if (key == nullptr) return;
    const auto found = g.overrides.find(key);
    g.options[key] = found != g.overrides.end() ? found->second : defaultValue != nullptr ? defaultValue : "";
}

// The box a core's keyboard request opens (RANGA_ENVIRONMENT_GET_TEXT); defined with the menu, below.
bool AskText(ranga_text_request& request);

void ShowMessage(const std::wstring& text, unsigned frames) {
    g.message = text;
    g.messageUntil = GetTickCount64() + std::max(1500u, frames * 17u);
}

bool RETRO_CALLCONV Environment(unsigned command, void* data) {
    switch (command) {
        case RETRO_ENVIRONMENT_GET_CAN_DUPE:
            *static_cast<bool*>(data) = true;
            return true;
        case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT: {
            const auto format = *static_cast<const retro_pixel_format*>(data);
            if (format > RETRO_PIXEL_FORMAT_RGB565) return false;
            g.pixelFormat = format;
            return true;
        }
        case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
        case RETRO_ENVIRONMENT_GET_CORE_ASSETS_DIRECTORY:
            *static_cast<const char**>(data) = g.systemDir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
            *static_cast<const char**>(data) = g.saveDir.c_str();
            return true;
        case RETRO_ENVIRONMENT_GET_LOG_INTERFACE:
            static_cast<retro_log_callback*>(data)->log = CoreLog;
            return true;
        case RETRO_ENVIRONMENT_GET_LANGUAGE:
            *static_cast<unsigned*>(data) = RETRO_LANGUAGE_PORTUGUESE_BRAZIL;
            return true;
        case RETRO_ENVIRONMENT_GET_USERNAME:
            *static_cast<const char**>(data) = "Ranga";
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
            return true;
        case RETRO_ENVIRONMENT_GET_INPUT_MAX_USERS:
            *static_cast<unsigned*>(data) = kPorts;
            return true;
        case RETRO_ENVIRONMENT_GET_AUDIO_VIDEO_ENABLE:
            if (data != nullptr) *static_cast<int*>(data) = 3;
            return true;
        case RETRO_ENVIRONMENT_GET_FASTFORWARDING:
            if (data != nullptr) *static_cast<bool*>(data) = false;
            return true;
        case RETRO_ENVIRONMENT_GET_MESSAGE_INTERFACE_VERSION:
            *static_cast<unsigned*>(data) = 0;
            return true;
        case RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE:
            static_cast<retro_rumble_interface*>(data)->set_rumble_state = SetRumble;
            return true;
        case RETRO_ENVIRONMENT_SET_MESSAGE: {
            const auto* message = static_cast<const retro_message*>(data);
            Log("[core] %s\n", message->msg);
            ShowMessage(Wide(message->msg), message->frames);
            return true;
        }
        case RETRO_ENVIRONMENT_SET_GEOMETRY:
            g.av.geometry = *static_cast<const retro_game_geometry*>(data);
            return true;
        case RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO:
            g.av = *static_cast<const retro_system_av_info*>(data);
            Log("[retro] o core mudou para %ux%u, %.3f fps, som %.0f Hz\n", g.av.geometry.base_width, g.av.geometry.base_height, g.av.timing.fps, g.av.timing.sample_rate);
            return true;
        case RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK:
            g.frameTime = *static_cast<const retro_frame_time_callback*>(data);
            return true;

        // Core options. The client answers with the newest format it reads, and every format comes down to a key and its
        // default value, which the options file can override.
        case RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION:
            *static_cast<unsigned*>(data) = 2;
            return true;
        case RETRO_ENVIRONMENT_SET_VARIABLES:
            for (const auto* variable = static_cast<const retro_variable*>(data); variable->key != nullptr; ++variable) {
                // "Description; first|second|third": the first value is the default.
                std::string values = variable->value != nullptr ? variable->value : "";
                const size_t semicolon = values.find("; ");
                const std::string description = semicolon == std::string::npos ? "" : values.substr(0, semicolon);
                values = semicolon == std::string::npos ? values : values.substr(semicolon + 2);
                SetOption(variable->key, values.substr(0, values.find('|')).c_str());
                auto& info = g.optionInfo[variable->key];
                if (info.values.empty()) g.optionOrder.push_back(variable->key);
                info.description = description;
                for (size_t start = 0; start <= values.size();) {
                    size_t end = values.find('|', start);
                    if (end == std::string::npos) end = values.size();
                    info.values.push_back({values.substr(start, end - start), ""});
                    start = end + 1;
                }
            }
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
            for (const auto* option = static_cast<const retro_core_option_definition*>(data); option->key != nullptr; ++option) {
                SetOption(option->key, option->default_value);
                DescribeOption(option->key, option->desc, option->values);
            }
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
            for (const auto* option = static_cast<const retro_core_options_intl*>(data)->us; option->key != nullptr; ++option) {
                SetOption(option->key, option->default_value);
                DescribeOption(option->key, option->desc, option->values);
            }
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            for (const auto* option = static_cast<const retro_core_options_v2*>(data)->definitions; option->key != nullptr; ++option) {
                SetOption(option->key, option->default_value);
                DescribeOption(option->key, option->desc, option->values);
            }
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
            for (const auto* option = static_cast<const retro_core_options_v2_intl*>(data)->us->definitions; option->key != nullptr; ++option) {
                SetOption(option->key, option->default_value);
                DescribeOption(option->key, option->desc, option->values);
            }
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* variable = static_cast<retro_variable*>(data);
            const std::string key = variable->key != nullptr ? variable->key : "";
            const std::string* value = nullptr;
            if (const auto found = g.options.find(key); found != g.options.end()) value = &found->second;
            // A key the core never listed but the game's options file has (Ranga's own, for the core's online rooms).
            else if (const auto extra = g.overrides.find(key); extra != g.overrides.end()) value = &extra->second;
            variable->value = value != nullptr ? value->c_str() : nullptr;
            return value != nullptr;
        }
        case RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE:
            *static_cast<bool*>(data) = g.optionsChanged;
            g.optionsChanged = false;
            return true;

        // Information the client takes note of without needing it.
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_DISPLAY:
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_UPDATE_DISPLAY_CALLBACK:
        case RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS:
        case RETRO_ENVIRONMENT_SET_CONTROLLER_INFO:
        case RETRO_ENVIRONMENT_SET_SUBSYSTEM_INFO:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
            return true;
        // Where the game's memory is, for RetroAchievements (Achievements.hpp).
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
            Achievements::SetMemoryMap(static_cast<const retro_memory_map*>(data));
            return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
            g.supportsNoGame = data != nullptr && *static_cast<const bool*>(data);
            return true;
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
            return true;

        // The GPU: OpenGL, compatibility or core profile. Vulkan and Direct3D contexts are refused, and a core then
        // falls back to another renderer or says it cannot run.
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            *static_cast<unsigned*>(data) = g_preferVulkan ? RETRO_HW_CONTEXT_VULKAN : RETRO_HW_CONTEXT_OPENGL_CORE;
            return true;
        case RETRO_ENVIRONMENT_GET_HW_RENDER_INTERFACE: {
            const auto* face = g_hwVulkan ? RetroVulkan::Interface() : nullptr;
            *static_cast<const retro_hw_render_interface**>(data) = reinterpret_cast<const retro_hw_render_interface*>(face);
            return face != nullptr;
        }
        case RETRO_ENVIRONMENT_SET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE: {
            const auto* face = static_cast<const retro_hw_render_context_negotiation_interface*>(data);
            if (face->interface_type != RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN) return false;
            g_negotiation = reinterpret_cast<const retro_hw_render_context_negotiation_interface_vulkan*>(data);
            return true;
        }
        case RETRO_ENVIRONMENT_GET_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_SUPPORT: {
            auto* face = static_cast<retro_hw_render_context_negotiation_interface*>(data);
            face->interface_version = face->interface_type == RETRO_HW_RENDER_CONTEXT_NEGOTIATION_INTERFACE_VULKAN ? 2 : 0;
            return true;
        }
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            auto* request = static_cast<retro_hw_render_callback*>(data);
            g_hwVulkan = request->context_type == RETRO_HW_CONTEXT_VULKAN;
            if (g_hwVulkan) {
                g_hw = *request;
                g_hwActive = true;
                Log("[retro] o core desenha com Vulkan\n");
                return true;
            }
            if (request->context_type != RETRO_HW_CONTEXT_OPENGL && request->context_type != RETRO_HW_CONTEXT_OPENGL_CORE) {
                Log("[retro] o core pediu um contexto de GPU que o Ranga Retro não tem (tipo %u)\n", static_cast<unsigned>(request->context_type));
                return false;
            }
            request->get_current_framebuffer = HwCurrentFramebuffer;
            request->get_proc_address = HwProcAddress;
            g_hw = *request;
            g_hwActive = true;
            Log("[retro] o core desenha com OpenGL %s %u.%u\n", request->context_type == RETRO_HW_CONTEXT_OPENGL_CORE ? "core" : "", request->version_major, request->version_minor);
            return true;
        }

        // Ranga's own commands (RangaRetroExt.h).
        case RANGA_ENVIRONMENT_GET_WINDOW:
            if (g_window == nullptr) return false;
            *static_cast<HWND*>(data) = g_window;
            if (!g_coreDraws) Log("[retro] o core desenha sozinho na janela do jogo\n");
            g_coreDraws = true;
            return true;
        case RANGA_ENVIRONMENT_SET_FPS:
            g_coreFps = *static_cast<const float*>(data);
            return true;
        case RANGA_ENVIRONMENT_GET_TEXT:
            return data != nullptr && AskText(*static_cast<ranga_text_request*>(data));
    }
    // RANGA_RETRO_DEBUG=1 also names each request Ranga does not answer, once, and keeps the core's debug messages.
    static const bool debug = std::getenv("RANGA_RETRO_DEBUG") != nullptr;
    static std::set<unsigned> told;
    if (debug && told.insert(command).second) Log("[retro] pedido sem resposta: %u (0x%X)\n", command & 0xffff, command);
    return false;
}

// ----------------------------------------------------------------------------------------------------- video, audio

uint32_t Expand5(uint32_t value) { return (value << 3) | (value >> 2); }
uint32_t Expand6(uint32_t value) { return (value << 2) | (value >> 4); }

void RETRO_CALLCONV VideoRefresh(const void* data, unsigned width, unsigned height, size_t pitch) {
    // A null frame repeats the previous one (GET_CAN_DUPE).
    if (data == nullptr || width == 0 || height == 0) return;
    // A GPU frame is already in the framebuffer; only its size is news.
    if (data == RETRO_HW_FRAME_BUFFER_VALID) {
        g_hwFrameWidth = width;
        g_hwFrameHeight = height;
        g.frameNew = true;
        return;
    }
    g.frame.resize(static_cast<size_t>(width) * height);
    const auto* bytes = static_cast<const uint8_t*>(data);
    for (unsigned y = 0; y < height; ++y) {
        uint32_t* out = g.frame.data() + static_cast<size_t>(y) * width;
        const uint8_t* row = bytes + y * pitch;
        if (g.pixelFormat == RETRO_PIXEL_FORMAT_XRGB8888) {
            const auto* in = reinterpret_cast<const uint32_t*>(row);
            for (unsigned x = 0; x < width; ++x) out[x] = in[x] | 0xff000000u;
        } else if (g.pixelFormat == RETRO_PIXEL_FORMAT_RGB565) {
            const auto* in = reinterpret_cast<const uint16_t*>(row);
            for (unsigned x = 0; x < width; ++x) out[x] = 0xff000000u | Expand5(in[x] >> 11) << 16 | Expand6((in[x] >> 5) & 63) << 8 | Expand5(in[x] & 31);
        } else {
            const auto* in = reinterpret_cast<const uint16_t*>(row);
            for (unsigned x = 0; x < width; ++x) out[x] = 0xff000000u | Expand5((in[x] >> 10) & 31) << 16 | Expand5((in[x] >> 5) & 31) << 8 | Expand5(in[x] & 31);
        }
    }
    g.frameWidth = width;
    g.frameHeight = height;
    g.frameNew = true;
}

void RETRO_CALLCONV AudioSample(int16_t left, int16_t right) {
    g.audio.push_back(left);
    g.audio.push_back(right);
}

size_t RETRO_CALLCONV AudioBatch(const int16_t* data, size_t frames) {
    g.audio.insert(g.audio.end(), data, data + frames * 2);
    return frames;
}

// The core's sound reaches the device resampled, with its rate nudged by up to half a percent so the queue stays near
// its target: the display's refresh, not the sound card, sets the pace, and neither drifts away from the other.
class Audio {
public:
    bool Open() {
        SDL_AudioSpec want{}, have{};
        want.freq = 48000;
        want.format = AUDIO_S16SYS;
        want.channels = 2;
        want.samples = 512;
        device = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
        if (device == 0) {
            Log("[retro] sem som: %s\n", SDL_GetError());
            return false;
        }
        SDL_PauseAudioDevice(device, 0);
        return true;
    }
    ~Audio() { if (device != 0) SDL_CloseAudioDevice(device); }
    unsigned Queued() const { return device != 0 ? SDL_GetQueuedAudioSize(device) : 0; }
    void Clear() { if (device != 0) SDL_ClearQueuedAudio(device); }

    void Push(const std::vector<int16_t>& in, double inputRate) {
        if (device == 0 || in.size() < 4 || inputRate <= 0) return;
        const double target = 0.064 * 48000 * 4, queued = SDL_GetQueuedAudioSize(device);
        // Far behind (the game was paused or the window dragged): start over instead of playing late. Nearly empty (the
        // start, or a stall): fill up to the target with silence at once, which the nudge alone would take minutes to do.
        if (queued > target * 4) SDL_ClearQueuedAudio(device);
        if (queued < target / 4) {
            const std::vector<int16_t> silence(static_cast<size_t>(target - queued) / sizeof(int16_t) & ~size_t{1});
            SDL_QueueAudio(device, silence.data(), static_cast<Uint32>(silence.size() * sizeof(int16_t)));
        }
        const double nudge = std::clamp((target - queued) / target, -1.0, 1.0) * 0.005;
        const double step = inputRate / 48000.0 / (1.0 + nudge);
        const size_t frames = in.size() / 2;
        out.clear();
        for (; position < static_cast<double>(frames - 1); position += step) {
            const size_t at = static_cast<size_t>(position);
            const double t = position - static_cast<double>(at);
            out.push_back(static_cast<int16_t>(in[at * 2] + (in[at * 2 + 2] - in[at * 2]) * t));
            out.push_back(static_cast<int16_t>(in[at * 2 + 1] + (in[at * 2 + 3] - in[at * 2 + 1]) * t));
        }
        position -= static_cast<double>(frames - 1);
        SDL_QueueAudio(device, out.data(), static_cast<Uint32>(out.size() * sizeof(int16_t)));
    }

private:
    SDL_AudioDeviceID device = 0;
    double position = 0;
    std::vector<int16_t> out;
};

// The picture's filter and size on the screen, from the game's options (ranga_filtro, ranga_escala), for the cores whose
// frames Ranga draws itself: filter 0 sharp pixels, 1 smooth, 2 old TV, 3 plain pixels; size 0 fill, 1 whole
// multiples, 2 stretch.
int g_screenFilter = 0, g_screenScale = 0;

// Draws the frame scaled to the window with the core's aspect ratio, through "sharp bilinear": pixels stay square and
// crisp at any size, and only the seams between them are blended.
const char kShader[] = R"(
cbuffer Frame : register(b0) { float2 texSize; float2 frameSize; float2 scale; float2 pad; };
Texture2D image : register(t0);
SamplerState smooth : register(s0);
struct V { float4 position : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    v.uv *= frameSize;
    return v;
}
float4 ps(V v) : SV_Target {
    float2 texel = v.uv;
    float2 floored = floor(texel);
    if (pad.x > 2.5) return image.Sample(smooth, (floored + 0.5) / texSize);
    if (pad.x > 0.5 && pad.x < 1.5) return image.Sample(smooth, texel / texSize);
    float2 range = 0.5 - 0.5 / scale;
    float2 center = frac(texel) - 0.5;
    float2 f = (center - clamp(center, -range, range)) * scale + 0.5;
    float4 color = image.Sample(smooth, (floored + f) / texSize);
    if (pad.x > 1.5) {
        // An old TV: dark gaps between the lines (only where a line is at least two screen rows tall) and a faint
        // aperture grille, with the brightness given back so the picture is not dimmer overall.
        float rows = scale.y >= 2.0 ? 0.62 + 0.38 * sin(3.14159 * frac(texel.y)) : 1.0;
        float grille = 0.92 + 0.08 * sin(v.position.x * 2.0944);
        color.rgb = saturate(color.rgb * rows * grille * 1.22);
    }
    return color;
}
)";

class Video {
public:
    bool Create(HWND window) {
        DXGI_SWAP_CHAIN_DESC description{};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        const D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
        if (FAILED(D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, &level, 1, D3D11_SDK_VERSION, &description, &swapChain, &device, nullptr, &context)))
            return false;
        ID3DBlob* vertexCode = nullptr;
        ID3DBlob* pixelCode = nullptr;
        ID3DBlob* errors = nullptr;
        if (FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "ranga", nullptr, nullptr, "vs", "vs_4_0", 0, 0, &vertexCode, &errors)) ||
            FAILED(D3DCompile(kShader, sizeof(kShader) - 1, "ranga", nullptr, nullptr, "ps", "ps_4_0", 0, 0, &pixelCode, &errors))) {
            Log("[retro] shader: %s\n", errors != nullptr ? static_cast<const char*>(errors->GetBufferPointer()) : "?");
            return false;
        }
        device->CreateVertexShader(vertexCode->GetBufferPointer(), vertexCode->GetBufferSize(), nullptr, &vertexShader);
        device->CreatePixelShader(pixelCode->GetBufferPointer(), pixelCode->GetBufferSize(), nullptr, &pixelShader);
        vertexCode->Release();
        pixelCode->Release();
        D3D11_SAMPLER_DESC sampler{};
        sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        sampler.MaxLOD = D3D11_FLOAT32_MAX;
        device->CreateSamplerState(&sampler, &samplerState);
        D3D11_BUFFER_DESC buffer{};
        buffer.ByteWidth = 32;
        buffer.Usage = D3D11_USAGE_DEFAULT;
        buffer.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        device->CreateBuffer(&buffer, nullptr, &constants);
        return true;
    }

    void Resize() {
        if (target != nullptr) { target->Release(); target = nullptr; }
        swapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
        ID3D11Texture2D* back = nullptr;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
        if (back != nullptr) {
            device->CreateRenderTargetView(back, nullptr, &target);
            back->Release();
        }
    }

    void Upload(const std::vector<uint32_t>& pixels, unsigned width, unsigned height) {
        if (width > textureWidth || height > textureHeight) {
            if (view != nullptr) { view->Release(); view = nullptr; }
            if (texture != nullptr) { texture->Release(); texture = nullptr; }
            textureWidth = std::max(width, textureWidth);
            textureHeight = std::max(height, textureHeight);
            D3D11_TEXTURE2D_DESC description{};
            description.Width = textureWidth;
            description.Height = textureHeight;
            description.MipLevels = description.ArraySize = 1;
            description.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            description.SampleDesc.Count = 1;
            description.Usage = D3D11_USAGE_DEFAULT;
            description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            device->CreateTexture2D(&description, nullptr, &texture);
            device->CreateShaderResourceView(texture, nullptr, &view);
        }
        const D3D11_BOX box{0, 0, 0, width, height, 1};
        context->UpdateSubresource(texture, 0, &box, pixels.data(), width * 4, 0);
        frameWidth = width;
        frameHeight = height;
    }

    void Draw(float aspect, bool vsync) {
        if (target == nullptr) Resize();
        RECT client{};
        GetClientRect(g_window, &client);
        const float windowWidth = static_cast<float>(client.right), windowHeight = static_cast<float>(client.bottom);
        const float black[4] = {0, 0, 0, 1};
        context->ClearRenderTargetView(target, black);
        if (view != nullptr && frameWidth != 0 && windowWidth > 0 && windowHeight > 0) {
            if (aspect <= 0) aspect = static_cast<float>(frameWidth) / static_cast<float>(frameHeight);
            float width = windowWidth, height = windowWidth / aspect;
            if (height > windowHeight) {
                height = windowHeight;
                width = windowHeight * aspect;
            }
            if (g_screenScale == 1) {
                // Whole multiples of the game's lines, so every pixel is the same size (pixel perfect).
                float times = std::floor(height / static_cast<float>(frameHeight));
                while (times > 1 && times * frameHeight * aspect > windowWidth) times -= 1;
                if (times >= 1) {
                    height = times * static_cast<float>(frameHeight);
                    width = height * aspect;
                }
            } else if (g_screenScale == 2) {
                width = windowWidth;
                height = windowHeight;
            }
            const D3D11_VIEWPORT viewport{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height, 0, 1};
            const float values[8] = {static_cast<float>(textureWidth), static_cast<float>(textureHeight), static_cast<float>(frameWidth), static_cast<float>(frameHeight),
                                     width / static_cast<float>(frameWidth), height / static_cast<float>(frameHeight), static_cast<float>(g_screenFilter), 0};
            context->UpdateSubresource(constants, 0, nullptr, values, 0, 0);
            context->OMSetRenderTargets(1, &target, nullptr);
            context->RSSetViewports(1, &viewport);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            context->VSSetShader(vertexShader, nullptr, 0);
            context->PSSetShader(pixelShader, nullptr, 0);
            context->PSSetConstantBuffers(0, 1, &constants);
            context->PSSetShaderResources(0, 1, &view);
            context->PSSetSamplers(0, 1, &samplerState);
            context->Draw(3, 0);
        }
        swapChain->Present(vsync ? 1 : 0, 0);
    }

private:
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* target = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11SamplerState* samplerState = nullptr;
    ID3D11Buffer* constants = nullptr;
    ID3D11Texture2D* texture = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    unsigned textureWidth = 0, textureHeight = 0, frameWidth = 0, frameHeight = 0;
};


// Stage 2: a core that draws with the GPU (SET_HW_RENDER) gets an OpenGL context on the game window and draws into a
// framebuffer object of Ranga's; each frame is then blitted to the window with the core's aspect ratio. Only the OpenGL
// entry points this needs are loaded, through wglGetProcAddress.
namespace gl {
using GLenum = unsigned;
using GLuint = unsigned;
using GLint = int;
using GLsizei = int;
using GLbitfield = unsigned;
constexpr GLenum kFramebuffer = 0x8D40, kReadFramebuffer = 0x8CA8, kDrawFramebuffer = 0x8CA9, kColorAttachment0 = 0x8CE0;
constexpr GLenum kRenderbuffer = 0x8D41, kDepth24Stencil8 = 0x88F0, kDepthStencilAttachment = 0x821A, kComplete = 0x8CD5;
constexpr GLenum kTexture2D = 0x0DE1, kRgba8 = 0x8058, kRgba = 0x1908, kUnsignedByte = 0x1401, kLinear = 0x2601;
constexpr GLenum kTextureMinFilter = 0x2801, kTextureMagFilter = 0x2800, kScissorTest = 0x0C11;
constexpr GLbitfield kColorBufferBit = 0x4000;
constexpr int kContextMajor = 0x2091, kContextMinor = 0x2092, kContextProfile = 0x9126, kCoreProfile = 1, kCompatibilityProfile = 2;

void(WINAPI* GenFramebuffers)(GLsizei, GLuint*);
void(WINAPI* BindFramebuffer)(GLenum, GLuint);
void(WINAPI* FramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
void(WINAPI* GenRenderbuffers)(GLsizei, GLuint*);
void(WINAPI* BindRenderbuffer)(GLenum, GLuint);
void(WINAPI* RenderbufferStorage)(GLenum, GLenum, GLsizei, GLsizei);
void(WINAPI* FramebufferRenderbuffer)(GLenum, GLenum, GLenum, GLuint);
GLenum(WINAPI* CheckFramebufferStatus)(GLenum);
void(WINAPI* BlitFramebuffer)(GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLint, GLbitfield, GLenum);
BOOL(WINAPI* SwapInterval)(int);
}

extern "C" {
__declspec(dllimport) void WINAPI glViewport(int, int, int, int);
__declspec(dllimport) void WINAPI glClearColor(float, float, float, float);
__declspec(dllimport) void WINAPI glClear(unsigned);
__declspec(dllimport) void WINAPI glDisable(unsigned);
__declspec(dllimport) void WINAPI glColorMask(unsigned char, unsigned char, unsigned char, unsigned char);
__declspec(dllimport) void WINAPI glGenTextures(int, unsigned*);
__declspec(dllimport) void WINAPI glBindTexture(unsigned, unsigned);
__declspec(dllimport) void WINAPI glTexImage2D(unsigned, int, int, int, int, int, unsigned, unsigned, const void*);
__declspec(dllimport) void WINAPI glTexParameteri(unsigned, unsigned, int);
__declspec(dllimport) void WINAPI glReadPixels(int, int, int, int, unsigned, unsigned, void*);
}

retro_hw_render_callback g_hw{};
bool g_hwActive = false;
unsigned g_hwFramebuffer = 0;
HMODULE g_openGl = nullptr;

uintptr_t RETRO_CALLCONV HwCurrentFramebuffer() { return g_hwFramebuffer; }

retro_proc_address_t RETRO_CALLCONV HwProcAddress(const char* name) {
    void* function = reinterpret_cast<void*>(wglGetProcAddress(name));
    // wglGetProcAddress answers 0, 1, 2, 3 or -1 for what opengl32.dll exports itself (OpenGL 1.1).
    const auto value = reinterpret_cast<intptr_t>(function);
    if (value >= -1 && value <= 3) function = reinterpret_cast<void*>(GetProcAddress(g_openGl, name));
    return reinterpret_cast<retro_proc_address_t>(function);
}

template <typename T> bool LoadGl(T& function, const char* name) {
    function = reinterpret_cast<T>(reinterpret_cast<void*>(HwProcAddress(name)));
    if (function == nullptr) Log("[retro] OpenGL sem %s\n", name);
    return function != nullptr;
}

class GlVideo {
public:
    bool Create(HWND window, unsigned width, unsigned height) {
        dc = GetDC(window);
        PIXELFORMATDESCRIPTOR format{sizeof(format), 1, PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER, PFD_TYPE_RGBA, 32};
        format.cDepthBits = 24;
        format.cStencilBits = 8;
        if (!SetPixelFormat(dc, ChoosePixelFormat(dc, &format), &format)) return false;
        g_openGl = LoadLibraryW(L"opengl32.dll");
        HGLRC legacy = wglCreateContext(dc);
        if (legacy == nullptr || !wglMakeCurrent(dc, legacy)) return false;
        context = legacy;
        // A core profile context of the version the core asked for; a core asking for plain OpenGL keeps the
        // compatibility context the driver gives, which on NVIDIA is the newest version.
        using CreateAttribs = HGLRC(WINAPI*)(HDC, HGLRC, const int*);
        const auto createAttribs = reinterpret_cast<CreateAttribs>(reinterpret_cast<void*>(wglGetProcAddress("wglCreateContextAttribsARB")));
        if (g_hw.context_type == RETRO_HW_CONTEXT_OPENGL_CORE && createAttribs != nullptr) {
            const int major = std::max<int>(3, static_cast<int>(g_hw.version_major));
            const int minor = g_hw.version_major >= 3 ? static_cast<int>(g_hw.version_minor) : 3;
            const int attributes[] = {gl::kContextMajor, major, gl::kContextMinor, minor, gl::kContextProfile, gl::kCoreProfile, 0};
            HGLRC core = createAttribs(dc, nullptr, attributes);
            if (core == nullptr) {
                Log("[retro] OpenGL %d.%d core não disponível\n", major, minor);
                return false;
            }
            wglMakeCurrent(dc, core);
            wglDeleteContext(legacy);
            context = core;
        }
        if (!(LoadGl(gl::GenFramebuffers, "glGenFramebuffers") & LoadGl(gl::BindFramebuffer, "glBindFramebuffer") &
              LoadGl(gl::FramebufferTexture2D, "glFramebufferTexture2D") & LoadGl(gl::GenRenderbuffers, "glGenRenderbuffers") &
              LoadGl(gl::BindRenderbuffer, "glBindRenderbuffer") & LoadGl(gl::RenderbufferStorage, "glRenderbufferStorage") &
              LoadGl(gl::FramebufferRenderbuffer, "glFramebufferRenderbuffer") & LoadGl(gl::CheckFramebufferStatus, "glCheckFramebufferStatus") &
              LoadGl(gl::BlitFramebuffer, "glBlitFramebuffer")))
            return false;
        LoadGl(gl::SwapInterval, "wglSwapIntervalEXT");
        if (gl::SwapInterval != nullptr) gl::SwapInterval(1);
        gl::GenFramebuffers(1, &g_hwFramebuffer);
        return Size(width, height);
    }

    // The core draws up to its largest frame; the framebuffer grows when a new geometry asks for more.
    bool Size(unsigned width, unsigned height) {
        if (width <= this->width && height <= this->height) return true;
        this->width = std::max(width, this->width);
        this->height = std::max(height, this->height);
        if (texture == 0) glGenTextures(1, &texture);
        glBindTexture(gl::kTexture2D, texture);
        glTexImage2D(gl::kTexture2D, 0, static_cast<int>(gl::kRgba8), static_cast<int>(this->width), static_cast<int>(this->height), 0, gl::kRgba, gl::kUnsignedByte, nullptr);
        glTexParameteri(gl::kTexture2D, gl::kTextureMinFilter, static_cast<int>(gl::kLinear));
        glTexParameteri(gl::kTexture2D, gl::kTextureMagFilter, static_cast<int>(gl::kLinear));
        glBindTexture(gl::kTexture2D, 0);
        gl::BindFramebuffer(gl::kFramebuffer, g_hwFramebuffer);
        gl::FramebufferTexture2D(gl::kFramebuffer, gl::kColorAttachment0, gl::kTexture2D, texture, 0);
        if (g_hw.depth || g_hw.stencil) {
            if (depth == 0) gl::GenRenderbuffers(1, &depth);
            gl::BindRenderbuffer(gl::kRenderbuffer, depth);
            gl::RenderbufferStorage(gl::kRenderbuffer, gl::kDepth24Stencil8, static_cast<int>(this->width), static_cast<int>(this->height));
            gl::FramebufferRenderbuffer(gl::kFramebuffer, gl::kDepthStencilAttachment, gl::kRenderbuffer, depth);
        }
        const bool complete = gl::CheckFramebufferStatus(gl::kFramebuffer) == gl::kComplete;
        gl::BindFramebuffer(gl::kFramebuffer, 0);
        Log("[retro] OpenGL: quadro de %ux%u %s\n", this->width, this->height, complete ? "pronto" : "INCOMPLETO");
        return complete;
    }

    void Present(unsigned frameWidth, unsigned frameHeight, float aspect) {
        RECT client{};
        GetClientRect(g_window, &client);
        gl::BindFramebuffer(gl::kDrawFramebuffer, 0);
        glDisable(gl::kScissorTest);
        glColorMask(1, 1, 1, 1);
        glViewport(0, 0, client.right, client.bottom);
        glClearColor(0, 0, 0, 1);
        glClear(gl::kColorBufferBit);
        if (frameWidth != 0 && frameHeight != 0 && client.right > 0 && client.bottom > 0) {
            if (aspect <= 0) aspect = static_cast<float>(frameWidth) / static_cast<float>(frameHeight);
            float w = static_cast<float>(client.right), h = w / aspect;
            if (h > static_cast<float>(client.bottom)) {
                h = static_cast<float>(client.bottom);
                w = h * aspect;
            }
            const int x0 = static_cast<int>((static_cast<float>(client.right) - w) / 2), y0 = static_cast<int>((static_cast<float>(client.bottom) - h) / 2);
            gl::BindFramebuffer(gl::kReadFramebuffer, g_hwFramebuffer);
            // OpenGL's origin is the bottom left; a core that draws top-down is flipped on the way.
            const int top = g_hw.bottom_left_origin ? static_cast<int>(frameHeight) : 0, bottom = g_hw.bottom_left_origin ? 0 : static_cast<int>(frameHeight);
            gl::BlitFramebuffer(0, bottom, static_cast<int>(frameWidth), top, x0, y0, x0 + static_cast<int>(w), y0 + static_cast<int>(h), gl::kColorBufferBit, gl::kLinear);
            gl::BindFramebuffer(gl::kReadFramebuffer, 0);
        }
        // RANGA_RETRO_PROBE=1 logs a few pixels of the core's frame and of the window every two seconds.
        static const bool probe = std::getenv("RANGA_RETRO_PROBE") != nullptr;
        static ULONGLONG probeAt = 0;
        if (probe && GetTickCount64() - probeAt > 2000 && frameWidth != 0) {
            probeAt = GetTickCount64();
            unsigned char core[4] = {}, shown[4] = {};
            unsigned bright = 0;
            gl::BindFramebuffer(gl::kReadFramebuffer, g_hwFramebuffer);
            for (unsigned i = 1; i < 8; ++i) {
                unsigned char pixel[4] = {};
                glReadPixels(static_cast<int>(frameWidth * i / 8), static_cast<int>(frameHeight * i / 8), 1, 1, gl::kRgba, gl::kUnsignedByte, pixel);
                bright = std::max<unsigned>(bright, pixel[0] + pixel[1] + pixel[2]);
                if (i == 4) std::memcpy(core, pixel, 4);
            }
            gl::BindFramebuffer(gl::kReadFramebuffer, 0);
            glReadPixels(client.right / 2, client.bottom / 2, 1, 1, gl::kRgba, gl::kUnsignedByte, shown);
            Log("[retro] sonda: quadro %ux%u, centro do core %u,%u,%u, mais claro %u, centro da janela %u,%u,%u\n", frameWidth, frameHeight,
                core[0], core[1], core[2], bright, shown[0], shown[1], shown[2]);
        }
        SwapBuffers(dc);
    }

    ~GlVideo() {
        if (context != nullptr) {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(context);
        }
    }

private:
    HDC dc = nullptr;
    HGLRC context = nullptr;
    unsigned texture = 0, depth = 0, width = 0, height = 0;
};

// ------------------------------------------------------------------------------------------------------------ window

bool g_running = true, g_resized = false, g_fullscreen = false;
WINDOWPLACEMENT g_placement{sizeof(g_placement)};

void ToggleFullscreen() {
    const LONG_PTR style = GetWindowLongPtrW(g_window, GWL_STYLE);
    if (!g_fullscreen) {
        MONITORINFO monitor{sizeof(monitor)};
        GetWindowPlacement(g_window, &g_placement);
        GetMonitorInfoW(MonitorFromWindow(g_window, MONITOR_DEFAULTTOPRIMARY), &monitor);
        SetWindowLongPtrW(g_window, GWL_STYLE, style & ~WS_OVERLAPPEDWINDOW);
        const RECT& r = monitor.rcMonitor;
        SetWindowPos(g_window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongPtrW(g_window, GWL_STYLE, style | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(g_window, &g_placement);
        SetWindowPos(g_window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    g_fullscreen = !g_fullscreen;
    ShowCursor(g_fullscreen ? FALSE : TRUE);
}

int g_hotkey = 0;

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_SIZE:
            g_resized = true;
            return 0;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if ((lParam & (1 << 30)) != 0) break;
            if (wParam == VK_F11 || (wParam == VK_RETURN && (GetKeyState(VK_MENU) & 0x8000))) { ToggleFullscreen(); return 0; }
            if (wParam == VK_F5 || wParam == VK_F7 || wParam == VK_ESCAPE) { g_hotkey = static_cast<int>(wParam); return 0; }
            if (message == WM_SYSKEYDOWN) break;
            return 0;
        case WM_RBUTTONUP:
            g_hotkey = VK_ESCAPE;
            return 0;
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            g_running = false;
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

HWND CreateGameWindow(const std::wstring& title) {
    WNDCLASSEXW windowClass{sizeof(windowClass)};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = GetModuleHandleW(nullptr);
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hIcon = LoadIconW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(1));
    if (windowClass.hIcon == nullptr) windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    windowClass.lpszClassName = L"RangaEmuRetro";
    RegisterClassExW(&windowClass);
    MONITORINFO monitor{sizeof(monitor)};
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    RECT frame{0, 0, std::min<LONG>(1280, work.right - work.left), std::min<LONG>(960, work.bottom - work.top - 60)};
    AdjustWindowRect(&frame, WS_OVERLAPPEDWINDOW, FALSE);
    const LONG width = frame.right - frame.left, height = frame.bottom - frame.top;
    HWND window = CreateWindowExW(0, windowClass.lpszClassName, (L"Ranga Emu - " + title).c_str(), WS_OVERLAPPEDWINDOW,
                                  work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2, width, height,
                                  nullptr, nullptr, windowClass.hInstance, nullptr);
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, 20, &dark, sizeof(dark));
    return window;
}

double DisplayRefresh() {
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &mode) && mode.dmDisplayFrequency > 1) return mode.dmDisplayFrequency == 59 ? 59.94 : mode.dmDisplayFrequency;
    return 60.0;
}

// ---------------------------------------------------------------------------------------------------------- saves

struct Saves {
    std::wstring sram, state;
    std::vector<unsigned char> written;
    // Position 1 keeps the old name (<game>.state); positions 2 to 5 add their number.
    int slot = 0;
    std::wstring StatePath() const { return slot == 0 ? state : state + std::to_wstring(slot + 1); }

    void LoadSram() {
        const size_t size = g.core.memorySize(RETRO_MEMORY_SAVE_RAM);
        void* memory = g.core.memoryData(RETRO_MEMORY_SAVE_RAM);
        if (size == 0 || memory == nullptr) return;
        const auto data = ReadFile(sram);
        if (!data.empty()) {
            std::memcpy(memory, data.data(), std::min(size, data.size()));
            Log("[retro] save carregado: %zu bytes\n", data.size());
        }
        written.assign(static_cast<unsigned char*>(memory), static_cast<unsigned char*>(memory) + size);
    }

    // Writes the game's own save memory when it changed since the last write.
    void FlushSram() {
        if (Netplay::Active() && !Netplay::Hosting()) return;
        const size_t size = g.core.memorySize(RETRO_MEMORY_SAVE_RAM);
        const auto* memory = static_cast<const unsigned char*>(g.core.memoryData(RETRO_MEMORY_SAVE_RAM));
        if (size == 0 || memory == nullptr) return;
        if (written.size() == size && std::memcmp(written.data(), memory, size) == 0) return;
        if (WriteFile(sram, memory, size)) written.assign(memory, memory + size);
    }

    void SaveState() {
        const size_t size = g.core.serializeSize();
        std::vector<unsigned char> data(size);
        const bool saved = size != 0 && g.core.serialize(data.data(), size) && WriteFile(StatePath(), data.data(), size);
        ShowMessage(saved ? L"Estado salvo na posição " + std::to_wstring(slot + 1) : L"Este core não salva estado", 0);
    }

    void LoadState() {
        const auto data = ReadFile(StatePath());
        const bool loaded = !data.empty() && g.core.unserialize(data.data(), data.size());
        if (loaded) Achievements::Reset();
        ShowMessage(loaded ? L"Estado carregado da posição " + std::to_wstring(slot + 1) : L"Nenhum estado na posição " + std::to_wstring(slot + 1), 0);
    }
};

// ------------------------------------------------------------------------------------------------------------ menu

// The in-game menu: Esc, a right click or the gamepad's PS/Guide button opens it, and the game waits while it is open.
// It is a Windows popup menu; a timer turns the gamepad's D-pad, A/Cross and B/Circle into the arrow, Enter and Esc
// keys it understands.
enum MenuCommand { kContinue = 1, kSave, kLoad, kReset, kFullscreen, kQuit, kSlot = 10, kOption = 1000 };

// Remembers a core option the user picked in the game's options file, so the game starts with it next time.
void PersistOption(const std::string& key, const std::string& value) {
    if (g.optionsPath.empty()) return;
    const auto data = ReadFile(g.optionsPath);
    std::string text(data.begin(), data.end()), out;
    bool replaced = false;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const size_t equals = line.find('=');
        std::string name = equals == std::string::npos ? "" : line.substr(0, equals);
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name == key) {
            line = key + " = \"" + value + "\"";
            replaced = true;
        }
        out += line + "\r\n";
    }
    if (!replaced) out += key + " = \"" + value + "\"\r\n";
    WriteFile(g.optionsPath, out.data(), out.size());
}

// The menu over the game (Esc, or the gamepad's PS/Guide button), drawn by Ranga in its own colors instead of a
// Windows menu: up and down choose, left and right change a value (the save slot, full screen, a core option), A or
// Enter picks, B or Esc goes back and closes. The game waits while it is open; the page behind it is dimmed.
namespace menu {

// The client's own palette (Client.cpp): flat and nearly black, the accent only on what is chosen.
const COLORREF kPanel = RGB(0x10, 0x11, 0x14), kRow = RGB(0x20, 0x21, 0x26), kText = RGB(0xe8, 0xe8, 0xea),
               kDim = RGB(0x8b, 0x8d, 0x93), kAccent = RGB(0xe5, 0x48, 0x4d), kLine = RGB(0x20, 0x21, 0x26);
constexpr int kShown = 10;

struct Row {
    std::wstring label, value;
    // Left and right change it; the value is drawn between ‹ › while the row has the focus.
    bool changes = false;
};

struct MenuState {
    HWND window = nullptr, dim = nullptr;
    // 0 the menu, 1 the core options.
    int page = 0, focus = 0, top = 0;
    int slot = 0;
    bool fullscreen = false, optionChanged = false;
    // The command picked; 0 while the menu is open.
    int result = 0;
    // The core options with a choice, as indices into g.optionOrder.
    std::vector<size_t> options;
    float scale = 1;
};
MenuState s;

std::vector<Row> Rows() {
    std::vector<Row> rows;
    if (s.page == 0) {
        const std::wstring slot = L"posição " + std::to_wstring(s.slot + 1);
        rows = {{L"Continuar", L""},
                {L"Salvar estado", slot},
                {L"Carregar estado", slot},
                {L"Posição do estado", std::to_wstring(s.slot + 1) + L" de 5", true},
                {L"Opções do core", s.options.empty() ? L"nenhuma" : std::to_wstring(s.options.size()) + L"  ›"},
                {L"Tela cheia", s.fullscreen ? L"sim" : L"não", true},
                {L"Reiniciar o jogo", L""},
                {L"Sair do jogo", L""}};
        return rows;
    }
    for (const size_t index : s.options) {
        const std::string& key = g.optionOrder[index];
        const auto& info = g.optionInfo[key];
        const std::string& current = g.options[key];
        std::wstring value = Wide(current);
        for (const auto& [name, label] : info.values) {
            if (name == current) value = Wide(label.empty() ? name : label);
        }
        rows.push_back({Wide(info.description.empty() ? key : info.description), value, true});
    }
    return rows;
}

int Visible(size_t rows) { return static_cast<int>(std::min<size_t>(rows, kShown)); }

// The panel's size for the page, centered on the game's window, with round corners.
void Place() {
    const int rows = Visible(Rows().size());
    const int width = static_cast<int>(560 * s.scale), height = static_cast<int>((74 + 46 * rows + 50) * s.scale);
    RECT game{};
    GetWindowRect(g_window, &game);
    const int x = (game.left + game.right - width) / 2, y = (game.top + game.bottom - height) / 2;
    SetWindowPos(s.window, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
    const int round = static_cast<int>(16 * s.scale);
    SetWindowRgn(s.window, CreateRoundRectRgn(0, 0, width + 1, height + 1, round, round), TRUE);
    if (s.dim != nullptr) SetWindowPos(s.dim, s.window, game.left, game.top, game.right - game.left, game.bottom - game.top, SWP_NOACTIVATE);
    InvalidateRect(s.window, nullptr, FALSE);
}

void Move(int step) {
    const int count = static_cast<int>(Rows().size());
    if (count == 0) return;
    s.focus = (s.focus + step + count) % count;
    if (s.focus < s.top) s.top = s.focus;
    if (s.focus >= s.top + kShown) s.top = s.focus - kShown + 1;
    InvalidateRect(s.window, nullptr, FALSE);
}

void Change(int step) {
    if (s.page == 0) {
        if (s.focus == 3) s.slot = (s.slot + 5 + step) % 5;
        else if (s.focus == 5) s.fullscreen = !s.fullscreen;
    } else if (s.focus < static_cast<int>(s.options.size())) {
        const std::string& key = g.optionOrder[s.options[static_cast<size_t>(s.focus)]];
        const auto& values = g.optionInfo[key].values;
        size_t at = 0;
        for (size_t v = 0; v < values.size(); ++v) {
            if (values[v].first == g.options[key]) at = v;
        }
        at = step > 0 ? (at + 1) % values.size() : (at + values.size() - 1) % values.size();
        g.options[key] = values[at].first;
        g.optionsChanged = true;
        s.optionChanged = true;
        PersistOption(key, values[at].first);
    }
    InvalidateRect(s.window, nullptr, FALSE);
}

void Pick() {
    if (s.page == 1) {
        Change(1);
        return;
    }
    switch (s.focus) {
        case 0: s.result = kContinue; break;
        case 1: s.result = kSave; break;
        case 2: s.result = kLoad; break;
        case 4:
            if (!s.options.empty()) {
                s.page = 1;
                s.focus = s.top = 0;
                Place();
            }
            break;
        case 6: s.result = kReset; break;
        case 7: s.result = kQuit; break;
        default: Change(1); break;
    }
}

void Back() {
    if (s.page == 1) {
        s.page = 0;
        s.focus = 4;
        s.top = 0;
        Place();
    } else {
        s.result = kContinue;
    }
}

void Paint(HWND window) {
    PAINTSTRUCT paint;
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const auto fill = [&](RECT rect, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    };
    const auto px = [](float value) { return static_cast<int>(value * s.scale); };
    fill(client, kPanel);
    SetBkMode(dc, TRANSPARENT);
    HFONT title = CreateFontW(-px(22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT text = CreateFontW(-px(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, title);
    SetTextColor(dc, kText);
    RECT head{px(28), px(22), client.right - px(28), px(56)};
    DrawTextW(dc, s.page == 0 ? L"Menu do jogo" : L"Opções do core", -1, &head, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    SelectObject(dc, small);
    SetTextColor(dc, kDim);
    DrawTextW(dc, L"Ranga Emu", -1, &head, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    fill(RECT{px(28), px(64), client.right - px(28), px(65)}, kLine);

    const auto rows = Rows();
    const int shown = Visible(rows.size());
    SelectObject(dc, text);
    for (int i = 0; i < shown; ++i) {
        const int index = s.top + i;
        if (index >= static_cast<int>(rows.size())) break;
        const Row& row = rows[static_cast<size_t>(index)];
        const bool focused = index == s.focus;
        RECT line{px(16), px(74) + i * px(46), client.right - px(16), px(74) + (i + 1) * px(46) - px(4)};
        if (focused) {
            fill(line, kRow);
            fill(RECT{line.left, line.top + px(8), line.left + px(4), line.bottom - px(8)}, kAccent);
        }
        RECT label{line.left + px(18), line.top, line.right - px(16), line.bottom};
        if (!row.value.empty()) {
            // The value takes at most 60% of the row, and the name what is left; both end in "…" when cut.
            const std::wstring value = focused && row.changes ? L"‹  " + row.value + L"  ›" : row.value;
            RECT measure{0, 0, 0, 0};
            DrawTextW(dc, value.c_str(), -1, &measure, DT_CALCRECT | DT_SINGLELINE);
            const int width = std::min<int>(measure.right, (label.right - label.left) * 6 / 10);
            RECT place{label.right - width, label.top, label.right, label.bottom};
            SetTextColor(dc, focused ? kText : kDim);
            DrawTextW(dc, value.c_str(), -1, &place, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            label.right = place.left - px(16);
        }
        SetTextColor(dc, focused ? kText : RGB(0xb4, 0xb6, 0xbc));
        DrawTextW(dc, row.label.c_str(), -1, &label, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    if (rows.size() > static_cast<size_t>(kShown)) {
        // Where the list is, as a thin bar on the right.
        const int span = shown * px(46), bar = std::max(px(24), span * shown / static_cast<int>(rows.size()));
        const int at = px(74) + (span - bar) * s.top / std::max(1, static_cast<int>(rows.size()) - shown);
        fill(RECT{client.right - px(8), at, client.right - px(5), at + bar}, kLine);
    }
    SelectObject(dc, small);
    SetTextColor(dc, kDim);
    RECT foot{px(28), client.bottom - px(40), client.right - px(28), client.bottom - px(12)};
    DrawTextW(dc, s.page == 0 ? L"↑↓ escolher    ←→ mudar    A / Enter: ok    B / Esc: voltar ao jogo"
                              : L"↑↓ escolher    ←→ mudar    B / Esc: voltar", -1, &foot, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(title);
    DeleteObject(text);
    DeleteObject(small);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &paint);
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            Paint(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
            if (wParam == VK_UP) Move(-1);
            else if (wParam == VK_DOWN) Move(1);
            else if (wParam == VK_LEFT) Change(-1);
            else if (wParam == VK_RIGHT) Change(1);
            else if (wParam == VK_RETURN || wParam == VK_SPACE) Pick();
            else if (wParam == VK_ESCAPE || wParam == VK_BACK) Back();
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP: {
            const int y = static_cast<short>(HIWORD(lParam)), row = (y - static_cast<int>(74 * s.scale)) / static_cast<int>(46 * s.scale);
            if (y >= static_cast<int>(74 * s.scale) && row < Visible(Rows().size()) && s.top + row != s.focus) {
                s.focus = s.top + row;
                InvalidateRect(window, nullptr, FALSE);
            }
            if (message == WM_LBUTTONUP && y >= static_cast<int>(74 * s.scale) && row < Visible(Rows().size())) Pick();
            return 0;
        }
        case WM_MOUSEWHEEL:
            Move(GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? -1 : 1);
            return 0;
        case WM_RBUTTONUP:
            Back();
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// The gamepads: the d-pad or left stick moves and changes (repeating while held), A picks, B goes back, PS/Guide or
// Start closes. A button already down when the menu opened (the one that opened it) waits to be let go.
void PollPads() {
    enum { Up, Down, Left, Right, A, B, Close, Count };
    static bool held[Count];
    static ULONGLONG next[Count];
    static bool first = true;
    SDL_GameControllerUpdate();
    bool now[Count]{};
    for (auto* pad : g_pads) {
        if (pad == nullptr) continue;
        const auto button = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
        const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        now[Up] |= button(SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -20000;
        now[Down] |= button(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 20000;
        now[Left] |= button(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -20000;
        now[Right] |= button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > 20000;
        now[A] |= button(SDL_CONTROLLER_BUTTON_A);
        now[B] |= button(SDL_CONTROLLER_BUTTON_B);
        now[Close] |= button(SDL_CONTROLLER_BUTTON_GUIDE) || button(SDL_CONTROLLER_BUTTON_START);
    }
    // RANGA_RETRO_PAD_SCRIPT drives it too, in RetroPad names: B (the bottom button) picks, A goes back.
    now[Up] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_UP);
    now[Down] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_DOWN);
    now[Left] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_LEFT);
    now[Right] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    now[A] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_B);
    now[B] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_A);
    now[Close] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_START);
    const ULONGLONG tick = GetTickCount64();
    for (int b = 0; b < Count; ++b) {
        if (first || !now[b]) {
            held[b] = now[b];
            next[b] = tick + 350;
            continue;
        }
        const bool press = !held[b] || (b <= Right && tick >= next[b]);
        if (!held[b]) next[b] = tick + 350;
        else if (press) next[b] = tick + 90;
        held[b] = true;
        if (!press) continue;
        if (b == Up) Move(-1);
        else if (b == Down) Move(1);
        else if (b == Left) Change(-1);
        else if (b == Right) Change(1);
        else if (b == A) Pick();
        else if (b == B) Back();
        else s.result = kContinue;
    }
    first = false;
}

} // namespace menu

// Shows the menu and returns what was picked; slot and fullscreen come back as the menu left them, and the core
// options are applied here.
int ShowMenu(int& slot, bool& fullscreen) {
    using namespace menu;
    s = MenuState{};
    s.slot = slot;
    s.fullscreen = g_fullscreen;
    for (size_t i = 0; i < g.optionOrder.size(); ++i) {
        if (g.optionInfo[g.optionOrder[i]].values.size() >= 2) s.options.push_back(i);
    }
    HDC screen = GetDC(nullptr);
    s.scale = std::max(1.0f, GetDeviceCaps(screen, LOGPIXELSY) / 96.0f);
    ReleaseDC(nullptr, screen);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW panel{sizeof(panel)};
        panel.lpfnWndProc = Proc;
        panel.hInstance = GetModuleHandleW(nullptr);
        panel.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        panel.lpszClassName = L"RangaRetroMenu";
        RegisterClassExW(&panel);
        WNDCLASSEXW shade{sizeof(shade)};
        shade.lpfnWndProc = DefWindowProcW;
        shade.hInstance = GetModuleHandleW(nullptr);
        shade.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        shade.lpszClassName = L"RangaRetroMenuShade";
        RegisterClassExW(&shade);
        registered = true;
    }
    s.dim = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"RangaRetroMenuShade", L"", WS_POPUP, 0, 0, 1, 1,
                            g_window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (s.dim != nullptr) SetLayeredWindowAttributes(s.dim, 0, 150, LWA_ALPHA);
    s.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"RangaRetroMenu", L"Ranga Emu - menu", WS_POPUP, 0, 0, 1, 1, g_window, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (s.window == nullptr) {
        // Without its window the menu would hold the game with nothing on screen.
        Log("[retro] o menu não abriu (erro %lu)\n", GetLastError());
        if (s.dim != nullptr) DestroyWindow(s.dim);
        return kContinue;
    }
    Place();
    if (s.dim != nullptr) ShowWindow(s.dim, SW_SHOWNOACTIVATE);
    ShowWindow(s.window, SW_SHOW);
    SetForegroundWindow(s.window);
    SetFocus(s.window);
    while (s.result == 0) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                s.result = kQuit;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        PollPads();
        if (!g_running || !IsWindow(g_window)) s.result = kQuit;
    }
    Log("[retro] menu: escolha %d\n", s.result);
    DestroyWindow(s.window);
    if (s.dim != nullptr) DestroyWindow(s.dim);
    SetForegroundWindow(g_window);
    slot = s.slot;
    fullscreen = s.fullscreen;
    if (s.optionChanged) ShowMessage(L"Opção alterada; alguns cores só aplicam ao reiniciar o jogo", 0);
    return s.result;
}

// The box for a game's keyboard (a name, a message), in the menu's colors: the text on top and an on-screen keyboard
// the gamepad walks (d-pad, A types the key, B erases, Y switches case, X is a space, Start confirms, Guide cancels).
// The mouse clicks keys and the PC's keyboard types straight in (Enter confirms, Esc cancels). The game waits while
// it is open, as with the menu.
namespace textbox {

using menu::kPanel;
using menu::kRow;
using menu::kText;
using menu::kDim;
using menu::kAccent;
using menu::kLine;

// The key rows; letters follow the case switch. The last row is the wide keys.
const wchar_t* const kKeys[] = {L"1234567890", L"qwertyuiop", L"asdfghjkl-", L"zxcvbnm._@"};
constexpr int kColumns = 10, kLetterRows = 4;
enum WideKey { kShift, kSpace, kErase, kOk };
// Where each wide key starts and how many columns it takes.
const int kWideStart[] = {0, 2, 6, 8}, kWideSpan[] = {2, 4, 2, 2};
const wchar_t* const kWideLabel[] = {L"Maiúsc", L"Espaço", L"Apagar", L"OK"};

struct Box {
    HWND window = nullptr, dim = nullptr;
    std::wstring title, guide, text;
    unsigned max = 0;
    bool password = false, upper = false;
    int row = 1, column = 0;
    // 0 open, 1 confirmed, 2 cancelled.
    int done = 0;
    float scale = 1;
};
Box s;

int Px(float value) { return static_cast<int>(value * s.scale); }

// The key's rectangle in the panel: rows of 10 keys under the text, the wide keys last.
RECT KeyRect(int row, int column, int span = 1) {
    const int left = Px(24), top = Px(176), width = Px(56), height = Px(46), gap = Px(6);
    return RECT{left + column * (width + gap), top + row * (height + gap), left + (column + span) * (width + gap) - gap, top + row * (height + gap) + height};
}

// The wide key under a column of the last row.
int WideAt(int column) {
    for (int k = 3; k >= 0; --k) {
        if (column >= kWideStart[k]) return k;
    }
    return 0;
}

wchar_t KeyChar(int row, int column) {
    const wchar_t c = kKeys[row][column];
    return s.upper ? static_cast<wchar_t>(towupper(c)) : c;
}

void Type(wchar_t c) {
    if (s.max > 0 && s.text.size() >= s.max) return;
    s.text += c;
    InvalidateRect(s.window, nullptr, FALSE);
}

void Erase() {
    if (!s.text.empty()) s.text.pop_back();
    InvalidateRect(s.window, nullptr, FALSE);
}

void Press() {
    if (s.row < kLetterRows) {
        Type(KeyChar(s.row, s.column));
        return;
    }
    switch (WideAt(s.column)) {
        case kShift: s.upper = !s.upper; break;
        case kSpace: Type(L' '); return;
        case kErase: Erase(); return;
        case kOk: s.done = 1; return;
    }
    InvalidateRect(s.window, nullptr, FALSE);
}

void Move(int rows, int columns) {
    if (s.row == kLetterRows && columns != 0) {
        // Along the wide keys, a step is a whole key.
        const int key = std::clamp(WideAt(s.column) + columns, 0, 3);
        s.column = kWideStart[key];
    } else {
        s.column = (s.column + columns + kColumns) % kColumns;
    }
    s.row = (s.row + rows + kLetterRows + 1) % (kLetterRows + 1);
    if (s.row == kLetterRows) s.column = kWideStart[WideAt(s.column)];
    InvalidateRect(s.window, nullptr, FALSE);
}

void Paint(HWND window) {
    PAINTSTRUCT paint;
    HDC target = BeginPaint(window, &paint);
    RECT client{};
    GetClientRect(window, &client);
    HDC dc = CreateCompatibleDC(target);
    HBITMAP bitmap = CreateCompatibleBitmap(target, client.right, client.bottom);
    HGDIOBJ oldBitmap = SelectObject(dc, bitmap);
    const auto fill = [&](RECT rect, COLORREF color) {
        HBRUSH brush = CreateSolidBrush(color);
        FillRect(dc, &rect, brush);
        DeleteObject(brush);
    };
    fill(client, kPanel);
    SetBkMode(dc, TRANSPARENT);
    HFONT title = CreateFontW(-Px(22), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT big = CreateFontW(-Px(22), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT text = CreateFontW(-Px(17), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT small = CreateFontW(-Px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HGDIOBJ oldFont = SelectObject(dc, title);
    SetTextColor(dc, kText);
    RECT head{Px(28), Px(22), client.right - Px(28), Px(56)};
    DrawTextW(dc, s.title.empty() ? L"Digite o texto" : s.title.c_str(), -1, &head, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    SelectObject(dc, small);
    SetTextColor(dc, kDim);
    DrawTextW(dc, L"Ranga Emu", -1, &head, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    fill(RECT{Px(28), Px(64), client.right - Px(28), Px(65)}, kLine);

    // The text, with a bar where the next letter goes, and how much room is left.
    RECT box{Px(24), Px(80), client.right - Px(24), Px(132)};
    fill(box, kRow);
    fill(RECT{box.left, box.bottom - Px(3), box.right, box.bottom}, kAccent);
    SelectObject(dc, big);
    SetTextColor(dc, kText);
    const std::wstring shown = (s.password ? std::wstring(s.text.size(), L'•') : s.text) + L"|";
    RECT inside{box.left + Px(14), box.top, box.right - Px(14), box.bottom};
    DrawTextW(dc, shown.c_str(), -1, &inside, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_PATH_ELLIPSIS);
    SelectObject(dc, small);
    SetTextColor(dc, kDim);
    RECT under{Px(28), Px(138), client.right - Px(28), Px(164)};
    if (!s.guide.empty()) DrawTextW(dc, s.guide.c_str(), -1, &under, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    if (s.max > 0) {
        const std::wstring count = std::to_wstring(s.text.size()) + L" / " + std::to_wstring(s.max);
        DrawTextW(dc, count.c_str(), -1, &under, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    }

    SelectObject(dc, text);
    const auto key = [&](RECT rect, const std::wstring& label, bool focused, bool lit) {
        fill(rect, focused ? kAccent : lit ? RGB(0x2a, 0x2b, 0x31) : kRow);
        SetTextColor(dc, focused ? RGB(0xff, 0xff, 0xff) : kText);
        DrawTextW(dc, label.c_str(), -1, &rect, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    };
    for (int r = 0; r < kLetterRows; ++r) {
        for (int c = 0; c < kColumns; ++c) key(KeyRect(r, c), std::wstring(1, KeyChar(r, c)), s.row == r && s.column == c, false);
    }
    for (int k = 0; k < 4; ++k) {
        key(KeyRect(kLetterRows, kWideStart[k], kWideSpan[k]), kWideLabel[k], s.row == kLetterRows && WideAt(s.column) == k, k == kShift && s.upper);
    }

    SelectObject(dc, small);
    SetTextColor(dc, kDim);
    RECT foot{Px(28), client.bottom - Px(40), client.right - Px(28), client.bottom - Px(12)};
    DrawTextW(dc, L"A tecla   B apaga   X espaço   Y maiúsc   Start OK   Guide/Esc cancela   ou digite no teclado do PC", -1,
              &foot, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    BitBlt(target, 0, 0, client.right, client.bottom, dc, 0, 0, SRCCOPY);
    SelectObject(dc, oldFont);
    SelectObject(dc, oldBitmap);
    DeleteObject(title);
    DeleteObject(big);
    DeleteObject(text);
    DeleteObject(small);
    DeleteObject(bitmap);
    DeleteDC(dc);
    EndPaint(window, &paint);
}

// The key under a point of the panel; false when there is none.
bool KeyAt(int x, int y, int& row, int& column) {
    for (int r = 0; r < kLetterRows; ++r) {
        for (int c = 0; c < kColumns; ++c) {
            const RECT rect = KeyRect(r, c);
            if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) {
                row = r;
                column = c;
                return true;
            }
        }
    }
    for (int k = 0; k < 4; ++k) {
        const RECT rect = KeyRect(kLetterRows, kWideStart[k], kWideSpan[k]);
        if (x >= rect.left && x < rect.right && y >= rect.top && y < rect.bottom) {
            row = kLetterRows;
            column = kWideStart[k];
            return true;
        }
    }
    return false;
}

LRESULT CALLBACK Proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_PAINT:
            Paint(window);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN) s.done = 1;
            else if (wParam == VK_ESCAPE) s.done = 2;
            else if (wParam == VK_BACK) Erase();
            else if (wParam == VK_UP) Move(-1, 0);
            else if (wParam == VK_DOWN) Move(1, 0);
            else if (wParam == VK_LEFT) Move(0, -1);
            else if (wParam == VK_RIGHT) Move(0, 1);
            return 0;
        case WM_CHAR:
            // What the PC's keyboard types, accents included; control characters were handled above.
            if (wParam >= 0x20 && wParam != 0x7f) Type(static_cast<wchar_t>(wParam));
            return 0;
        case WM_MOUSEMOVE:
        case WM_LBUTTONUP: {
            int row = 0, column = 0;
            if (KeyAt(static_cast<short>(LOWORD(lParam)), static_cast<short>(HIWORD(lParam)), row, column)) {
                if (row != s.row || column != s.column) {
                    s.row = row;
                    s.column = column;
                    InvalidateRect(window, nullptr, FALSE);
                }
                if (message == WM_LBUTTONUP) Press();
            }
            return 0;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

// The gamepads, as the menu reads them: the d-pad and left stick repeat while held; a button already down when the
// box opened waits to be let go.
void PollPads() {
    enum { Up, Down, Left, Right, A, B, X, Y, Start, Guide, Count };
    static bool held[Count];
    static ULONGLONG next[Count];
    static bool first = true;
    if (s.done == 0 && first) {
        for (auto& h : held) h = false;
    }
    SDL_GameControllerUpdate();
    bool now[Count]{};
    for (auto* pad : g_pads) {
        if (pad == nullptr) continue;
        const auto button = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
        const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
        now[Up] |= button(SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -20000;
        now[Down] |= button(SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 20000;
        now[Left] |= button(SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -20000;
        now[Right] |= button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > 20000;
        now[A] |= button(SDL_CONTROLLER_BUTTON_A);
        now[B] |= button(SDL_CONTROLLER_BUTTON_B);
        now[X] |= button(SDL_CONTROLLER_BUTTON_X);
        now[Y] |= button(SDL_CONTROLLER_BUTTON_Y);
        now[Start] |= button(SDL_CONTROLLER_BUTTON_START);
        now[Guide] |= button(SDL_CONTROLLER_BUTTON_GUIDE);
    }
    // RANGA_RETRO_PAD_SCRIPT, in RetroPad names: B (the bottom button) types, A erases.
    now[Up] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_UP);
    now[Down] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_DOWN);
    now[Left] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_LEFT);
    now[Right] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_RIGHT);
    now[A] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_B);
    now[B] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_A);
    now[Start] |= ScriptedButton(RETRO_DEVICE_ID_JOYPAD_START);
    const ULONGLONG tick = GetTickCount64();
    for (int b = 0; b < Count; ++b) {
        if (first || !now[b]) {
            held[b] = now[b];
            next[b] = tick + 350;
            continue;
        }
        const bool repeats = b <= Right || b == B;
        const bool press = !held[b] || (repeats && tick >= next[b]);
        if (!held[b]) next[b] = tick + 350;
        else if (press) next[b] = tick + 90;
        held[b] = true;
        if (!press) continue;
        switch (b) {
            case Up: Move(-1, 0); break;
            case Down: Move(1, 0); break;
            case Left: Move(0, -1); break;
            case Right: Move(0, 1); break;
            case A: Press(); break;
            case B: Erase(); break;
            case X: Type(L' '); break;
            case Y: s.upper = !s.upper; InvalidateRect(s.window, nullptr, FALSE); break;
            case Start: s.done = 1; break;
            case Guide: s.done = 2; break;
        }
    }
    first = false;
}

} // namespace textbox

bool AskText(ranga_text_request& request) {
    using namespace textbox;
    s = Box{};
    s.title = Wide(request.title != nullptr ? request.title : "");
    s.guide = Wide(request.guide != nullptr ? request.guide : "");
    s.text = Wide(request.initial != nullptr ? request.initial : "");
    s.max = request.max_length;
    s.password = request.password != 0;
    if (s.max > 0 && s.text.size() > s.max) s.text.resize(s.max);
    HDC screen = GetDC(nullptr);
    s.scale = std::max(1.0f, GetDeviceCaps(screen, LOGPIXELSY) / 96.0f);
    ReleaseDC(nullptr, screen);
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW panel{sizeof(panel)};
        panel.lpfnWndProc = textbox::Proc;
        panel.hInstance = GetModuleHandleW(nullptr);
        panel.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        panel.lpszClassName = L"RangaRetroText";
        RegisterClassExW(&panel);
        WNDCLASSEXW shade{sizeof(shade)};
        shade.lpfnWndProc = DefWindowProcW;
        shade.hInstance = GetModuleHandleW(nullptr);
        shade.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        shade.lpszClassName = L"RangaRetroTextShade";
        RegisterClassExW(&shade);
        registered = true;
    }
    s.dim = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, L"RangaRetroTextShade", L"", WS_POPUP, 0, 0, 1, 1,
                            g_window, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (s.dim != nullptr) SetLayeredWindowAttributes(s.dim, 0, 150, LWA_ALPHA);
    s.window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"RangaRetroText", L"Ranga Emu - texto", WS_POPUP, 0, 0, 1, 1, g_window, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    if (s.window == nullptr) {
        Log("[retro] a caixa de texto não abriu (erro %lu)\n", GetLastError());
        if (s.dim != nullptr) DestroyWindow(s.dim);
        return false;
    }
    // Centered on the game's window: ten keys of 56 and their gaps, the text above, the help line below.
    const int width = Px(24 * 2 + 10 * 56 + 9 * 6), height = Px(176 + 5 * 52 + 50);
    RECT game{};
    GetWindowRect(g_window, &game);
    SetWindowPos(s.window, HWND_TOPMOST, (game.left + game.right - width) / 2, (game.top + game.bottom - height) / 2, width, height, SWP_NOACTIVATE);
    SetWindowRgn(s.window, CreateRoundRectRgn(0, 0, width + 1, height + 1, Px(16), Px(16)), TRUE);
    if (s.dim != nullptr) {
        SetWindowPos(s.dim, s.window, game.left, game.top, game.right - game.left, game.bottom - game.top, SWP_NOACTIVATE);
        ShowWindow(s.dim, SW_SHOWNOACTIVATE);
    }
    ShowWindow(s.window, SW_SHOW);
    SetForegroundWindow(s.window);
    SetFocus(s.window);
    Log("[retro] caixa de texto aberta: %s\n", request.title != nullptr ? request.title : "");
    while (s.done == 0) {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 16, QS_ALLINPUT);
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) {
                PostQuitMessage(static_cast<int>(message.wParam));
                s.done = 2;
                break;
            }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        textbox::PollPads();
        if (!g_running || !IsWindow(g_window)) s.done = 2;
    }
    DestroyWindow(s.window);
    if (s.dim != nullptr) DestroyWindow(s.dim);
    SetForegroundWindow(g_window);
    const std::string typed = Utf8(s.text);
    if (request.result != nullptr && request.result_size > 0) {
        const size_t length = std::min<size_t>(typed.size(), request.result_size - 1);
        std::memcpy(request.result, typed.data(), length);
        request.result[length] = '\0';
    }
    request.accepted = s.done == 1 ? 1 : 0;
    Log("[retro] caixa de texto: %s\n", s.done == 1 ? "confirmada" : "cancelada");
    return true;
}

// Ranga's own defaults for a core, applied before the game's options file, which can still change any of them: the
// OpenGL renderer at about 1080p, and for SwanStation the PS1 BIOS inside the PS3 firmware Sony publishes (ps1_rom.bin,
// taken from RPCS3's dev_flash), which serves every region.
void ApplyCoreDefaults(const std::wstring& corePath, const std::wstring& systemDir) {
    std::wstring name = corePath.substr(corePath.find_last_of(L"\\/") + 1);
    for (auto& character : name) character = static_cast<wchar_t>(towlower(character));
    struct Default { const wchar_t* core; const char* key; const char* value; };
    static const Default defaults[] = {
        {L"melondsds", "melonds_render_mode", "opengl"},
        {L"xemu", "xemu_skip_boot_anim", "enabled"},
    };
    for (const auto& entry : defaults) {
        if (name.rfind(entry.core, 0) == 0) g.overrides[entry.key] = entry.value;
    }
    // The picture's size by the Qualidade chosen for this PC (--quality): ruim, fraco, bom, ótimo. bom is what Ranga
    // always used (about 1080p); the values are the ones each core lists (RANGA_RETRO_LIST_OPTIONS, 14/11/2026).
    struct Quality { const wchar_t* core; const char* key; const char* values[4]; };
    static const Quality qualities[] = {
        {L"swanstation", "swanstation_GPU_ResolutionScale", {"1", "2", "5", "8"}},
        {L"melondsds", "melonds_opengl_resolution", {"1", "2", "4", "6"}},
        {L"mupen64plus_next", "mupen64plus-43screensize", {"640x480", "960x720", "1440x1080", "1920x1440"}},
        {L"mupen64plus_next", "mupen64plus-parallel-rdp-upscaling", {"1x", "2x", "4x", "8x"}},
        {L"ppsspp", "ppsspp_internal_resolution", {"480x272", "960x544", "1920x1088", "2880x1632"}},
        {L"flycast", "reicast_internal_resolution", {"640x480", "960x720", "1920x1440", "2880x2160"}},
        {L"pcsx2", "pcsx2_upscale_multiplier", {"1x (Native)", "1x (Native)", "2x", "4x"}},
        {L"azahar", "citra_resolution_factor", {"1", "2", "3", "4"}},
        {L"cemu", "cemu_internal_resolution", {"0.75x", "native", "1.5x", "2x"}},
        {L"xemu", "xemu_surface_scale", {"1", "1", "2", "3"}},
        {L"dolphin", "dolphin_efb_scale", {"1", "2", "3", "4"}},
        {L"eden", "eden_qualidade", {"ruim", "fraco", "bom", "otimo"}},
        {L"ppsspp", "ppsspp_texture_anisotropic_filtering", {"disabled", "4x", "16x", "16x"}},
        {L"flycast", "reicast_anisotropic_filtering", {"off", "2", "4", "16"}},
        {L"pcsx2", "pcsx2_blending_accuracy", {"Minimum", "Basic", "Basic", "Medium"}},
        {L"dolphin", "dolphin_max_anisotropy", {"0", "0", "0", "3"}},
        // Shaders built in the background, the frame drawn without them meanwhile: no stutter the first time an effect
        // shows, which is what a weak PC feels most.
        {L"dolphin", "dolphin_shader_compilation_mode", {"3", "3", "2", "2"}},
        {L"swanstation", "swanstation_GPU_ShaderPrecompile", {"Enabled", "Enabled", "Lazy", "Lazy"}},
    };
    for (const auto& entry : qualities) {
        if (name.rfind(entry.core, 0) == 0) g.overrides[entry.key] = entry.values[g_quality];
    }
    static const char* const levels[] = {"ruim", "fraco", "bom", "ótimo"};
    Log("[retro] qualidade: %s\n", levels[g_quality]);
    // Without Sony's, Ranga's own PS1 BIOS (ranga-bios-ps1.bin: OpenBIOS, MIT, built with Ranga's colors in
    // C:\projetos\sos-ps5\ranga-bios-ps1), which boots most games but not all (Azure Dreams, Wipeout NTSC-U).
    if (name.rfind(L"swanstation", 0) == 0) {
        const bool sony = GetFileAttributesW((systemDir + L"\\ps1_rom.bin").c_str()) != INVALID_FILE_ATTRIBUTES;
        const char* bios = sony ? "ps1_rom.bin" : "ranga-bios-ps1.bin";
        for (const char* key : {"swanstation_BIOS_PathNTSCU", "swanstation_BIOS_PathNTSCJ", "swanstation_BIOS_PathPAL"}) g.overrides[key] = bios;
    }
    // With Vulkan, SwanStation draws with it and the N64 goes to paraLLEl-RDP, the accurate renderer that only runs on
    // Vulkan; with OpenGL, GLideN64.
    if (name.rfind(L"swanstation", 0) == 0) g.overrides["swanstation_GPU_Renderer"] = g_preferVulkan ? "Vulkan" : "OpenGL";
    if (name.rfind(L"mupen64plus_next", 0) == 0) g.overrides["mupen64plus-rdp-plugin"] = g_preferVulkan ? "parallel" : "gliden64";
    // Cemu starts in OpenGL; with Vulkan it shares Ranga's device. xemu hands over a finished picture either way.
    if (name.rfind(L"cemu", 0) == 0) g.overrides["cemu_gpu_api"] = g_preferVulkan ? "Vulkan" : "OpenGL";
}

void ReadOptionsFile(const std::wstring& path) {
    const auto data = ReadFile(path);
    std::string text(data.begin(), data.end());
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const size_t equals = line.find('=');
        if (line.empty() || line[0] == '#' || line[0] == ';' || equals == std::string::npos) continue;
        std::string key = line.substr(0, equals), value = line.substr(equals + 1);
        while (!key.empty() && key.back() == ' ') key.pop_back();
        while (!value.empty() && value.front() == ' ') value.erase(0, 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"') value = value.substr(1, value.size() - 2);
        if (value == "auto" && key.rfind("eden_", 0) != 0) continue;
        g.overrides[key] = value;
    }
}

}

// Online play (Netplay.hpp), when the game's options ask for it: ranga_online = ligado, ranga_online_endereco (the
// table server, "host:port"), ranga_online_atraso (frames, 2 to 8) and ranga_online_nome. The game is told apart by
// the core and the file (its size and the first 8 MB), so only the same game of the same core sits at one table.
void StartOnline(const std::wstring& corePath, const std::wstring& romPath, bool noGame) {
    const auto option = [](const char* key) {
        const auto found = g.overrides.find(key);
        return found == g.overrides.end() ? std::string() : found->second;
    };
    if (option("ranga_online") != "ligado" || noGame) return;
    std::string address = option("ranga_online_endereco");
    if (address.empty()) {
        ShowMessage(L"Online: falta o endereço da sala; jogando sozinho", 0);
        return;
    }
    if (g.core.serializeSize() == 0) {
        ShowMessage(L"Online: este core não salva estado, então não joga online", 0);
        Log("[online] o core não salva estado\n");
        return;
    }
    std::string game = Utf8(corePath.substr(corePath.find_last_of(L"\\/") + 1));
    for (char& c : game) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    {
        HANDLE file = CreateFileW(romPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        LARGE_INTEGER size{};
        std::vector<unsigned char> head(8u << 20);
        DWORD read = 0;
        if (file != INVALID_HANDLE_VALUE) {
            GetFileSizeEx(file, &size);
            ::ReadFile(file, head.data(), static_cast<DWORD>(head.size()), &read, nullptr);
            CloseHandle(file);
        }
        uint32_t hash = 2166136261u;
        for (DWORD i = 0; i < read; ++i) hash = (hash ^ head[i]) * 16777619u;
        char text[64];
        std::snprintf(text, sizeof(text), ":%llx:%08x", static_cast<unsigned long long>(size.QuadPart), hash);
        game += text;
    }
    // "auto" (or nothing) is the automatic delay (0); a number is that many frames.
    const std::string delayOption = option("ranga_online_atraso");
    const int delay = delayOption.empty() || delayOption == "auto" ? 0 : std::clamp(std::atoi(delayOption.c_str()), 0, 15);
    Netplay::Hooks hooks;
    hooks.stateSize = [] { return g.core.serializeSize(); };
    hooks.saveState = [](void* data, size_t size) { return g.core.serialize(data, size); };
    hooks.loadState = [](const void* data, size_t size) {
        const bool loaded = g.core.unserialize(data, size);
        if (loaded) Achievements::Reset();
        return loaded;
    };
    hooks.log = Log;
    hooks.message = [](const std::wstring& text) { ShowMessage(text, 240); };
    // Players 3 and 4 need their ports too.
    for (unsigned port = 2; port < 4; ++port) g.core.setControllerPort(port, RETRO_DEVICE_JOYPAD);
    Netplay::Start(address, game, option("ranga_online_nome"), delay, hooks);
}

int RetroMain(int argc, wchar_t** argv) {
    std::wstring corePath, romPath, title = L"jogo", optionsPath, systemDir, saveDir;
    bool fullscreen = false;
    for (int i = 1; i < argc; ++i) {
        const std::wstring arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == L"--core" && hasValue) corePath = argv[++i];
        else if (arg == L"--rom" && hasValue) romPath = argv[++i];
        else if (arg == L"--title" && hasValue) title = argv[++i];
        else if (arg == L"--system" && hasValue) systemDir = argv[++i];
        else if (arg == L"--saves" && hasValue) saveDir = argv[++i];
        else if (arg == L"--options" && hasValue) optionsPath = argv[++i];
        else if (arg == L"--controls" && hasValue) ReadControls(argv[++i]);
        else if (arg == L"--quality" && hasValue) {
            const std::wstring level = argv[++i];
            g_quality = level == L"ruim" ? 0 : level == L"fraco" ? 1 : level == L"otimo" ? 3 : 2;
        }
        else if (arg == L"--fullscreen") fullscreen = true;
        else if (arg == L"--no-game") romPath = L"<sem jogo>";
        else if (arg == L"--gpu" && hasValue) g_preferVulkan = std::wstring(argv[++i]) != L"opengl";
    }
    const bool noGame = romPath == L"<sem jogo>";
    if (corePath.empty() || romPath.empty()) {
        Log("uso: ranga_retro --core <core.dll> --rom <jogo> [--title t] [--system pasta] [--saves pasta] [--options arquivo] [--fullscreen]\n");
        return 2;
    }
    if (saveDir.empty()) saveDir = noGame ? L"." : romPath.substr(0, romPath.find_last_of(L"\\/"));
    if (systemDir.empty()) systemDir = saveDir;
    CreateDirectoryW(saveDir.c_str(), nullptr);
    // The game runs ahead of whatever else is open (browser, chat, updaters): above normal, not high, which would get
    // in the way of the sound and input drivers. Windows 11 also slows down and coarsens the timer of a program it
    // judges to be in the background; a game is told apart from that here, so a covered or unfocused window keeps pace.
    SetPriorityClass(GetCurrentProcess(), ABOVE_NORMAL_PRIORITY_CLASS);
    {
        PROCESS_POWER_THROTTLING_STATE power{};
        power.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
        power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED | PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
        power.StateMask = 0;
        // Looked up by name: the headers only declare it for Windows 8 and later. 4 is ProcessPowerThrottling.
        using Setter = BOOL(WINAPI*)(HANDLE, int, void*, DWORD);
        const auto set = reinterpret_cast<Setter>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "SetProcessInformation")));
        if (set != nullptr && !set(GetCurrentProcess(), 4, &power, sizeof(power))) {
            // Windows 10 knows only the first of the two.
            power.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
            set(GetCurrentProcess(), 4, &power, sizeof(power));
        }
    }
    g.systemDir = Utf8(systemDir);
    g.saveDir = Utf8(saveDir);
    ApplyCoreDefaults(corePath, systemDir);
    {
        std::wstring coreName = corePath.substr(corePath.find_last_of(L"\\/") + 1);
        for (auto& character : coreName) character = static_cast<wchar_t>(towlower(character));
        g_xemuPorts = coreName.rfind(L"xemu", 0) == 0;
    }
    if (!optionsPath.empty()) ReadOptionsFile(optionsPath);
    g.optionsPath = optionsPath;
    {
        const auto pick = [](const char* key, std::initializer_list<const char*> names) {
            const auto found = g.overrides.find(key);
            int index = 0;
            for (const char* name : names) {
                if (found != g.overrides.end() && found->second == name) return index;
                ++index;
            }
            return 0;
        };
        g_screenFilter = pick("ranga_filtro", {"nitido", "suave", "crt", "pixel"});
        g_screenScale = pick("ranga_escala", {"preencher", "inteira", "esticar"});
    }
    // Online play needs a game that runs the same on every PC from the same buttons: the cores' threads that run beside
    // the emulated CPU (Dolphin's dual core, PCSX2's VU1 thread) make it depend on each PC's timing, so they go off.
    if (const auto online = g.overrides.find("ranga_online"); online != g.overrides.end() && online->second == "ligado") {
        std::wstring coreName = corePath.substr(corePath.find_last_of(L"\\/") + 1);
        for (auto& character : coreName) character = static_cast<wchar_t>(towlower(character));
        if (coreName.rfind(L"dolphin", 0) == 0) g.overrides["dolphin_main_cpu_thread"] = "disabled";
        if (coreName.rfind(L"pcsx2", 0) == 0) g.overrides["pcsx2_mtvu"] = "disabled";
    }

    // Per-monitor DPI awareness, so the picture is not blurred by Windows' scaling (Windows 10 1703 and later).
    using SetAwareness = BOOL(WINAPI*)(HANDLE);
    if (auto set = reinterpret_cast<SetAwareness>(reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "SetProcessDpiAwarenessContext"))))
        set(reinterpret_cast<HANDLE>(static_cast<intptr_t>(-4)));
    SDL_SetMainReady();
    // The window is Win32, not SDL's, so SDL is told to read pads regardless of focus; the game checks focus itself.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) Log("[retro] SDL: %s\n", SDL_GetError());
    OpenPads();

    if (!g.core.Load(corePath)) {
        Log("[retro] não deu para carregar o core %s\n", Utf8(corePath).c_str());
        return 3;
    }
    // The order RetroArch uses: some cores (Mesen) make what keeps the other callbacks only in retro_init.
    g.core.setEnvironment(Environment);
    g.core.init();
    g.core.setVideo(VideoRefresh);
    g.core.setAudio(AudioSample);
    g.core.setAudioBatch(AudioBatch);
    g.core.setInputPoll(InputPoll);
    g.core.setInputState(InputState);
    retro_system_info info{};
    g.core.getSystemInfo(&info);
    Log("[retro] core %s %s\n", info.library_name, info.library_version);
    // RANGA_RETRO_LIST_OPTIONS=1 logs every option the core declared, its value now and the values it accepts, so the
    // defaults Ranga sets for a core come from the core and not from a guess.
    if (std::getenv("RANGA_RETRO_LIST_OPTIONS") != nullptr) {
        for (const auto& key : g.optionOrder) {
            std::string accepted;
            for (const auto& [value, label] : g.optionInfo[key].values) accepted += (accepted.empty() ? "" : "|") + value;
            Log("[option] %s = %s  [%s]\n", key.c_str(), g.options[key].c_str(), accepted.c_str());
        }
    }

    // A core that reads the file itself gets the path; otherwise it gets the bytes.
    const std::string romUtf8 = Utf8(romPath);
    std::vector<unsigned char> romData;
    retro_game_info game{romUtf8.c_str(), nullptr, 0, nullptr};
    if (!info.need_fullpath && !noGame) {
        romData = ReadFile(romPath);
        game.data = romData.data();
        game.size = romData.size();
    }
    // The window exists before the game loads, so a GPU context can be made on it right after.
    g_window = CreateGameWindow(title);
    // A core that never ran a game is not unloaded either: some crash in retro_deinit (Mupen64Plus) and some hang while
    // the DLL unloads at exit (Azahar), which left a process that could not be killed. The process ends right here.
    const auto endNow = [] {
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 4);
    };
    if (noGame && !g.supportsNoGame) {
        Log("[retro] este core não inicia sem jogo\n");
        endNow();
    }
    if (!g.core.loadGame(noGame ? nullptr : &game)) {
        Log("[retro] o core recusou o jogo %s\n", romUtf8.c_str());
        endNow();
    }
    g.core.setControllerPort(0, RETRO_DEVICE_JOYPAD);
    g.core.setControllerPort(1, RETRO_DEVICE_JOYPAD);
    g.core.getAvInfo(&g.av);
    Log("[retro] %ux%u, %.3f fps, som %.0f Hz\n", g.av.geometry.base_width, g.av.geometry.base_height, g.av.timing.fps, g.av.timing.sample_rate);

    std::wstring romName = noGame ? L"sem-jogo" : romPath.substr(romPath.find_last_of(L"\\/") + 1);
    romName = romName.substr(0, romName.find_last_of(L'.'));
    Saves saves{saveDir + L"\\" + romName + L".srm", saveDir + L"\\" + romName + L".state"};
    saves.LoadSram();
    StartOnline(corePath, romPath, noGame);
    if (!noGame) {
        // RetroAchievements, when the player signed in on Ranga's profile and the console has them.
        Achievements::Setup achievements;
        wchar_t program[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, program, MAX_PATH);
        achievements.programFolder = program;
        achievements.programFolder = achievements.programFolder.substr(0, achievements.programFolder.find_last_of(L"\\/"));
        if (const wchar_t* system = _wgetenv(L"RANGA_SISTEMA")) achievements.system = system;
        achievements.rom = romPath;
        achievements.data = romData.empty() ? nullptr : romData.data();
        achievements.size = romData.size();
        if (!saveDir.empty()) achievements.workspace = saveDir.substr(0, saveDir.find_last_of(L"\\/"));
        achievements.window = g_window;
        achievements.memoryData = [](unsigned id) { return g.core.memoryData(id); };
        achievements.memorySize = [](unsigned id) { return g.core.memorySize(id); };
        achievements.log = [](const char* text) { Log("%s", text); };
        Achievements::Start(achievements);
    }

    Video video;
    GlVideo glVideo;
    if (g_coreDraws) {
        // The core's renderer owns the window's surface; a swapchain of Ranga's on it would fight it.
    } else if (g_hwActive && g_hwVulkan) {
        if (!RetroVulkan::Create(g_window, g_negotiation, Log)) {
            Log("[retro] Vulkan não iniciou; tente --gpu opengl\n");
            return 5;
        }
        if (g_hw.context_reset != nullptr) g_hw.context_reset();
    } else if (g_hwActive) {
        if (!glVideo.Create(g_window, std::max(1u, g.av.geometry.max_width), std::max(1u, g.av.geometry.max_height))) {
            Log("[retro] OpenGL não iniciou\n");
            return 5;
        }
        if (g_hw.context_reset != nullptr) g_hw.context_reset();
    } else if (!video.Create(g_window)) {
        Log("[retro] Direct3D 11 não iniciou\n");
        return 5;
    }
    // What the game draws with, for the client: DLSS 5 (a Vulkan layer) is only offered for games that draw with Vulkan.
    if (!saveDir.empty()) {
        const std::wstring workspace = saveDir.substr(0, saveDir.find_last_of(L"\\/"));
        const char* api = g_coreDraws ? "proprio" : g_hwVulkan ? "vulkan" : g_hwActive ? "opengl" : "d3d11";
        if (FILE* file = _wfopen((workspace + L"\\video.txt").c_str(), L"wb")) {
            std::fputs(api, file);
            std::fclose(file);
        }
    }
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
    if (fullscreen) ToggleFullscreen();
    // RANGA_RETRO_TEXT_TEST=1 opens the keyboard box once, before the game, and logs what was typed: the box tested
    // without a game that asks for text.
    if (std::getenv("RANGA_RETRO_TEXT_TEST") != nullptr) {
        char typed[256] = {};
        ranga_text_request request{"Apelido do Mii", "Teste da caixa de texto", "Eden", 10, 0, typed, sizeof(typed), 0};
        AskText(request);
        Log("[retro] teste do texto: aceito=%d texto=%s\n", request.accepted, typed);
    }
    Audio audio;
    audio.Open();

    // One core frame per refresh when the game's rate is the screen's (within 1%), so motion never judders; otherwise the
    // core runs on its own clock and the screen shows the latest frame.
    const double refresh = DisplayRefresh();
    double accumulator = 0;
    auto last = std::chrono::steady_clock::now();
    ULONGLONG lastSave = GetTickCount64(), fpsStart = lastSave;
    int fpsFrames = 0;
    std::wstring shownTitle;
    // RANGA_RETRO_QUIT_AFTER_MS ends the game by itself, as closing the window would, for automated tests.
    const char* quitAfter = std::getenv("RANGA_RETRO_QUIT_AFTER_MS");
    const ULONGLONG quitAt = quitAfter != nullptr ? GetTickCount64() + std::strtoull(quitAfter, nullptr, 10) : 0;
    while (g_running) {
        if (quitAt != 0 && GetTickCount64() >= quitAt) g_running = false;
        // At most one WM_PAINT a turn: a core's own window that repaints itself on every paint (Xenia's) would otherwise
        // keep this loop in the message queue for good, since Windows hands out WM_PAINT whenever the queue is empty.
        MSG message;
        Achievements::Idle();
        for (int pending = 0; pending < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++pending) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_PAINT) break;
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED) OpenPads();
            else if (event.type == SDL_CONTROLLERDEVICEREMOVED) ClosePad(event.cdevice.which);
        }
        // The gamepad's PS/Guide button opens the menu too.
        static bool guideHeld = false;
        bool guide = false;
        for (auto* pad : g_pads) guide |= pad != nullptr && SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_GUIDE) != 0;
        if (guide && !guideHeld && GetForegroundWindow() == g_window) g_hotkey = VK_ESCAPE;
        guideHeld = guide;
        if (g_hotkey == VK_ESCAPE) {
            g_hotkey = 0;
            const int slot = saves.slot;
            bool fullscreen = g_fullscreen;
            const int picked = ShowMenu(saves.slot, fullscreen);
            if (saves.slot != slot) ShowMessage(L"Posição do estado: " + std::to_wstring(saves.slot + 1), 0);
            if (fullscreen != g_fullscreen) ToggleFullscreen();
            const bool online = Netplay::Active();
            if ((picked == kSave || picked == kLoad) && online) ShowMessage(L"Online: estados ficam desligados enquanto joga com amigos", 0);
            else if (picked == kSave) saves.SaveState();
            else if (picked == kLoad) saves.LoadState();
            else if (picked == kReset && online && !Netplay::Hosting()) ShowMessage(L"Online: só o anfitrião reinicia o jogo", 0);
            else if (picked == kReset) {
                g.core.reset();
                Achievements::Reset();
                if (online) Netplay::Resync();
            }
            else if (picked == kQuit) g_running = false;
            // The pause leaves no catching up and no stale sound behind.
            audio.Clear();
            last = std::chrono::steady_clock::now();
            accumulator = 0;
        }
        if ((g_hotkey == VK_F5 || g_hotkey == VK_F7) && Netplay::Active()) ShowMessage(L"Online: estados ficam desligados enquanto joga com amigos", 0);
        else if (g_hotkey == VK_F5) saves.SaveState();
        else if (g_hotkey == VK_F7) saves.LoadState();
        g_hotkey = 0;
        if (g_resized) {
            if (!g_hwActive && !g_coreDraws) video.Resize();
            if (g_hwVulkan) RetroVulkan::Resize();
            g_resized = false;
        }
        if (IsIconic(g_window)) {
            Sleep(30);
            last = std::chrono::steady_clock::now();
            continue;
        }

        const double fps = g.av.timing.fps > 0 ? g.av.timing.fps : 60.0;
        const bool locked = std::fabs(fps - refresh) / refresh < 0.01;
        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - last).count();
        last = now;
        int frames = 1;
        if (!locked) {
            accumulator = std::min(accumulator + elapsed, 0.25);
            frames = static_cast<int>(accumulator * fps);
            accumulator -= frames / fps;
        }
        const auto runFrom = std::chrono::steady_clock::now();
        // Every present takes the next frame's fence, semaphore and command buffer, a repeated frame too (a 50 Hz game
        // on a 60 Hz screen repeats one in six): presenting again on the one still in flight stalled the core's next
        // frame, and PAL games and the PS2's PAL menu ran at about 30 FPS.
        if (g_hwVulkan && (frames > 0 || !g_coreDraws)) RetroVulkan::BeginFrame();
        Netplay::Pump();
        for (int i = 0; i < frames; ++i) {
            if (Netplay::Active()) {
                Netplay::Send(LocalInput());
                Netplay::Pump();
                if (!Netplay::Ready()) {
                    // Waiting for a friend's controls: the loop goes on (window, sound), only the game holds.
                    Sleep(1);
                    break;
                }
            }
            if (g.frameTime.callback != nullptr) g.frameTime.callback(static_cast<retro_usec_t>(1000000.0 / fps));
            g.core.run();
            Netplay::FrameDone();
            Achievements::Frame();
            ++fpsFrames;
        }
        // RANGA_RETRO_STATS=1 puts the sound's loudest sample, the device queue and where a loop's time goes (the
        // core's frames, presenting them) in the log every five seconds.
        static const bool stats = std::getenv("RANGA_RETRO_STATS") != nullptr;
        static int peak = 0, loops = 0, runs = 0;
        static double runSeconds = 0, presentSeconds = 0;
        static ULONGLONG statsAt = GetTickCount64();
        const auto presentFrom = std::chrono::steady_clock::now();
        if (stats) {
            runSeconds += std::chrono::duration<double>(presentFrom - runFrom).count();
            runs += frames;
            ++loops;
            for (const int16_t sample : g.audio) peak = std::max(peak, std::abs(static_cast<int>(sample)));
            if (GetTickCount64() - statsAt > 5000) {
                Log("[retro] som: pico %d, fila %u bytes; %d voltas, %d quadros do core: %.1f ms no core e %.1f ms na tela por volta\n",
                    peak, audio.Queued(), loops, runs, loops > 0 ? runSeconds * 1000 / loops : 0.0, loops > 0 ? presentSeconds * 1000 / loops : 0.0);
                peak = loops = runs = 0;
                runSeconds = presentSeconds = 0;
                statsAt = GetTickCount64();
            }
        }
        audio.Push(g.audio, g.av.timing.sample_rate);
        g.audio.clear();
        const float aspect = g.av.geometry.aspect_ratio > 0 ? g.av.geometry.aspect_ratio : 0;
        if (g_coreDraws) {
            // Nothing to present and no swapchain to wait on: the loop keeps the core's rate by the clock instead.
            static auto next = std::chrono::steady_clock::now();
            next += std::chrono::microseconds(static_cast<long long>(1000000.0 / fps));
            const auto behind = std::chrono::steady_clock::now();
            if (next < behind) next = behind;
            else Sleep(static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(next - behind).count()));
        } else if (g_hwVulkan) {
            g.frameNew = false;
            RetroVulkan::Present(g_window, g_hwFrameWidth, g_hwFrameHeight, aspect);
        } else if (g_hwActive) {
            glVideo.Size(g.av.geometry.max_width, g.av.geometry.max_height);
            g.frameNew = false;
            glVideo.Present(g_hwFrameWidth, g_hwFrameHeight, aspect);
        } else {
            if (g.frameNew) {
                video.Upload(g.frame, g.frameWidth, g.frameHeight);
                g.frameNew = false;
            }
            video.Draw(aspect, true);
        }
        if (stats) presentSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - presentFrom).count();

        const ULONGLONG tick = GetTickCount64();
        if (tick - lastSave > 10000) {
            saves.FlushSram();
            lastSave = tick;
        }
        if (tick - fpsStart >= 1000) {
            // A core drawing by itself shows frames at its own pace; its count is the one that means anything, and a
            // core that reports none (Xenia counts no frames) gets no number rather than Ranga's loop rate.
            const double measured = g_coreDraws ? g_coreFps : fpsFrames * 1000.0 / static_cast<double>(tick - fpsStart);
            fpsFrames = 0;
            fpsStart = tick;
            wchar_t text[64] = L"";
            if (measured >= 0) std::swprintf(text, 64, L" | %.0f FPS", measured);
            std::wstring next = L"Ranga Emu - " + title + text;
            if (const std::wstring online = Netplay::Status(); !online.empty()) next += L" | " + online;
            if (tick < g.messageUntil) next += L" | " + g.message;
            if (next != shownTitle) SetWindowTextW(g_window, (shownTitle = next).c_str());
        }
    }
    saves.FlushSram();
    Netplay::Stop();
    Achievements::Stop();
    // The context goes first, while the game is still loaded: SwanStation, told its context is lost, switches to its
    // software renderer, which needs the game's system still there.
    if (g_hwActive && g_hw.context_destroy != nullptr) g_hw.context_destroy();
    g.core.unloadGame();
    g.core.deinit();
    // With DLSS 5 (ReShade's Vulkan layer and NVIDIA's DLSS) the device's teardown crashed in nvngx_dlssd.dll on
    // 14/11/2026, after everything was saved: the process ends here instead, as Windows frees all of it anyway.
    if (const char* layers = std::getenv("VK_INSTANCE_LAYERS"); g_hwVulkan && layers != nullptr && *layers != '\0') {
        Log("[retro] fim (DLSS 5: sem desmontar o Vulkan)\n");
        std::fflush(nullptr);
        TerminateProcess(GetCurrentProcess(), 0);
    }
    if (g_hwVulkan) RetroVulkan::Destroy(g_negotiation);
    for (auto* pad : g_pads) if (pad != nullptr) SDL_GameControllerClose(pad);
    SDL_Quit();
    Log("[retro] fim\n");
    return 0;
}

// A window program: no console opens when it is run by hand, and the client hands it its log handles.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return RetroMain(__argc, __wargv); }
