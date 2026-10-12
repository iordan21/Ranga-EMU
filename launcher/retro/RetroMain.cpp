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
               Bind(unserialize, "retro_unserialize") & Bind(memoryData, "retro_get_memory_data") & Bind(memorySize, "retro_get_memory_size");
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
    bool optionsChanged = false;
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
unsigned g_hwFrameWidth = 0, g_hwFrameHeight = 0;

// ------------------------------------------------------------------------------------------------------------- input

constexpr int kPorts = 2;
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

// The RetroPad follows the Super Nintendo: B below, A right, Y left, X above, which is where SDL's south, east, west and
// north buttons are on every pad it knows.
int16_t PadButton(int port, unsigned id) {
    // The game reads input only while its window is the one in front.
    if (GetForegroundWindow() != g_window) return 0;
    bool down = false;
    if (SDL_GameController* pad = g_pads[port]) {
        const auto button = [&](SDL_GameControllerButton b) { return SDL_GameControllerGetButton(pad, b) != 0; };
        const auto trigger = [&](SDL_GameControllerAxis a) { return SDL_GameControllerGetAxis(pad, a) > 12000; };
        switch (id) {
            case RETRO_DEVICE_ID_JOYPAD_B: down = button(SDL_CONTROLLER_BUTTON_A); break;
            case RETRO_DEVICE_ID_JOYPAD_A: down = button(SDL_CONTROLLER_BUTTON_B); break;
            case RETRO_DEVICE_ID_JOYPAD_Y: down = button(SDL_CONTROLLER_BUTTON_X); break;
            case RETRO_DEVICE_ID_JOYPAD_X: down = button(SDL_CONTROLLER_BUTTON_Y); break;
            case RETRO_DEVICE_ID_JOYPAD_SELECT: down = button(SDL_CONTROLLER_BUTTON_BACK); break;
            case RETRO_DEVICE_ID_JOYPAD_START: down = button(SDL_CONTROLLER_BUTTON_START); break;
            case RETRO_DEVICE_ID_JOYPAD_UP: down = button(SDL_CONTROLLER_BUTTON_DPAD_UP); break;
            case RETRO_DEVICE_ID_JOYPAD_DOWN: down = button(SDL_CONTROLLER_BUTTON_DPAD_DOWN); break;
            case RETRO_DEVICE_ID_JOYPAD_LEFT: down = button(SDL_CONTROLLER_BUTTON_DPAD_LEFT); break;
            case RETRO_DEVICE_ID_JOYPAD_RIGHT: down = button(SDL_CONTROLLER_BUTTON_DPAD_RIGHT); break;
            case RETRO_DEVICE_ID_JOYPAD_L: down = button(SDL_CONTROLLER_BUTTON_LEFTSHOULDER); break;
            case RETRO_DEVICE_ID_JOYPAD_R: down = button(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER); break;
            case RETRO_DEVICE_ID_JOYPAD_L2: down = trigger(SDL_CONTROLLER_AXIS_TRIGGERLEFT); break;
            case RETRO_DEVICE_ID_JOYPAD_R2: down = trigger(SDL_CONTROLLER_AXIS_TRIGGERRIGHT); break;
            case RETRO_DEVICE_ID_JOYPAD_L3: down = button(SDL_CONTROLLER_BUTTON_LEFTSTICK); break;
            case RETRO_DEVICE_ID_JOYPAD_R3: down = button(SDL_CONTROLLER_BUTTON_RIGHTSTICK); break;
        }
    }
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
    if (pad == nullptr || GetForegroundWindow() != g_window) return 0;
    if (index == RETRO_DEVICE_INDEX_ANALOG_BUTTON) {
        if (id == RETRO_DEVICE_ID_JOYPAD_L2) return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
        if (id == RETRO_DEVICE_ID_JOYPAD_R2) return SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
        return PadButton(port, id) ? 0x7fff : 0;
    }
    const bool left = index == RETRO_DEVICE_INDEX_ANALOG_LEFT;
    const SDL_GameControllerAxis axis = id == RETRO_DEVICE_ID_ANALOG_X ? (left ? SDL_CONTROLLER_AXIS_LEFTX : SDL_CONTROLLER_AXIS_RIGHTX)
                                                                     : (left ? SDL_CONTROLLER_AXIS_LEFTY : SDL_CONTROLLER_AXIS_RIGHTY);
    return SDL_GameControllerGetAxis(pad, axis);
}

void RETRO_CALLCONV InputPoll() {}

int16_t RETRO_CALLCONV InputState(unsigned port, unsigned device, unsigned index, unsigned id) {
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
    if (level == RETRO_LOG_DEBUG) return;
    std::fprintf(stderr, "[core %s] ", names[std::min<int>(level, 3)]);
    va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fflush(stderr);
}

void SetOption(const char* key, const char* defaultValue) {
    if (key == nullptr) return;
    const auto found = g.overrides.find(key);
    g.options[key] = found != g.overrides.end() ? found->second : defaultValue != nullptr ? defaultValue : "";
}

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
                values = semicolon == std::string::npos ? values : values.substr(semicolon + 2);
                SetOption(variable->key, values.substr(0, values.find('|')).c_str());
            }
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS:
            for (const auto* option = static_cast<const retro_core_option_definition*>(data); option->key != nullptr; ++option) SetOption(option->key, option->default_value);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_INTL:
            for (const auto* option = static_cast<const retro_core_options_intl*>(data)->us; option->key != nullptr; ++option) SetOption(option->key, option->default_value);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2:
            for (const auto* option = static_cast<const retro_core_options_v2*>(data)->definitions; option->key != nullptr; ++option) SetOption(option->key, option->default_value);
            return true;
        case RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2_INTL:
            for (const auto* option = static_cast<const retro_core_options_v2_intl*>(data)->us->definitions; option->key != nullptr; ++option) SetOption(option->key, option->default_value);
            return true;
        case RETRO_ENVIRONMENT_GET_VARIABLE: {
            auto* variable = static_cast<retro_variable*>(data);
            const auto found = g.options.find(variable->key != nullptr ? variable->key : "");
            variable->value = found != g.options.end() ? found->second.c_str() : nullptr;
            return found != g.options.end();
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
        case RETRO_ENVIRONMENT_SET_MEMORY_MAPS:
        case RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL:
        case RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME:
        case RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS:
        case RETRO_ENVIRONMENT_SET_SERIALIZATION_QUIRKS:
            return true;

        // The GPU: OpenGL, compatibility or core profile. Vulkan and Direct3D contexts are refused, and a core then
        // falls back to another renderer or says it cannot run.
        case RETRO_ENVIRONMENT_GET_PREFERRED_HW_RENDER:
            *static_cast<unsigned*>(data) = RETRO_HW_CONTEXT_OPENGL_CORE;
            return true;
        case RETRO_ENVIRONMENT_SET_HW_RENDER: {
            auto* request = static_cast<retro_hw_render_callback*>(data);
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
    }
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
    float2 range = 0.5 - 0.5 / scale;
    float2 center = frac(texel) - 0.5;
    float2 f = (center - clamp(center, -range, range)) * scale + 0.5;
    return image.Sample(smooth, (floored + f) / texSize);
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
            const D3D11_VIEWPORT viewport{(windowWidth - width) / 2, (windowHeight - height) / 2, width, height, 0, 1};
            const float values[8] = {static_cast<float>(textureWidth), static_cast<float>(textureHeight), static_cast<float>(frameWidth), static_cast<float>(frameHeight),
                                     width / static_cast<float>(frameWidth), height / static_cast<float>(frameHeight), 0, 0};
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
            if (wParam == VK_F5 || wParam == VK_F7) { g_hotkey = static_cast<int>(wParam); return 0; }
            if (message == WM_SYSKEYDOWN) break;
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
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
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
        const size_t size = g.core.memorySize(RETRO_MEMORY_SAVE_RAM);
        const auto* memory = static_cast<const unsigned char*>(g.core.memoryData(RETRO_MEMORY_SAVE_RAM));
        if (size == 0 || memory == nullptr) return;
        if (written.size() == size && std::memcmp(written.data(), memory, size) == 0) return;
        if (WriteFile(sram, memory, size)) written.assign(memory, memory + size);
    }

    void SaveState() {
        const size_t size = g.core.serializeSize();
        std::vector<unsigned char> data(size);
        const bool saved = size != 0 && g.core.serialize(data.data(), size) && WriteFile(state, data.data(), size);
        ShowMessage(saved ? L"Estado salvo (F7 carrega)" : L"Este core não salva estado", 0);
    }

    void LoadState() {
        const auto data = ReadFile(state);
        const bool loaded = !data.empty() && g.core.unserialize(data.data(), data.size());
        ShowMessage(loaded ? L"Estado carregado" : L"Nenhum estado salvo ainda (F5 salva)", 0);
    }
};

// Ranga's own defaults for a core, applied before the game's options file, which can still change any of them: the
// OpenGL renderer at about 1080p, and for SwanStation the PS1 BIOS inside the PS3 firmware Sony publishes (ps1_rom.bin,
// taken from RPCS3's dev_flash), which serves every region.
void ApplyCoreDefaults(const std::wstring& corePath) {
    std::wstring name = corePath.substr(corePath.find_last_of(L"\\/") + 1);
    for (auto& character : name) character = static_cast<wchar_t>(towlower(character));
    struct Default { const wchar_t* core; const char* key; const char* value; };
    static const Default defaults[] = {
        {L"swanstation", "swanstation_GPU_Renderer", "OpenGL"},
        {L"swanstation", "swanstation_GPU_ResolutionScale", "5"},
        {L"melondsds", "melonds_render_mode", "opengl"},
        {L"melondsds", "melonds_opengl_resolution", "4"},
        {L"mupen64plus_next", "mupen64plus-rdp-plugin", "gliden64"},
        {L"mupen64plus_next", "mupen64plus-43screensize", "1440x1080"},
        {L"swanstation", "swanstation_BIOS_PathNTSCU", "ps1_rom.bin"},
        {L"swanstation", "swanstation_BIOS_PathNTSCJ", "ps1_rom.bin"},
        {L"swanstation", "swanstation_BIOS_PathPAL", "ps1_rom.bin"},
    };
    for (const auto& entry : defaults) {
        if (name.rfind(entry.core, 0) == 0) g.overrides[entry.key] = entry.value;
    }
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
        g.overrides[key] = value;
    }
}

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
        else if (arg == L"--fullscreen") fullscreen = true;
        else if (arg == L"--no-game") romPath = L"<sem jogo>";
    }
    const bool noGame = romPath == L"<sem jogo>";
    if (corePath.empty() || romPath.empty()) {
        Log("uso: ranga_retro --core <core.dll> --rom <jogo> [--title t] [--system pasta] [--saves pasta] [--options arquivo] [--fullscreen]\n");
        return 2;
    }
    if (saveDir.empty()) saveDir = noGame ? L"." : romPath.substr(0, romPath.find_last_of(L"\\/"));
    if (systemDir.empty()) systemDir = saveDir;
    CreateDirectoryW(saveDir.c_str(), nullptr);
    g.systemDir = Utf8(systemDir);
    g.saveDir = Utf8(saveDir);
    ApplyCoreDefaults(corePath);
    if (!optionsPath.empty()) ReadOptionsFile(optionsPath);

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
    if (!g.core.loadGame(noGame ? nullptr : &game)) {
        Log("[retro] o core recusou o jogo %s\n", romUtf8.c_str());
        g.core.deinit();
        return 4;
    }
    g.core.setControllerPort(0, RETRO_DEVICE_JOYPAD);
    g.core.setControllerPort(1, RETRO_DEVICE_JOYPAD);
    g.core.getAvInfo(&g.av);
    Log("[retro] %ux%u, %.3f fps, som %.0f Hz\n", g.av.geometry.base_width, g.av.geometry.base_height, g.av.timing.fps, g.av.timing.sample_rate);

    std::wstring romName = noGame ? L"sem-jogo" : romPath.substr(romPath.find_last_of(L"\\/") + 1);
    romName = romName.substr(0, romName.find_last_of(L'.'));
    Saves saves{saveDir + L"\\" + romName + L".srm", saveDir + L"\\" + romName + L".state"};
    saves.LoadSram();

    Video video;
    GlVideo glVideo;
    if (g_hwActive) {
        if (!glVideo.Create(g_window, std::max(1u, g.av.geometry.max_width), std::max(1u, g.av.geometry.max_height))) {
            Log("[retro] OpenGL não iniciou\n");
            return 5;
        }
        if (g_hw.context_reset != nullptr) g_hw.context_reset();
    } else if (!video.Create(g_window)) {
        Log("[retro] Direct3D 11 não iniciou\n");
        return 5;
    }
    ShowWindow(g_window, SW_SHOWNORMAL);
    SetForegroundWindow(g_window);
    if (fullscreen) ToggleFullscreen();
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
        MSG message;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED) OpenPads();
            else if (event.type == SDL_CONTROLLERDEVICEREMOVED) ClosePad(event.cdevice.which);
        }
        if (g_hotkey == VK_F5) saves.SaveState();
        if (g_hotkey == VK_F7) saves.LoadState();
        g_hotkey = 0;
        if (g_resized) {
            if (!g_hwActive) video.Resize();
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
        for (int i = 0; i < frames; ++i) {
            if (g.frameTime.callback != nullptr) g.frameTime.callback(static_cast<retro_usec_t>(1000000.0 / fps));
            g.core.run();
            ++fpsFrames;
        }
        // RANGA_RETRO_STATS=1 puts the sound's loudest sample and the device queue in the log every five seconds.
        static const bool stats = std::getenv("RANGA_RETRO_STATS") != nullptr;
        static int peak = 0;
        static ULONGLONG statsAt = GetTickCount64();
        if (stats) {
            for (const int16_t sample : g.audio) peak = std::max(peak, std::abs(static_cast<int>(sample)));
            if (GetTickCount64() - statsAt > 5000) {
                Log("[retro] som: pico %d, fila %u bytes\n", peak, audio.Queued());
                peak = 0;
                statsAt = GetTickCount64();
            }
        }
        audio.Push(g.audio, g.av.timing.sample_rate);
        g.audio.clear();
        const float aspect = g.av.geometry.aspect_ratio > 0 ? g.av.geometry.aspect_ratio : 0;
        if (g_hwActive) {
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

        const ULONGLONG tick = GetTickCount64();
        if (tick - lastSave > 10000) {
            saves.FlushSram();
            lastSave = tick;
        }
        if (tick - fpsStart >= 1000) {
            const double measured = fpsFrames * 1000.0 / static_cast<double>(tick - fpsStart);
            fpsFrames = 0;
            fpsStart = tick;
            wchar_t text[64];
            std::swprintf(text, 64, L" | %.0f FPS", measured);
            std::wstring next = L"Ranga Emu - " + title + text;
            if (tick < g.messageUntil) next += L" | " + g.message;
            if (next != shownTitle) SetWindowTextW(g_window, (shownTitle = next).c_str());
        }
    }
    saves.FlushSram();
    // The context goes first, while the game is still loaded: SwanStation, told its context is lost, switches to its
    // software renderer, which needs the game's system still there.
    if (g_hwActive && g_hw.context_destroy != nullptr) g_hw.context_destroy();
    g.core.unloadGame();
    g.core.deinit();
    for (auto* pad : g_pads) if (pad != nullptr) SDL_GameControllerClose(pad);
    SDL_Quit();
    Log("[retro] fim\n");
    return 0;
}

// A window program: no console opens when it is run by hand, and the client hands it its log handles.
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) { return RetroMain(__argc, __wargv); }
