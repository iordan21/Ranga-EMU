// Ranga Emu client: a library of games of every console on the left, the chosen game on the right, with Play and its
// options, cheats and logs, and an Emulators page where each console other than PS5 gets the emulator that runs it.
#include "Common.hpp"
#include "Covers.hpp"
#include "Dlss5.hpp"
#include "Library.hpp"
#include "Runner.hpp"
#include "Systems.hpp"

#include <d3d11.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <array>
#include <map>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace {

using namespace Ranga;

constexpr const wchar_t* kWindowClass = L"RangaEmuClient";
constexpr const wchar_t* kMutex = L"Local\\RangaEmuLauncher";

ID3D11Device* g_device = nullptr;
ID3D11DeviceContext* g_context = nullptr;
IDXGISwapChain* g_swapChain = nullptr;
ID3D11RenderTargetView* g_target = nullptr;
UINT g_resizeWidth = 0, g_resizeHeight = 0;
bool g_occluded = false;
Runner* g_runner = nullptr;

void CreateTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back == nullptr) return;
    g_device->CreateRenderTargetView(back, nullptr, &g_target);
    back->Release();
}

void ReleaseTarget() {
    if (g_target != nullptr) { g_target->Release(); g_target = nullptr; }
}

bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC description{};
    description.BufferCount = 2;
    description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.BufferDesc.RefreshRate = {60, 1};
    description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    description.OutputWindow = hwnd;
    description.SampleDesc.Count = 1;
    description.Windowed = TRUE;
    description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    D3D_FEATURE_LEVEL level;
    HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &description, &g_swapChain, &g_device, &level, &g_context);
    if (result == DXGI_ERROR_UNSUPPORTED)
        result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &description, &g_swapChain, &g_device, &level, &g_context);
    if (result != S_OK) return false;
    CreateTarget();
    return true;
}

void ReleaseDevice() {
    ReleaseTarget();
    if (g_swapChain != nullptr) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context != nullptr) { g_context->Release(); g_context = nullptr; }
    if (g_device != nullptr) { g_device->Release(); g_device = nullptr; }
}

struct Texture {
    ID3D11ShaderResourceView* view = nullptr;
    int width = 0, height = 0;
    ImTextureRef Ref() const { return ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<intptr_t>(view))); }
};

// maxSide > 0 shrinks a bigger image by a whole factor (box filter): a library of box art at full size would fill the
// video memory.
Texture LoadTexture(const std::wstring& path, int maxSide = 0) {
    Texture texture;
    const std::string bytes = ReadBytes(path);
    if (bytes.empty()) return texture;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &texture.width, &texture.height, &channels, 4);
    if (pixels == nullptr) return texture;
    if (const int side = std::max(texture.width, texture.height); maxSide > 0 && side > maxSide) {
        const int factor = (side + maxSide - 1) / maxSide;
        const int width = texture.width / factor, height = texture.height / factor;
        auto* small = static_cast<unsigned char*>(STBI_MALLOC(static_cast<size_t>(width) * height * 4));
        if (small != nullptr && width > 0 && height > 0) {
            for (int y = 0; y < height; ++y) {
                for (int x = 0; x < width; ++x) {
                    unsigned sum[4] = {};
                    for (int dy = 0; dy < factor; ++dy) {
                        const unsigned char* row = pixels + (static_cast<size_t>(y * factor + dy) * texture.width + x * factor) * 4;
                        for (int dx = 0; dx < factor * 4; ++dx) sum[dx & 3] += row[dx];
                    }
                    for (int c = 0; c < 4; ++c) small[(static_cast<size_t>(y) * width + x) * 4 + c] = static_cast<unsigned char>(sum[c] / (factor * factor));
                }
            }
            stbi_image_free(pixels);
            pixels = small;
            texture.width = width;
            texture.height = height;
        } else if (small != nullptr) {
            STBI_FREE(small);
        }
    }
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(texture.width);
    description.Height = static_cast<UINT>(texture.height);
    description.MipLevels = 1;
    description.ArraySize = 1;
    description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    description.SampleDesc.Count = 1;
    description.Usage = D3D11_USAGE_DEFAULT;
    description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA data{pixels, description.Width * 4, 0};
    ID3D11Texture2D* image = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&description, &data, &image))) {
        D3D11_SHADER_RESOURCE_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        view.Texture2D.MipLevels = 1;
        g_device->CreateShaderResourceView(image, &view, &texture.view);
        image->Release();
    }
    stbi_image_free(pixels);
    return texture;
}

ImVec4 Color(unsigned rgb, float alpha = 1.0f) {
    return ImVec4(((rgb >> 16) & 0xff) / 255.0f, ((rgb >> 8) & 0xff) / 255.0f, (rgb & 0xff) / 255.0f, alpha);
}

ImU32 Packed(unsigned rgb, float alpha = 1.0f) { return ImGui::ColorConvertFloat4ToU32(Color(rgb, alpha)); }

constexpr unsigned kBackground = 0x111214, kSidebar = 0x17181b, kSurface = 0x1f2024, kHover = 0x26272c, kSelected = 0x2d2e34;
constexpr unsigned kText = 0xe8e8ea, kDim = 0x8b8d93, kAccent = 0xe5484d, kAccentHover = 0xf0595e, kGreen = 0x46a758, kAmber = 0xf5a524, kRed = 0xff6369;

void ApplyTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(0, 0);
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 10);
    style.FrameRounding = 6;
    style.ChildRounding = 0;
    style.PopupRounding = 6;
    style.GrabRounding = 6;
    style.TabRounding = 6;
    style.ScrollbarSize = 10;
    style.ScrollbarRounding = 6;
    style.TabBarBorderSize = 0;
    ImVec4* colors = style.Colors;
    colors[ImGuiCol_WindowBg] = Color(kBackground);
    colors[ImGuiCol_ChildBg] = Color(kBackground, 0);
    colors[ImGuiCol_PopupBg] = Color(kSurface);
    // The gamepad's ring is white everywhere, as on the console row; a notice dims the page behind it to black.
    colors[ImGuiCol_NavCursor] = Color(0xffffff);
    colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.6f);
    colors[ImGuiCol_Text] = Color(kText);
    colors[ImGuiCol_TextDisabled] = Color(kDim);
    colors[ImGuiCol_FrameBg] = Color(kSurface);
    colors[ImGuiCol_FrameBgHovered] = Color(kHover);
    colors[ImGuiCol_FrameBgActive] = Color(kSelected);
    colors[ImGuiCol_Button] = Color(kSurface);
    colors[ImGuiCol_ButtonHovered] = Color(kHover);
    colors[ImGuiCol_ButtonActive] = Color(kSelected);
    colors[ImGuiCol_Header] = Color(kSelected);
    colors[ImGuiCol_HeaderHovered] = Color(kHover);
    colors[ImGuiCol_HeaderActive] = Color(kSelected);
    colors[ImGuiCol_CheckMark] = Color(kAccent);
    colors[ImGuiCol_SliderGrab] = Color(kAccent);
    colors[ImGuiCol_Tab] = Color(kBackground, 0);
    colors[ImGuiCol_TabHovered] = Color(kHover);
    colors[ImGuiCol_TabSelected] = Color(kSurface);
    colors[ImGuiCol_TabSelectedOverline] = Color(kAccent);
    colors[ImGuiCol_Separator] = Color(kSurface);
    colors[ImGuiCol_ScrollbarBg] = Color(kBackground, 0);
    colors[ImGuiCol_ScrollbarGrab] = Color(kHover);
    colors[ImGuiCol_Border] = Color(kSurface);
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
}

// Each console's card on the home screen: Ranga's own art, two colors and a drawn outline of its kind of machine, not
// the makers' logos.
enum class Glyph { Pad, Handheld, Cartridge, Disc, Arcade };

struct Look {
    const wchar_t* id;
    const char* tag;
    unsigned top, bottom;
    Glyph glyph;
};

constexpr Look kLooks[] = {
    {L"PS5", "PS5", 0x3a7bf0, 0x0b1d4a, Glyph::Pad}, {L"PS4", "PS4", 0x2459b8, 0x081633, Glyph::Pad},
    {L"PS3", "PS3", 0x4a4e57, 0x0c0d10, Glyph::Pad}, {L"PS2", "PS2", 0x2b3f8f, 0x070b1f, Glyph::Pad},
    {L"PS1", "PS1", 0x9097a3, 0x2a2c33, Glyph::Disc}, {L"PSP", "PSP", 0x585c66, 0x111216, Glyph::Handheld},
    {L"SWITCH", "Switch", 0xe4002b, 0x4a0010, Glyph::Handheld}, {L"WIIU", "Wii U", 0x0aa6d6, 0x00303f, Glyph::Pad},
    {L"WII", "Wii", 0xa9bfcf, 0x2e3a44, Glyph::Pad}, {L"GC", "GC", 0x7357c0, 0x1d1238, Glyph::Pad},
    {L"3DS", "3DS", 0xd8303b, 0x3d070b, Glyph::Handheld}, {L"NDS", "DS", 0x878a93, 0x24262b, Glyph::Handheld},
    {L"GBA", "GBA", 0x6550c8, 0x170f3d, Glyph::Handheld}, {L"GB", "GB", 0x93b31c, 0x1f3a0b, Glyph::Handheld},
    {L"N64", "N64", 0x24a05a, 0x06281a, Glyph::Cartridge}, {L"SNES", "SNES", 0x7a6abd, 0x1e1838, Glyph::Cartridge},
    {L"NES", "NES", 0xd21a38, 0x3b0610, Glyph::Cartridge}, {L"MD", "MD", 0x45474d, 0x0a0a0a, Glyph::Cartridge},
    {L"SMS", "SMS", 0xd03434, 0x2d0808, Glyph::Cartridge}, {L"SEGACD", "Sega CD", 0x3a4aa0, 0x0a0f2b, Glyph::Disc},
    {L"SATURN", "Saturn", 0x4a4e94, 0x101129, Glyph::Disc}, {L"DC", "DC", 0xf47a30, 0x4a1a03, Glyph::Disc},
    {L"XBOX", "Xbox", 0x36a336, 0x0a2a0a, Glyph::Pad}, {L"X360", "360", 0x78cc56, 0x1b3a10, Glyph::Pad},
    {L"PCE", "PCE", 0xf2962a, 0x46230a, Glyph::Cartridge}, {L"A2600", "2600", 0xa8784a, 0x2b1a0c, Glyph::Cartridge},
    {L"ARCADE", "Arcade", 0xe0306e, 0x3a0620, Glyph::Arcade},
};

const Look& LookFor(const std::wstring& id) {
    for (const Look& look : kLooks) if (id == look.id) return look;
    static const Look other{L"", "?", 0x55575e, 0x16171a, Glyph::Pad};
    return other;
}

unsigned Mix(unsigned a, unsigned b, float t) {
    auto channel = [&](int shift) { return static_cast<unsigned>(((a >> shift) & 0xff) * (1 - t) + ((b >> shift) & 0xff) * t) << shift; };
    return channel(16) | channel(8) | channel(0);
}

// A rounded rectangle shaded from top to bottom.
void GradientRect(ImDrawList* draw, ImVec2 min, ImVec2 max, unsigned top, unsigned bottom, float rounding, float alpha = 1.0f) {
    const int start = draw->VtxBuffer.Size;
    draw->AddRectFilled(min, max, IM_COL32_WHITE, rounding);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(draw, start, draw->VtxBuffer.Size, min, ImVec2(min.x, max.y), Packed(top, alpha), Packed(bottom, alpha));
}

// The outline of a kind of console, centered on center, about size wide, in color; its details are cut in shade.
void DrawGlyph(ImDrawList* draw, Glyph glyph, ImVec2 c, float s, ImU32 color, ImU32 shade) {
    switch (glyph) {
        case Glyph::Pad: {
            draw->AddCircleFilled(ImVec2(c.x - s * 0.32f, c.y + s * 0.12f), s * 0.2f, color, 32);
            draw->AddCircleFilled(ImVec2(c.x + s * 0.32f, c.y + s * 0.12f), s * 0.2f, color, 32);
            draw->AddRectFilled(ImVec2(c.x - s * 0.45f, c.y - s * 0.2f), ImVec2(c.x + s * 0.45f, c.y + s * 0.14f), color, s * 0.14f);
            const float arm = s * 0.07f, length = s * 0.2f;
            const ImVec2 dpad(c.x - s * 0.27f, c.y - s * 0.03f);
            draw->AddRectFilled(ImVec2(dpad.x - length / 2, dpad.y - arm / 2), ImVec2(dpad.x + length / 2, dpad.y + arm / 2), shade, arm * 0.3f);
            draw->AddRectFilled(ImVec2(dpad.x - arm / 2, dpad.y - length / 2), ImVec2(dpad.x + arm / 2, dpad.y + length / 2), shade, arm * 0.3f);
            const ImVec2 face(c.x + s * 0.27f, c.y - s * 0.03f);
            const float r = s * 0.045f, d = s * 0.085f;
            for (const ImVec2 offset : {ImVec2(0, -d), ImVec2(0, d), ImVec2(-d, 0), ImVec2(d, 0)}) draw->AddCircleFilled(ImVec2(face.x + offset.x, face.y + offset.y), r, shade, 16);
            draw->AddCircleFilled(ImVec2(c.x - s * 0.1f, c.y + s * 0.13f), s * 0.07f, shade, 20);
            draw->AddCircleFilled(ImVec2(c.x + s * 0.1f, c.y + s * 0.13f), s * 0.07f, shade, 20);
            break;
        }
        case Glyph::Handheld: {
            const ImVec2 min(c.x - s * 0.27f, c.y - s * 0.42f), max(c.x + s * 0.27f, c.y + s * 0.42f);
            draw->AddRectFilled(min, max, color, s * 0.07f);
            draw->AddRectFilled(ImVec2(min.x + s * 0.06f, min.y + s * 0.07f), ImVec2(max.x - s * 0.06f, c.y - s * 0.02f), shade, s * 0.02f);
            const float arm = s * 0.05f, length = s * 0.16f;
            const ImVec2 dpad(c.x - s * 0.12f, c.y + s * 0.2f);
            draw->AddRectFilled(ImVec2(dpad.x - length / 2, dpad.y - arm / 2), ImVec2(dpad.x + length / 2, dpad.y + arm / 2), shade, arm * 0.3f);
            draw->AddRectFilled(ImVec2(dpad.x - arm / 2, dpad.y - length / 2), ImVec2(dpad.x + arm / 2, dpad.y + length / 2), shade, arm * 0.3f);
            draw->AddCircleFilled(ImVec2(c.x + s * 0.09f, c.y + s * 0.24f), s * 0.045f, shade, 16);
            draw->AddCircleFilled(ImVec2(c.x + s * 0.17f, c.y + s * 0.16f), s * 0.045f, shade, 16);
            break;
        }
        case Glyph::Cartridge: {
            const ImVec2 min(c.x - s * 0.34f, c.y - s * 0.4f), max(c.x + s * 0.34f, c.y + s * 0.4f);
            const ImVec2 shape[] = {min, ImVec2(max.x - s * 0.12f, min.y), ImVec2(max.x, min.y + s * 0.12f), max, ImVec2(min.x, max.y)};
            draw->AddConvexPolyFilled(shape, 5, color);
            draw->AddRectFilled(ImVec2(min.x + s * 0.09f, min.y + s * 0.1f), ImVec2(max.x - s * 0.09f, c.y + s * 0.12f), shade, s * 0.03f);
            for (int i = 0; i < 4; ++i) {
                const float x = min.x + s * (0.16f + i * 0.12f);
                draw->AddRectFilled(ImVec2(x, c.y + s * 0.22f), ImVec2(x + s * 0.06f, max.y - s * 0.05f), shade, s * 0.01f);
            }
            break;
        }
        case Glyph::Disc:
            draw->AddCircleFilled(c, s * 0.42f, color, 64);
            draw->AddCircle(c, s * 0.3f, shade, 64, s * 0.02f);
            draw->AddCircleFilled(c, s * 0.12f, shade, 32);
            draw->AddCircleFilled(c, s * 0.05f, color, 24);
            break;
        case Glyph::Arcade: {
            const ImVec2 cabinet[] = {ImVec2(c.x - s * 0.26f, c.y - s * 0.44f), ImVec2(c.x + s * 0.26f, c.y - s * 0.44f), ImVec2(c.x + s * 0.26f, c.y + s * 0.44f), ImVec2(c.x - s * 0.26f, c.y + s * 0.44f)};
            draw->AddConvexPolyFilled(cabinet, 4, color);
            draw->AddRectFilled(ImVec2(c.x - s * 0.2f, c.y - s * 0.38f), ImVec2(c.x + s * 0.2f, c.y - s * 0.28f), shade, s * 0.02f);
            draw->AddRectFilled(ImVec2(c.x - s * 0.19f, c.y - s * 0.22f), ImVec2(c.x + s * 0.19f, c.y + s * 0.06f), shade, s * 0.02f);
            draw->AddLine(ImVec2(c.x - s * 0.1f, c.y + s * 0.2f), ImVec2(c.x - s * 0.1f, c.y + s * 0.12f), shade, s * 0.03f);
            draw->AddCircleFilled(ImVec2(c.x - s * 0.1f, c.y + s * 0.11f), s * 0.04f, shade, 16);
            draw->AddCircleFilled(ImVec2(c.x + s * 0.06f, c.y + s * 0.18f), s * 0.03f, shade, 12);
            draw->AddCircleFilled(ImVec2(c.x + s * 0.15f, c.y + s * 0.18f), s * 0.03f, shade, 12);
            break;
        }
    }
}

// A soft round light of color, strongest at the center.
void Glow(ImDrawList* draw, ImVec2 center, float radius, unsigned color, float strength) {
    constexpr int kRings = 24;
    for (int i = 0; i < kRings; ++i) draw->AddCircleFilled(center, radius * (1.0f - static_cast<float>(i) / kRings), Packed(color, strength / kRings), 96);
}

// The console's card: its colors, its outline and its short name.
void ConsoleCard(ImDrawList* draw, ImFont* font, const Look& look, ImVec2 min, ImVec2 max, float scale, float alpha) {
    const float size = max.x - min.x, rounding = 14 * scale;
    GradientRect(draw, min, max, look.top, look.bottom, rounding, alpha);
    DrawGlyph(draw, look.glyph, ImVec2((min.x + max.x) / 2, min.y + size * 0.42f), size * 0.56f, Packed(0xffffff, 0.92f * alpha), Packed(look.bottom, 0.85f * alpha));
    const float text = std::max(13.0f, size * 0.13f);
    draw->AddText(font, text, ImVec2(min.x + size * 0.09f, max.y - size * 0.09f - text), Packed(0xffffff, alpha), look.tag);
}

// DualSense, Xbox and other controllers for moving around the client, through SDL like Ranga Retro. They are let go
// while a game runs, so the game's emulator has them to itself.
class Gamepads {
public:
    enum Button { Left, Right, Up, Down, Accept, Back, Count };

    ~Gamepads() { Stop(); }

    void Start() {
        if (started) return;
        SDL_SetMainReady();
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return;
        started = true;
        Open();
    }

    void Stop() {
        if (!started) return;
        for (auto* pad : pads) SDL_GameControllerClose(pad);
        pads.clear();
        SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
        started = false;
        held = {};
    }

    bool Connected() const { return !pads.empty(); }

    // Reads the controllers; pressed() then tells which buttons went down this frame, arrows repeating while held.
    void Update(bool active) {
        pressed = {};
        if (!started) return;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_CONTROLLERDEVICEADDED) Open();
            if (event.type == SDL_CONTROLLERDEVICEREMOVED) {
                for (size_t i = 0; i < pads.size(); ++i) {
                    if (SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pads[i])) == event.cdevice.which) {
                        SDL_GameControllerClose(pads[i]);
                        pads.erase(pads.begin() + static_cast<long long>(i));
                        break;
                    }
                }
            }
        }
        std::array<bool, Count> now{};
        for (auto* pad : pads) {
            const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            now[Left] = now[Left] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_LEFT) || x < -20000;
            now[Right] = now[Right] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || x > 20000;
            now[Up] = now[Up] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_UP) || y < -20000;
            now[Down] = now[Down] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_DPAD_DOWN) || y > 20000;
            // By position: Cross on a DualSense, A on an Xbox pad enters; Circle or B goes back.
            now[Accept] = now[Accept] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_A);
            now[Back] = now[Back] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_B);
        }
        // RANGA_CLIENT_TEST_INPUT=1: the number pad stands in for a gamepad (8 4 6 2 the d-pad, 5 A, 0 B), so the
        // interface tests go the gamepad's way.
        static const bool testPad = _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr;
        if (testPad) {
            const ImGuiKey keys[Count] = {ImGuiKey_Keypad4, ImGuiKey_Keypad6, ImGuiKey_Keypad8, ImGuiKey_Keypad2, ImGuiKey_Keypad5, ImGuiKey_Keypad0};
            for (int b = 0; b < Count; ++b) now[b] = now[b] || ImGui::IsKeyDown(keys[b]);
        }
        const double time = ImGui::GetTime();
        for (int b = 0; b < Count; ++b) {
            if (!now[b]) { held[b] = 0; continue; }
            if (held[b] == 0) {
                held[b] = time;
                next[b] = time + 0.4;
                pressed[b] = active;
            } else if (b <= Down && time >= next[b]) {
                next[b] = time + 0.11;
                pressed[b] = active;
            }
        }
    }

    bool Pressed(Button button) const { return pressed[button]; }
    bool Held(Button button) const { return held[button] != 0; }

private:
    void Open() {
        for (int i = 0; i < SDL_NumJoysticks(); ++i) {
            if (!SDL_IsGameController(i)) continue;
            const SDL_JoystickID id = SDL_JoystickGetDeviceInstanceID(i);
            bool open = false;
            for (auto* pad : pads) open = open || SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(pad)) == id;
            if (!open) if (auto* pad = SDL_GameControllerOpen(i)) pads.push_back(pad);
        }
    }

    bool started = false;
    std::vector<SDL_GameController*> pads;
    std::array<double, Count> held{}, next{};
    std::array<bool, Count> pressed{};
};

const char* kLogFiles[] = {"game.err", "game.log", "prepare.log", "launcher.log", "dlss5.log"};

struct Client {
    HWND window = nullptr;
    Library library;
    Runner runner;
    int selected = -1;
    Options options;
    std::map<std::wstring, Texture> icons, banners;
    ImFont* semibold = nullptr;
    int logFile = 0;
    std::string logText;
    double logRead = 0;
    bool logDirty = true;
    bool logFollow = true;
    Runner::State lastState = Runner::State::Idle;
    std::function<void()> pending;
    std::vector<Addon> addons;
    Dlss5Kit dlss5Kit;
    // The Emulators page takes the place of the game while it is open.
    bool enginesPage = false;
    // Each console's emulator, read again whenever the page changes one, so drawing never reads engines.ini.
    std::map<std::wstring, Engine> engines;
    std::wstring missingKey, missingText;
    ULONGLONG missingAt = 0;
    std::map<std::wstring, std::array<char, 1024>> argsEdit, fullscreenEdit;
    // A game file whose extension fits several consoles waits here until the user says which.
    std::wstring askFile;
    Detected askDetected;
    CoverFetcher covers;
    Gamepads gamepads;
    // Home is the row of consoles; Console the grid of one console's games; Game the chosen game.
    enum class Page { Home, Console, Game };
    Page page = Page::Home;
    std::wstring console = L"PS5";
    int gridFocus = 0;
    bool gridMoved = false;
    float homeScroll = 0, homeTarget = 0;
    // Where the gamepad is on the home screen: 0 the row of consoles, 1 the buttons under it, 2 the console's games;
    // the column within the row, and how many games the last frame had room for.
    int homeRow = 0, homeColumn = 0, homeGamesShown = 0;
    // The game page has just opened: the focus goes to Jogar.
    bool focusPlay = false, focusEngines = false;
    // Frames left in which ImGui's focus ring is kept showing (ShowRing).
    int ringFrames = 0;
    // The gamepad on the row of buttons at the top (TopBar): which one, -1 when it is elsewhere; the one A pressed
    // this frame, -1 when none. And which of the three buttons of an empty console page it is on.
    int barFocus = -1, barPress = -1, emptyFocus = 0;
    std::wstring homeConsole;

    explicit Client(const std::wstring& root) : library(root), covers(root) {}

    void RefreshEngines() {
        engines.clear();
        for (const auto& system : Systems()) {
            const Engine engine = LoadEngine(library.Root(), system.id);
            auto& args = argsEdit[system.id];
            auto& fullscreen = fullscreenEdit[system.id];
            snprintf(args.data(), args.size(), "%s", Utf8(engine.args).c_str());
            snprintf(fullscreen.data(), fullscreen.size(), "%s", Utf8(engine.fullscreen).c_str());
            engines.emplace(system.id, engine);
        }
        missingKey.clear();
    }

    const Engine& EngineFor(const std::wstring& system) {
        static const Engine none;
        const auto found = engines.find(system);
        return found != engines.end() ? found->second : none;
    }

    static bool External(const Game& game) { return game.system != L"PS5" && !ProfileFor(game.titleId).bloodborne; }

    // The BIOS, keys or firmware a console's emulator is missing (MissingFiles) for one game, or for the console's own
    // system when folder is empty. It reads folders, so the answer is kept for 2 s.
    const std::wstring& MissingFor(const std::wstring& system, const std::wstring& folder) {
        const std::wstring key = system + L"|" + folder;
        if (key != missingKey || GetTickCount64() - missingAt > 2000) {
            missingKey = key;
            missingAt = GetTickCount64();
            const Engine& engine = EngineFor(system);
            missingText = engine.ready ? MissingFiles(library.Root(), system, engine, folder.empty() ? L"" : GameRom(system, folder)) : L"";
        }
        return missingText;
    }

    Game* Current() { return selected >= 0 && selected < static_cast<int>(library.Games().size()) ? &library.Games()[static_cast<size_t>(selected)] : nullptr; }

    void Select(int index) {
        selected = index;
        logText.clear();
        logDirty = true;
        addons.clear();
        if (Game* game = Current()) {
            options = LoadOptions(*game);
            addons = LoadAddons(*game);
            dlss5Kit = InspectDlss5(library.Root());
            library.Select(game->titleId);
            // The Bloodborne port and the emulators write everything to game.log; PS5 games put their errors in game.err.
            logFile = game->system != L"PS5" ? 1 : 0;
        }
    }

    void Reload(const std::wstring& keep) {
        library.Load();
        RefreshEngines();
        int index = library.Games().empty() ? -1 : 0;
        for (size_t i = 0; i < library.Games().size(); ++i) {
            if (library.Games()[i].titleId == keep) index = static_cast<int>(i);
        }
        Select(index);
    }

    // A cover the user chose wins; then the art the game itself carries, when its console has any; then the box art
    // the client fetched (Covers.hpp).
    static std::wstring ArtFile(const Game& game, bool icon) {
        const std::wstring art = game.workspace + L"\\art";
        std::vector<std::wstring> files = {art + L"\\cover.png"};
        if (game.system == L"PS5" || game.system == L"PS4") {
            // PS4 games have their background in pic1.png.
            if (icon) files.push_back(game.folder + L"\\sce_sys\\icon0.png");
            else files.insert(files.end(), {game.folder + L"\\sce_sys\\pic0.png", game.folder + L"\\sce_sys\\pic1.png"});
        }
        if (game.system == L"PS3" && IsDirectory(game.folder)) {
            const std::wstring base = IsDirectory(game.folder + L"\\PS3_GAME") ? game.folder + L"\\PS3_GAME" : game.folder;
            files.push_back(base + (icon ? L"\\ICON0.PNG" : L"\\PIC1.PNG"));
        }
        if (game.system == L"PSP") files.push_back(art + (icon ? L"\\ICON0.PNG" : L"\\PIC1.PNG"));
        files.push_back(BoxartFile(game));
        for (const auto& file : files) if (Exists(file)) return file;
        return L"";
    }

    const Texture& Icon(const Game& game) {
        auto found = icons.find(game.titleId);
        if (found == icons.end()) found = icons.emplace(game.titleId, LoadTexture(ArtFile(game, true), 512)).first;
        if (found->second.view == nullptr) covers.Request(game);
        return found->second;
    }

    const Texture& Banner(const Game& game) {
        auto found = banners.find(game.titleId);
        if (found == banners.end()) found = banners.emplace(game.titleId, LoadTexture(ArtFile(game, false))).first;
        return found->second;
    }

    void ForgetArt(const Game& game) {
        for (auto* cache : {&icons, &banners}) {
            const auto found = cache->find(game.titleId);
            if (found == cache->end()) continue;
            if (found->second.view != nullptr) found->second.view->Release();
            cache->erase(found);
        }
    }

    void AddGame() {
        const std::wstring folder = PickFolder(window, library.Root(), L"Escolha a pasta do jogo (PS5, Bloodborne, PS3, Xbox 360 ou Wii U)");
        if (folder.empty()) return;
        std::wstring error;
        const int index = library.Add(folder, error);
        if (index < 0) {
            Notify(error);
            return;
        }
        OpenGame(index);
    }

    void AddGameFile() {
        const std::wstring path = PickFile(window, L"", L"Escolha o arquivo do jogo (ISO, ROM, NSP...)", GameFileFilters());
        if (path.empty()) return;
        const Detected found = DetectFile(path);
        if (found.systems.empty()) {
            const std::wstring text = L"O Ranga não sabe qual console abre arquivos " + (Extension(path).empty() ? std::wstring(L"sem extensão") : Extension(path)) + L":\n\n" + path;
            Notify(text);
            return;
        }
        if (found.systems.size() > 1) {
            askFile = path;
            askDetected = found;
            return;
        }
        AddDetected(path, found, found.systems[0]);
    }

    void AddRomFolder() {
        const std::wstring folder = PickFolder(window, L"", L"Escolha a pasta com os jogos (ROMs, ISOs, CHDs...)");
        if (folder.empty()) return;
        SetCursor(LoadCursorW(nullptr, IDC_WAIT));
        int added = 0, ambiguous = 0, unknown = 0;
        library.AddRomFolder(folder, added, ambiguous, unknown);
        RefreshEngines();
        std::wstring text = std::to_wstring(added) + L" jogo(s) adicionado(s).";
        if (ambiguous > 0) text += L"\n" + std::to_wstring(ambiguous) + L" arquivo(s) servem para mais de um console e o Ranga não conseguiu ler qual: adicione esses por \"Arquivo do jogo\".";
        if (unknown > 0) text += L"\n" + std::to_wstring(unknown) + L" arquivo(s) não são de nenhum console que o Ranga conhece.";
        Notify(text);
        enginesPage = false;
        page = Page::Home;
        if (selected < 0 && !library.Games().empty()) Select(0);
    }

    void AddDetected(const std::wstring& path, const Detected& found, const std::wstring& system) {
        std::wstring error;
        const int index = library.AddFile(path, system, found.serial, found.title, error);
        if (index < 0) {
            Notify(error);
            return;
        }
        OpenGame(index);
    }

    void AskSystem(float scale) {
        if (!askFile.empty() && !ImGui::IsPopupOpen("Qual console?")) ImGui::OpenPopup("Qual console?");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(20 * scale, 18 * scale));
        if (ImGui::BeginPopupModal("Qual console?", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::TextUnformatted(Utf8(FileName(askFile)).c_str());
            ImGui::TextDisabled("Este tipo de arquivo serve para mais de um console e o Ranga não conseguiu ler qual é.");
            ImGui::Dummy(ImVec2(0, 4 * scale));
            for (const auto& id : askDetected.systems) {
                if (ImGui::Button(SystemName(id).c_str(), ImVec2(320 * scale, 0))) {
                    pending = [this, id, path = askFile, found = askDetected] { AddDetected(path, found, id); };
                    askFile.clear();
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::Dummy(ImVec2(0, 4 * scale));
            if (ImGui::Button("Cancelar", ImVec2(320 * scale, 0))) {
                askFile.clear();
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    // A message or a yes/no question inside Ranga's window, in its colors and on the gamepad, instead of a Windows
    // message box: the d-pad goes between the buttons, A presses, B answers no (or closes a message).
    struct Notice {
        std::wstring text;
        // Done on Sim; a message has none and shows only OK.
        std::function<void()> yes;
    };
    std::vector<Notice> notices;

    void Notify(const std::wstring& text) { notices.push_back({text, nullptr}); }
    void Confirm(const std::wstring& text, std::function<void()> yes) { notices.push_back({text, std::move(yes)}); }

    void NoticeWindow(float scale) {
        if (notices.empty()) return;
        if (!ImGui::IsPopupOpen("##notice")) ImGui::OpenPopup("##notice");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(380 * scale, 0), ImVec2(620 * scale, FLT_MAX));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(24 * scale, 20 * scale));
        if (ImGui::BeginPopupModal("##notice", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
            const Notice& notice = notices.front();
            const bool question = static_cast<bool>(notice.yes);
            ImGui::PushTextWrapPos(570 * scale);
            ImGui::TextUnformatted(Utf8(notice.text).c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 10 * scale));
            const ImVec2 button(130 * scale, 38 * scale);
            int answer = -1;
            if (question) {
                ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(kAccent));
                if (ImGui::Button("Sim", button)) answer = 1;
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0, 12 * scale);
            }
            if (ImGui::Button(question ? "Não" : "OK", button)) answer = 0;
            // A question starts on Não, so a stray press undoes nothing.
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetItemDefaultFocus();
                ShowRing();
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false)) answer = 0;
            if (answer >= 0) {
                if (answer == 1) pending = notice.yes;
                notices.erase(notices.begin());
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void Save() {
        if (Game* game = Current()) SaveOptions(*game, options);
    }

    // What the arrows, Enter and Esc, or a controller's d-pad, Cross/A and Circle/B, asked for this frame. Nothing while
    // the window is in the back, a popup is open or a field takes the keys.
    struct Nav {
        bool left = false, right = false, up = false, down = false, accept = false, back = false, padAccept = false;
    };

    Nav ReadNav() {
        Nav nav;
        // RANGA_CLIENT_TEST_INPUT=1: keys posted to a window in the back count too (the interface tests).
        static const bool testInput = _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr;
        const bool front = testInput || GetForegroundWindow() == window;
        const bool free = front && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !ImGui::IsAnyItemActive();
        gamepads.Update(free);
        if (!free) return nav;
        nav.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || gamepads.Pressed(Gamepads::Left);
        nav.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow) || gamepads.Pressed(Gamepads::Right);
        nav.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow) || gamepads.Pressed(Gamepads::Up);
        nav.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow) || gamepads.Pressed(Gamepads::Down);
        nav.padAccept = gamepads.Pressed(Gamepads::Accept);
        nav.accept = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || nav.padAccept;
        nav.back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || gamepads.Pressed(Gamepads::Back);
        return nav;
    }

    // The consoles on the home row: PS5 first, the ones with games before the ones without.
    struct Shelf {
        std::wstring id;
        int count = 0;
    };

    std::vector<Shelf> Shelves() {
        std::vector<Shelf> shelves = {{L"PS5"}};
        for (const auto& game : library.Games()) {
            if (game.system == L"PS4") { shelves.push_back({L"PS4"}); break; }
        }
        for (const auto& system : Systems()) shelves.push_back({system.id});
        for (auto& shelf : shelves) {
            for (const auto& game : library.Games()) shelf.count += game.system == shelf.id;
        }
        std::stable_partition(shelves.begin(), shelves.end(), [](const Shelf& shelf) { return shelf.count > 0; });
        return shelves;
    }

    std::vector<int> ConsoleGames(const std::wstring& id) {
        std::vector<int> found;
        for (size_t i = 0; i < library.Games().size(); ++i) {
            if (library.Games()[i].system == id) found.push_back(static_cast<int>(i));
        }
        return found;
    }

    void OpenGame(int index) {
        Select(index);
        enginesPage = false;
        if (Game* game = Current()) {
            console = game->system;
            page = Page::Game;
            focusPlay = true;
        }
    }

    void OpenConsole() {
        page = Page::Console;
        gridFocus = 0;
        const auto games = ConsoleGames(console);
        for (size_t k = 0; k < games.size(); ++k) if (games[k] == selected) gridFocus = static_cast<int>(k);
        gridMoved = true;
    }

    // The consoles whose own system opens with no game, as tested on 13/10/2026: the PS2's menu (with its BIOS), the
    // dashboard on the Xbox's hard disk, the Switch's Mii editor. The others show nothing without a game or need files
    // from the console that are not there (the DS firmware, the Dreamcast BIOS).
    static const char* SystemButton(const std::wstring& id) {
        if (id == L"PS2") return "Abrir o menu do PS2";
        if (id == L"XBOX") return "Abrir o painel do Xbox";
        if (id == L"SWITCH") return "Abrir o editor de Mii";
        return nullptr;
    }

    // How a console's games run, in a few words for the home screen.
    std::string HowItRuns(const std::wstring& id) {
        if (id == L"PS5") return "Roda no próprio Ranga";
        if (id == L"PS4") return "Só o Bloodborne, como experimento";
        const Engine& engine = EngineFor(id);
        if (engine.native) return engine.ready ? "Roda dentro do Ranga" : "Falta: " + Utf8(engine.problem);
        if (engine.ready) return "Pelo " + (engine.presetName.empty() ? Utf8(FileName(engine.exe)) : Utf8(engine.presetName));
        return "Sem emulador: escolha um em Emuladores";
    }

    // The row of buttons at the top of the home and console pages: up from a page's first row reaches it, left and
    // right go along it, A presses, down or B leave it. True while the gamepad is there, so the page ignores it.
    bool TopBar(const Nav& nav, int count) {
        barPress = -1;
        if (barFocus < 0) return false;
        barFocus = std::min(barFocus, count - 1);
        if (nav.left && barFocus > 0) --barFocus;
        if (nav.right && barFocus + 1 < count) ++barFocus;
        if (nav.accept) barPress = barFocus;
        else if (nav.down || nav.back) barFocus = -1;
        return true;
    }

    // Keeps ImGui's focus ring showing over the next frames when a gamepad is there to move it: the focus given
    // to a page or list that just opened lands a frame later.
    void ShowRing() {
        if (gamepads.Connected() || _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr) ringFrames = 3;
    }

    // The ring on the item just drawn when the gamepad is on it.
    void FocusLast(bool on, float scale) {
        if (on) Focus(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), scale, 6 * scale);
    }

    // Adicionar jogo and Emuladores; first is Adicionar jogo's place on the top row (TopBar).
    void TopButtons(float scale, float right, int first) {
        const float wide = 170 * scale, narrow = 130 * scale, height = 36 * scale;
        ImGui::SetCursorPos(ImVec2(right - wide - narrow - 10 * scale, 22 * scale));
        if (ImGui::Button("+  Adicionar jogo", ImVec2(wide, height)) || barPress == first) ImGui::OpenPopup("##add");
        FocusLast(barFocus == first, scale);
        AddMenu(scale);
        ImGui::SameLine(0, 10 * scale);
        if (ImGui::Button("Emuladores", ImVec2(narrow, height)) || barPress == first + 1) {
            enginesPage = true;
            RefreshEngines();
        }
        FocusLast(barFocus == first + 1, scale);
    }

    // The art of a game in a box: its own square art fills it; box art of another shape sits whole on the console's
    // colors; no art at all is the console's colors with the title.
    void GameArt(ImDrawList* draw, const Game& game, ImVec2 min, ImVec2 max, float scale) {
        const Look& look = LookFor(game.system);
        const float rounding = 10 * scale, width = max.x - min.x, height = max.y - min.y;
        const Texture& icon = Icon(game);
        if (icon.view == nullptr) {
            GradientRect(draw, min, max, Mix(look.top, kBackground, 0.25f), Mix(look.bottom, kBackground, 0.3f), rounding);
            DrawGlyph(draw, look.glyph, ImVec2(min.x + width * 0.5f, min.y + height * 0.36f), width * 0.42f, Packed(0xffffff, 0.16f), Packed(look.bottom, 0.0f));
            const std::string title = Utf8(game.title);
            const float size = ImGui::GetFontSize() * 1.05f;
            ImGui::PushClipRect(min, max, true);
            draw->AddText(semibold, size, ImVec2(min.x + 12 * scale, min.y + height * 0.62f), Packed(0xffffff, 0.95f), title.c_str(), nullptr, width - 24 * scale);
            ImGui::PopClipRect();
            return;
        }
        const float imageAspect = static_cast<float>(icon.width) / static_cast<float>(icon.height), boxAspect = width / height;
        if (imageAspect > boxAspect * 0.8f && imageAspect < boxAspect * 1.25f) {
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (imageAspect < boxAspect) {
                const float visible = imageAspect / boxAspect;
                uv0.y = (1 - visible) / 2;
                uv1.y = uv0.y + visible;
            } else {
                const float visible = boxAspect / imageAspect;
                uv0.x = (1 - visible) / 2;
                uv1.x = uv0.x + visible;
            }
            draw->AddImageRounded(icon.Ref(), min, max, uv0, uv1, IM_COL32_WHITE, rounding);
            return;
        }
        GradientRect(draw, min, max, Mix(look.top, kBackground, 0.45f), Mix(look.bottom, kBackground, 0.5f), rounding);
        const float inset = 8 * scale;
        float w = width - 2 * inset, h = w / imageAspect;
        if (h > height - 2 * inset) {
            h = height - 2 * inset;
            w = h * imageAspect;
        }
        const ImVec2 at(min.x + (width - w) / 2, min.y + (height - h) / 2);
        draw->AddImageRounded(icon.Ref(), at, ImVec2(at.x + w, at.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 4 * scale);
    }

    void Focus(ImDrawList* draw, ImVec2 min, ImVec2 max, float scale, float rounding) {
        const float grow = 4 * scale;
        draw->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow), Packed(0xffffff), rounding + grow, 0, 3 * scale);
    }

    void Home(float scale, const Nav& input) {
        // On the top row (Adicionar jogo, Emuladores) the gamepad moves nothing else; up from the consoles reaches it.
        const bool inBar = TopBar(input, 2);
        const Nav nav = inBar ? Nav{} : input;
        if (!inBar && homeRow == 0 && input.up) barFocus = 0;
        const auto shelves = Shelves();
        int focus = 0;
        for (size_t i = 0; i < shelves.size(); ++i) if (shelves[i].id == console) focus = static_cast<int>(i);
        if (homeRow == 0 && nav.left && focus > 0) --focus;
        if (homeRow == 0 && nav.right && focus + 1 < static_cast<int>(shelves.size())) ++focus;
        console = shelves[static_cast<size_t>(focus)].id;
        const Shelf& shelf = shelves[static_cast<size_t>(focus)];
        if (console != homeConsole) {
            homeConsole = console;
            homeRow = homeColumn = homeGamesShown = 0;
        }
        const Engine& engine = EngineFor(console);
        const std::wstring missing = console == L"PS5" || console == L"PS4" ? L"" : MissingFor(console, L"");
        const char* systemLabel = SystemButton(console);
        const bool systemButton = systemLabel != nullptr && engine.native && engine.ready && missing.empty();
        // Down goes from the consoles to the buttons under them and on to the games; up and back return.
        bool openLibrary = false, openSystem = false;
        int openGame = -1;
        if (homeRow == 0) {
            if (nav.down) homeRow = 1, homeColumn = 0;
            else if (nav.accept) openLibrary = true;
        } else if (homeRow == 1) {
            const int buttons = systemButton ? 2 : 1;
            if (nav.left && homeColumn > 0) --homeColumn;
            if (nav.right && homeColumn + 1 < buttons) ++homeColumn;
            homeColumn = std::min(homeColumn, buttons - 1);
            if (nav.up || nav.back) homeRow = 0;
            else if (nav.down && homeGamesShown > 0) homeRow = 2, homeColumn = 0;
            else if (nav.accept) (homeColumn == 0 ? openLibrary : openSystem) = true;
        } else if (homeGamesShown == 0) {
            homeRow = 1, homeColumn = 0;
        } else {
            if (nav.left && homeColumn > 0) --homeColumn;
            if (nav.right && homeColumn + 1 < homeGamesShown) ++homeColumn;
            homeColumn = std::min(homeColumn, homeGamesShown - 1);
            if (nav.up) homeRow = 1, homeColumn = 0;
            else if (nav.back) homeRow = 0;
            else if (nav.accept) openGame = homeColumn;
        }
        if (openLibrary) pending = [this] { OpenConsole(); };
        if (openSystem && !runner.Busy()) pending = [this] { runner.StartSystem(library.Root(), console); };

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const Look& look = LookFor(console);
        GradientRect(draw, origin, ImVec2(origin.x + size.x, origin.y + size.y), Mix(look.top, kBackground, 0.5f), kBackground, 0);
        Glow(draw, ImVec2(origin.x + size.x * 0.78f, origin.y + size.y * 0.62f), size.y * 0.75f, look.top, 0.4f);

        const float left = 40 * scale;
        ImGui::SetCursorPos(ImVec2(left, 24 * scale));
        ImGui::PushFont(semibold, 24);
        ImGui::TextUnformatted("Ranga Emu");
        ImGui::PopFont();
        TopButtons(scale, size.x - left, 0);

        // The row of consoles slides so the chosen one stays in view.
        const float tile = 124 * scale, big = 160 * scale, gap = 20 * scale, rowY = origin.y + 92 * scale;
        const float view = size.x - 2 * left - big;
        const float at = focus * (tile + gap);
        if (at - homeTarget < 0) homeTarget = at;
        if (at - homeTarget > view) homeTarget = at - view;
        homeScroll += (homeTarget - homeScroll) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14);
        if (std::fabs(homeTarget - homeScroll) < 0.5f) homeScroll = homeTarget;
        for (size_t i = 0; i < shelves.size(); ++i) {
            const bool chosen = static_cast<int>(i) == focus;
            const float x = origin.x + left + i * (tile + gap) - homeScroll + (static_cast<int>(i) > focus ? big - tile : 0);
            const float side = chosen ? big : tile;
            if (x > origin.x + size.x || x + side < origin.x) continue;
            const ImVec2 min(x, rowY), max(x + side, rowY + side);
            ConsoleCard(draw, semibold, LookFor(shelves[i].id), min, max, scale, chosen || shelves[i].count > 0 ? 1.0f : 0.45f);
            if (chosen && homeRow == 0 && barFocus < 0) Focus(draw, min, max, scale, 14 * scale);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(static_cast<int>(i));
            if (ImGui::InvisibleButton("##console", ImVec2(side, side))) {
                if (chosen) pending = [this] { OpenConsole(); };
                else console = shelves[i].id;
            }
            ImGui::PopID();
        }

        float y = rowY + big + 30 * scale;
        ImGui::SetCursorScreenPos(ImVec2(origin.x + left, y));
        ImGui::BeginGroup();
        ImGui::PushFont(semibold, 40);
        ImGui::TextUnformatted(SystemName(console).c_str());
        ImGui::PopFont();
        const std::string count = shelf.count == 0 ? "Nenhum jogo ainda" : shelf.count == 1 ? "1 jogo" : std::to_string(shelf.count) + " jogos";
        ImGui::TextDisabled("%s   ·   %s", count.c_str(), HowItRuns(console).c_str());
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
        ImGui::PushFont(semibold, 18);
        if (ImGui::Button(shelf.count > 0 ? "Abrir biblioteca" : "Ver console", ImVec2(190 * scale, 44 * scale))) pending = [this] { OpenConsole(); };
        if (homeRow == 1 && homeColumn == 0) Focus(draw, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), scale, 6 * scale);
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        if (systemButton) {
            ImGui::SameLine(0, 12 * scale);
            ImGui::PushFont(semibold, 18);
            ImGui::BeginDisabled(runner.Busy());
            if (ImGui::Button(systemLabel, ImVec2(0, 44 * scale))) pending = [this] { runner.StartSystem(library.Root(), console); };
            ImGui::EndDisabled();
            if (homeRow == 1 && homeColumn == 1) Focus(draw, ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), scale, 6 * scale);
            ImGui::PopFont();
        }
        if (!missing.empty()) {
            ImGui::Dummy(ImVec2(0, 4 * scale));
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::PushTextWrapPos(origin.x + size.x - left - ImGui::GetWindowPos().x);
            ImGui::TextWrapped("%s", Utf8(missing).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();

        // A few of its games, straight to their page.
        const auto games = ConsoleGames(console);
        y = ImGui::GetItemRectMax().y + 28 * scale;
        const float cover = 112 * scale, coverGap = 14 * scale;
        homeGamesShown = 0;
        if (!games.empty() && y + cover < origin.y + size.y - 30 * scale) {
            for (size_t k = 0; k < games.size(); ++k) {
                const float x = origin.x + left + k * (cover + coverGap);
                if (x + cover > origin.x + size.x - left) break;
                ++homeGamesShown;
                const ImVec2 min(x, y), max(x + cover, y + cover);
                GameArt(draw, library.Games()[static_cast<size_t>(games[k])], min, max, scale);
                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(games[k]);
                if (ImGui::InvisibleButton("##recent", ImVec2(cover, cover)) || openGame == static_cast<int>(k)) pending = [this, index = games[k]] { OpenGame(index); };
                if (homeRow == 2 && homeColumn == static_cast<int>(k)) {
                    // The gamepad has no pointer for a tooltip to follow: the name goes under the row.
                    Focus(draw, min, max, scale * 0.6f, 10 * scale);
                    const std::string title = Utf8(library.Games()[static_cast<size_t>(games[k])].title);
                    const ImVec4 clip(origin.x + left, max.y, origin.x + size.x - left, max.y + 40 * scale);
                    draw->AddText(nullptr, ImGui::GetFontSize(), ImVec2(origin.x + left, max.y + 12 * scale), Packed(kText), title.c_str(), nullptr, 0, &clip);
                } else if (ImGui::IsItemHovered()) {
                    Focus(draw, min, max, scale * 0.6f, 10 * scale);
                    ImGui::SetTooltip("%s", Utf8(library.Games()[static_cast<size_t>(games[k])].title).c_str());
                }
                ImGui::PopID();
            }
        }
        const char* hint = "Setas ou direcional: escolher    Enter, X ou A: abrir    Esc, O ou B: voltar";
        draw->AddText(nullptr, ImGui::GetFontSize() * 0.9f, ImVec2(origin.x + left, origin.y + size.y - 30 * scale), Packed(kDim), hint);
    }

    void ConsolePage(float scale, const Nav& input) {
        const auto games = ConsoleGames(console);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const Look& look = LookFor(console);
        GradientRect(draw, origin, ImVec2(origin.x + size.x, origin.y + size.y * 0.6f), Mix(look.top, kBackground, 0.7f), kBackground, 0);
        // The top row here is Consoles, Adicionar jogo and Emuladores.
        const bool inBar = TopBar(input, 3);
        const Nav nav = inBar ? Nav{} : input;
        if (nav.back) {
            page = Page::Home;
            return;
        }

        const float left = 32 * scale;
        ImGui::SetCursorPos(ImVec2(left, 22 * scale));
        if (ImGui::Button("<  Consoles", ImVec2(0, 36 * scale)) || barPress == 0) pending = [this] { page = Page::Home; };
        FocusLast(barFocus == 0, scale);
        ImGui::SameLine(0, 18 * scale);
        ImGui::PushFont(semibold, 28);
        ImGui::TextUnformatted(SystemName(console).c_str());
        ImGui::PopFont();
        ImGui::SameLine(0, 12 * scale);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 10 * scale);
        ImGui::TextDisabled("%s", games.size() == 1 ? "1 jogo" : (std::to_string(games.size()) + " jogos").c_str());
        TopButtons(scale, size.x - left, 1);

        if (games.empty()) {
            // Left and right go along the three ways to add a game; up reaches the top row.
            if (nav.left && emptyFocus > 0) --emptyFocus;
            if (nav.right && emptyFocus < 2) ++emptyFocus;
            if (nav.up) barFocus = 0;
            const bool here = barFocus < 0 && !inBar;
            const int pressed = here && nav.accept ? emptyFocus : -1;
            ImGui::SetCursorPos(ImVec2(left, 110 * scale));
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 22);
            ImGui::TextUnformatted("Nenhum jogo deste console ainda");
            ImGui::PopFont();
            ImGui::TextDisabled("%s", HowItRuns(console).c_str());
            ImGui::Dummy(ImVec2(0, 6 * scale));
            if (ImGui::Button("Pasta do jogo...", ImVec2(190 * scale, 38 * scale)) || pressed == 0) pending = [this] { AddGame(); };
            FocusLast(here && emptyFocus == 0, scale);
            ImGui::SameLine();
            if (ImGui::Button("Arquivo do jogo...", ImVec2(190 * scale, 38 * scale)) || pressed == 1) pending = [this] { AddGameFile(); };
            FocusLast(here && emptyFocus == 1, scale);
            ImGui::SameLine();
            if (ImGui::Button("Pasta de ROMs...", ImVec2(190 * scale, 38 * scale)) || pressed == 2) pending = [this] { AddRomFolder(); };
            FocusLast(here && emptyFocus == 2, scale);
            ImGui::EndGroup();
            return;
        }

        const float tile = 168 * scale, gap = 22 * scale, label = ImGui::GetFontSize() * 2.7f;
        ImGui::SetCursorPos(ImVec2(left, 80 * scale));
        ImGui::BeginChild("##grid", ImVec2(size.x - left, 0), ImGuiChildFlags_None);
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x - left + gap) / (tile + gap)));
        const int total = static_cast<int>(games.size());
        gridFocus = std::clamp(gridFocus, 0, total - 1);
        const int before = gridFocus;
        if (nav.left && gridFocus > 0) --gridFocus;
        if (nav.right && gridFocus + 1 < total) ++gridFocus;
        if (nav.up && gridFocus - columns >= 0) gridFocus -= columns;
        else if (nav.up) barFocus = 0;
        if (nav.down) gridFocus = std::min(total - 1, gridFocus + columns);
        if (gridFocus != before) gridMoved = true;
        if (nav.accept) pending = [this, index = games[static_cast<size_t>(gridFocus)]] { OpenGame(index); };

        ImDrawList* grid = ImGui::GetWindowDrawList();
        for (int k = 0; k < total; ++k) {
            const int index = games[static_cast<size_t>(k)];
            const Game& game = library.Games()[static_cast<size_t>(index)];
            ImGui::SetCursorPos(ImVec2((k % columns) * (tile + gap) + 6 * scale, (k / columns) * (tile + label + gap) + 6 * scale));
            ImGui::PushID(index);
            if (ImGui::InvisibleButton("##game", ImVec2(tile, tile + label))) pending = [this, index] { OpenGame(index); };
            const bool hovered = ImGui::IsItemHovered();
            if (gridMoved && k == gridFocus) {
                ImGui::SetScrollHereY(0.5f);
                gridMoved = false;
            }
            ImGui::PopID();
            const ImVec2 min = ImGui::GetItemRectMin(), max(min.x + tile, min.y + tile);
            if (!ImGui::IsRectVisible(min, ImVec2(max.x, max.y + label))) continue;
            GameArt(grid, game, min, max, scale);
            if (k == gridFocus && barFocus < 0) Focus(grid, min, max, scale, 10 * scale);
            else if (hovered) grid->AddRect(min, max, Packed(0xffffff, 0.5f), 10 * scale, 0, 2 * scale);
            // A game that cannot start says why on its art.
            std::string problem;
            if (!GameAvailable(game)) problem = "não encontrado";
            else if (External(game) && !EngineFor(game.system).ready) problem = "falta emulador";
            if (!problem.empty()) {
                const float small = ImGui::GetFontSize() * 0.8f;
                const ImVec2 extent = ImGui::GetFont()->CalcTextSizeA(small, FLT_MAX, 0, problem.c_str());
                const ImVec2 badge(min.x + 8 * scale, min.y + 8 * scale);
                grid->AddRectFilled(badge, ImVec2(badge.x + extent.x + 12 * scale, badge.y + extent.y + 6 * scale), Packed(0x000000, 0.7f), 6 * scale);
                grid->AddText(nullptr, small, ImVec2(badge.x + 6 * scale, badge.y + 3 * scale), Packed(GameAvailable(game) ? kAmber : kRed), problem.c_str());
            }
            const std::string title = Utf8(game.title);
            ImGui::PushClipRect(ImVec2(min.x, max.y), ImVec2(max.x, max.y + label - 4 * scale), true);
            grid->AddText(semibold, ImGui::GetFontSize(), ImVec2(min.x + 2 * scale, max.y + 8 * scale), Packed(k == gridFocus ? 0xffffff : kText), title.c_str(), nullptr, tile - 4 * scale);
            ImGui::PopClipRect();
        }
        ImGui::Dummy(ImVec2(0, 30 * scale));
        ImGui::EndChild();
    }

    void AddMenu(float scale) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * scale, 8 * scale));
        if (ImGui::BeginPopup("##add")) {
            if (ImGui::Selectable("Pasta do jogo...")) pending = [this] { AddGame(); };
            // Opened from the gamepad, the list starts on its first entry, with its ring showing.
            if (ImGui::IsWindowAppearing()) {
                ImGui::SetItemDefaultFocus();
                ShowRing();
            }
            ImGui::TextDisabled("   PS5, Bloodborne, PS3, Xbox 360, Wii U");
            ImGui::Separator();
            if (ImGui::Selectable("Arquivo do jogo...")) pending = [this] { AddGameFile(); };
            ImGui::TextDisabled("   ISO, CHD, ROM, NSP, WBFS e outros");
            ImGui::Separator();
            if (ImGui::Selectable("Pasta de ROMs (vários jogos)...")) pending = [this] { AddRomFolder(); };
            ImGui::TextDisabled("   Todos os jogos da pasta de uma vez");
            ImGui::EndPopup();
        }
        ImGui::PopStyleVar();
    }

    void OptionsTab(const Game& game, const Profile& profile, float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        const float labelWidth = 150 * scale, fieldWidth = 260 * scale;
        auto label = [&](const char* text) {
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(text);
            ImGui::SameLine(labelWidth);
            ImGui::SetNextItemWidth(fieldWidth);
        };
        const char* modes[] = {"Janela", "Tela cheia sem borda"};
        int mode = options.borderless ? 1 : 0;
        label("Tela");
        if (ImGui::Combo("##mode", &mode, modes, 2)) { options.borderless = mode == 1; Save(); }
        if (profile.sonsOfSparta) {
            const char* resolutions[] = {"Manter a do jogo", "3840 x 2160 (4K)", "2560 x 1440", "1920 x 1080"};
            const int indexes[] = {-1, 0, 6, 8};
            int current = 0;
            for (int i = 0; i < 4; ++i) if (indexes[i] == options.resolution) current = i;
            label("Resolução");
            if (ImGui::Combo("##resolution", &current, resolutions, 4)) { options.resolution = indexes[current]; Save(); }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::TextDisabled("Resoluções menores aliviam a placa de vídeo.");
            ImGui::Dummy(ImVec2(0, 2 * scale));
            if (ImGui::Checkbox("Pular a intro", &options.skipIntro)) Save();
            if (ImGui::Checkbox("Pular diálogos", &options.skipDialogue)) Save();
            if (options.skipDialogue) {
                ImGui::SameLine();
                ImGui::TextDisabled("X / A / Enter pula a fala atual");
            }
        } else if (profile.bloodborne) {
            const char* rates[] = {"30 FPS (original)", "60 FPS", "90 FPS", "Sem limite"};
            const wchar_t* rateValues[] = {L"30", L"60", L"90", L"uncap"};
            int rate = 1;
            for (int i = 0; i < 4; ++i) if (options.fps == rateValues[i]) rate = i;
            label("Quadros");
            if (ImGui::Combo("##fps", &rate, rates, 4)) { options.fps = rateValues[rate]; Save(); }
            const char* renders[] = {"Nativa (1920 x 1080)", "1280 x 720 (mais leve)", "2560 x 1440", "3840 x 2160 (4K)"};
            const wchar_t* renderValues[] = {L"", L"1280x720", L"2560x1440", L"3840x2160"};
            int render = 0;
            for (int i = 0; i < 4; ++i) if (options.renderResolution == renderValues[i]) render = i;
            label("Resolução");
            if (ImGui::Combo("##render", &render, renders, 4)) { options.renderResolution = renderValues[render]; Save(); }
            ImGui::Dummy(ImVec2(0, 2 * scale));
            if (ImGui::Checkbox("Pular a intro", &options.skipIntro)) Save();
            ImGui::Dummy(ImVec2(0, 4 * scale));
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Experimento: o Ranga não roda jogos de PS4, só este. Roda pelo port nativo do Bloodborne (deadinside28, Mrsuss60 e shadPS4, licença GPL), não pelo emulador de PS5. Precisa da versão 1.09 com a atualização mesclada na pasta do jogo e do Python 3.10 ou mais novo.");
            ImGui::TextDisabled("No jogo, Insert ou L3 + R3 abre o menu de gráficos e FSR.");
            ImGui::PopTextWrapPos();
        } else if (External(game)) {
            const Engine& engine = EngineFor(game.system);
            label("Emulador");
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(engine.exe.empty() ? "nenhum escolhido" : engine.presetName.empty() ? Utf8(FileName(engine.exe)).c_str() : Utf8(engine.presetName).c_str());
            ImGui::SameLine();
            if (ImGui::Button("Emuladores...")) {
                enginesPage = true;
                RefreshEngines();
            }
            if (const std::wstring& problem = engine.ready ? MissingFor(game.system, game.folder) : engine.problem; !problem.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                ImGui::TextWrapped("%s", Utf8(problem).c_str());
                ImGui::PopStyleColor();
            }
            if (const System* system = FindSystem(game.system)) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("%s", system->note.c_str());
                ImGui::PopTextWrapPos();
            }
        } else {
            ImGui::Dummy(ImVec2(0, 2 * scale));
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextWrapped("Jogo experimental: o Ranga Emu só foi testado com God of War Sons of Sparta. Este jogo pode travar ou nem abrir. Se der erro, a aba Log mostra o que aconteceu.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        Dlss5Section(game);
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        const bool isFile = !IsDirectory(game.folder) && game.system != L"PS5" && game.system != L"PS4";
        ImGui::TextDisabled(isFile ? "Arquivo do jogo" : "Pasta do jogo");
        ImGui::TextWrapped("%s", Utf8(game.folder).c_str());
        if (ImGui::Button("Abrir pasta")) {
            if (isFile) ShellExecuteW(window, L"open", L"explorer.exe", (L"/select,\"" + game.folder + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            else ShellExecuteW(window, L"open", game.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
        ImGui::SameLine();
        if (ImGui::Button(isFile ? "Trocar arquivo..." : "Trocar pasta...")) {
            const std::wstring start = isFile ? ParentFolder(game.folder) : game.folder;
            const std::wstring path = isFile ? PickFile(window, start, L"Escolha o novo arquivo do jogo", GameFileFilters()) : PickFolder(window, start, L"Escolha a nova pasta do jogo");
            if (!path.empty()) {
                pending = [this, path, titleId = game.titleId, system = game.system] {
                    // PS5 and PS4 games are found again by their title id; other consoles keep their entry and workspace.
                    if (system == L"PS5" || system == L"PS4") {
                        std::wstring error;
                        const int index = library.Add(path, error);
                        if (index < 0) Notify(error);
                        else OpenGame(index);
                        return;
                    }
                    library.Relocate(titleId, path);
                    Reload(titleId);
                };
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Capa...")) {
            const std::wstring image = PickFile(window, L"", L"Escolha uma imagem PNG para a capa", {{L"Imagem PNG", L"*.png"}});
            if (!image.empty()) {
                pending = [this, image] {
                    Game* current = Current();
                    if (current == nullptr) return;
                    CreateDirectoryW((library.Root() + L"\\games").c_str(), nullptr);
                    CreateDirectoryW(current->workspace.c_str(), nullptr);
                    CreateDirectoryW((current->workspace + L"\\art").c_str(), nullptr);
                    CopyFileW(image.c_str(), (current->workspace + L"\\art\\cover.png").c_str(), FALSE);
                    ForgetArt(*current);
                };
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Remover da lista")) {
            const std::wstring question = L"Remover " + game.title + L" da lista?\n\nOs arquivos do jogo e os saves não são apagados.";
            Confirm(question, [this, index = static_cast<size_t>(selected)] {
                library.Remove(index);
                Reload(L"");
                // Back to the console's games, or to the consoles when it was the last one.
                if (ConsoleGames(console).empty()) page = Page::Home;
                else OpenConsole();
            });
        }
    }

    void Dlss5Section(const Game& game) {
        // Its files go in and out when the game starts, so the switch only changes while the game is closed.
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        if (ImGui::Checkbox("DLSS 5 (experimental)", &options.dlss5)) {
            dlss5Kit = InspectDlss5(library.Root());
            Save();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (running) {
            ImGui::TextDisabled("Feche o jogo para ligar ou desligar.");
        } else if (dlss5Kit.ready) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
            ImGui::TextUnformatted(options.dlss5 ? "ligado, arquivos prontos" : "arquivos prontos");
            ImGui::PopStyleColor();
        } else {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(options.dlss5 ? kAmber : kDim));
            ImGui::TextWrapped("Ainda não dá: %s", Utf8(dlss5Kit.problem).c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Como o DLSS 5 Swapper faz com emuladores: ReShade + DLSS5-Feeder + um add-on neural, com os arquivos que você colocar na pasta do DLSS 5. O Ranga não baixa nenhum deles. Só placa RTX 50, e pesa: o FPS pode cair perto da metade.");
        if (External(game)) ImGui::TextDisabled("Em outro emulador, só pega se ele estiver desenhando com Vulkan (escolha Vulkan nas configurações de vídeo dele).");
        ImGui::PopTextWrapPos();
        if (ImGui::Button("Abrir pasta do DLSS 5")) {
            CreateDlss5Folder(library.Root());
            ShellExecuteW(window, L"open", Dlss5Folder(library.Root()).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            dlss5Kit = InspectDlss5(library.Root());
        }
        ImGui::SameLine();
        if (ImGui::Button("Verificar de novo")) dlss5Kit = InspectDlss5(library.Root());
    }

    void CheatsTab(float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::TextDisabled("Marcados aqui já começam ligados. As teclas ligam e desligam durante o jogo.");
        ImGui::Dummy(ImVec2(0, 2 * scale));
        bool changed = false;
        changed |= ImGui::Checkbox("Modo deus (F1)", &options.godMode);
        changed |= ImGui::Checkbox("Spartan Spirit infinito (F2)", &options.infiniteSpirit);
        changed |= ImGui::Checkbox("Magia infinita (F3)", &options.infiniteMagic);
        changed |= ImGui::Checkbox("Velocidade 2x (F5)", &options.movementSpeed);
        changed |= ImGui::Checkbox("Pulo 2x (F6)", &options.jumpHeight);
        const char* damages[] = {"Desligado", "2x", "4x", "6x"};
        const int factors[] = {1, 2, 4, 6};
        int current = 0;
        for (int i = 0; i < 4; ++i) if (factors[i] == options.damageMultiplier) current = i;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Multiplicador de dano (F4)");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(130 * scale);
        if (ImGui::Combo("##damage", &current, damages, 4)) { options.damageMultiplier = factors[current]; changed = true; }
        if (changed) Save();
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::TextDisabled("F7  +1000 Blood Orbs");
        ImGui::TextDisabled("F8  +10 materiais de melhoria");
    }

    void DlcTab(const Game& game, float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        // The runtime reads the installed add-ons when the game starts, so they change only while it is closed.
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        if (ImGui::Button("+  Instalar DLC...")) {
            const std::wstring folder = PickFolder(window, game.folder, L"Escolha a pasta da DLC (a que tem sce_sys\\param.json)");
            if (!folder.empty()) {
                pending = [this, folder] {
                    Game* current = Current();
                    if (current == nullptr) return;
                    const std::wstring error = InstallAddon(*current, folder);
                    if (!error.empty()) Notify(error);
                    addons = LoadAddons(*current);
                };
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", running ? "Feche o jogo para instalar ou remover DLC." : "A DLC precisa ter sido tirada do seu console.");
        ImGui::Dummy(ImVec2(0, 4 * scale));
        if (addons.empty()) {
            ImGui::TextDisabled("Nenhuma DLC instalada.");
            return;
        }
        for (size_t i = 0; i < addons.size(); ++i) {
            const Addon& addon = addons[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 0.0f);
            ImGui::TextUnformatted(Utf8(addon.title).c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s   ·   %s", Utf8(addon.label).c_str(), addon.hasData ? "com arquivos" : "só licença");
            if (addon.hasData && !IsDirectory(addon.folder)) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kRed));
                ImGui::TextUnformatted("pasta não encontrada");
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 90 * scale);
            ImGui::BeginDisabled(running);
            if (ImGui::Button("Remover", ImVec2(90 * scale, 0))) {
                const std::wstring question = L"Remover a DLC " + addon.title + L"?\n\nA pasta dela não é apagada.";
                Confirm(question, [this, label = addon.label] {
                    Game* current = Current();
                    if (current == nullptr) return;
                    RemoveAddon(*current, label);
                    addons = LoadAddons(*current);
                });
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

    void LogTab(const Game& game, float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::SetNextItemWidth(160 * scale);
        if (ImGui::Combo("##logfile", &logFile, kLogFiles, IM_ARRAYSIZE(kLogFiles))) logDirty = true;
        ImGui::SameLine();
        if (ImGui::Button("Atualizar")) logDirty = true;
        ImGui::SameLine();
        if (ImGui::Button("Abrir pasta de logs")) ShellExecuteW(window, L"open", (game.workspace + L"\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        ImGui::Checkbox("Seguir o fim", &logFollow);
        // Re-read every second while this game runs, so the tab follows it live.
        const double now = ImGui::GetTime();
        const bool live = runner.Busy() && runner.TitleId() == game.titleId;
        if (logDirty || (live && now - logRead > 1.0)) {
            logDirty = false;
            logText = ReadTail(game.workspace + L"\\logs\\" + Wide(kLogFiles[logFile]), 96 * 1024);
            if (logText.empty()) logText = "(vazio ou ainda não existe)";
            logRead = now;
        }
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(kSidebar));
        ImGui::BeginChild("##logtext", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
        ImGui::TextUnformatted(logText.c_str(), logText.c_str() + logText.size());
        ImGui::PopStyleVar();
        if (logFollow) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void EnginesPage(float scale) {
        const float pad = 32 * scale;
        ImGui::BeginChild("##engines", ImVec2(0, 0), ImGuiChildFlags_None);
        ImGui::SetCursorPos(ImVec2(pad, 24 * scale));
        ImGui::BeginGroup();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - pad);
        ImGui::PushFont(semibold, 28);
        ImGui::TextUnformatted("Emuladores");
        ImGui::PopFont();
        ImGui::TextDisabled("Jogos de PS5 rodam no Ranga e o Bloodborne, como experimento, no port nativo; outros jogos de PS4 não rodam. Cada outro console roda no emulador que você escolher aqui. O Ranga não baixa nenhum: baixe do site oficial, extraia onde quiser e aponte o .exe. BIOS, firmware e chaves vêm do seu próprio console.");
        ImGui::Dummy(ImVec2(0, 6 * scale));
        // The page opens with the focus on Voltar, so the gamepad has a place to move from.
        if (focusEngines) {
            ImGui::SetKeyboardFocusHere();
            focusEngines = false;
            ShowRing();
        }
        if (ImGui::Button("Voltar")) enginesPage = false;
        ImGui::SameLine();
        if (ImGui::Button("Verificar de novo")) RefreshEngines();
        ImGui::Dummy(ImVec2(0, 6 * scale));
        // A RetroArch chosen for one console can serve every console its cores cover.
        std::wstring retroArch;
        for (const auto& [id, engine] : engines) {
            if (Lower(FileName(engine.exe)).rfind(L"retroarch", 0) == 0) retroArch = engine.exe;
        }
        for (const auto& system : Systems()) {
            const Engine& engine = EngineFor(system.id);
            ImGui::PushID(Utf8(system.id).c_str());
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 2 * scale));
            ImGui::PushFont(semibold, 0.0f);
            ImGui::TextUnformatted(system.name.c_str());
            ImGui::PopFont();
            ImGui::SameLine();
            if (engine.ready) {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
                ImGui::TextUnformatted(engine.presetName.empty() ? "pronto (argumentos genéricos)" : ("pronto  ·  " + Utf8(engine.presetName)).c_str());
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(engine.exe.empty() ? kDim : kAmber));
                ImGui::TextUnformatted(engine.exe.empty() ? "sem emulador" : "precisa de atenção");
            }
            ImGui::PopStyleColor();
            if (engine.nativeAvailable) {
                bool native = engine.native;
                if (ImGui::Checkbox("Rodar dentro do Ranga (nativo)", &native)) pending = [this, id = system.id, native] { SetNative(library.Root(), id, native); RefreshEngines(); };
            }
            if (engine.native) {
                ImGui::TextDisabled("O jogo roda no Ranga Retro, com o core %s, sem abrir outro emulador. F11 tela cheia, F5 salva estado, F7 carrega.", Utf8(system.nativeCore).c_str());
                if (!engine.ready) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                    ImGui::TextUnformatted(Utf8(engine.problem).c_str());
                    ImGui::PopStyleColor();
                }
                if (!engine.bios.empty()) ImGui::TextDisabled("BIOS: %s", Utf8(engine.bios).c_str());
            } else {
                if (!engine.exe.empty()) ImGui::TextDisabled("%s", Utf8(engine.exe).c_str());
                if (!engine.ready && !engine.exe.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                    ImGui::TextUnformatted(Utf8(engine.problem).c_str());
                    ImGui::PopStyleColor();
                }
                if (ImGui::Button(engine.exe.empty() ? "Escolher emulador..." : "Trocar...")) {
                    const std::wstring start = engine.exe.empty() ? L"" : ParentFolder(engine.exe);
                    const std::wstring exe = PickFile(window, start, (L"Escolha o .exe do emulador de " + Wide(system.name)).c_str(), {{L"Programa", L"*.exe"}});
                    if (!exe.empty()) pending = [this, id = system.id, exe] { ChooseEngine(library.Root(), id, exe); RefreshEngines(); };
                }
                if (!engine.exe.empty()) {
                    ImGui::SameLine();
                    if (ImGui::Button("Abrir pasta")) ShellExecuteW(window, L"open", ParentFolder(engine.exe).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    ImGui::SameLine();
                    if (ImGui::Button("Tirar")) pending = [this, id = system.id] { ClearEngine(library.Root(), id); RefreshEngines(); };
                    if (engine.tunable) {
                        ImGui::SameLine();
                        // It rewrites the emulator's own settings, which the emulator overwrites when it closes, so it waits for no game.
                        ImGui::BeginDisabled(runner.Busy());
                        if (ImGui::Button("Ajustar para este PC")) {
                            pending = [this, id = system.id, name = system.name] {
                                const std::wstring report = TuneEngine(library.Root(), id);
                                const std::wstring text = L"Ajustes de " + Wide(name) + L" (com o emulador fechado):\n\n" + report;
                                Notify(text);
                                RefreshEngines();
                            };
                        }
                        ImGui::EndDisabled();
                    }
                }
                if (!engine.bios.empty()) {
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("BIOS: %s", Utf8(engine.bios).c_str());
                    ImGui::SameLine();
                    if (ImGui::Button("Abrir pasta da BIOS")) {
                        CreateDirectoryW((library.Root() + L"\\bios").c_str(), nullptr);
                        CreateDirectoryW(engine.bios.c_str(), nullptr);
                        ShellExecuteW(window, L"open", engine.bios.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
                    }
                }
                if (engine.exe.empty() && !retroArch.empty()) {
                    for (const auto& preset : system.presets) {
                        if (preset.exePrefixes != L"retroarch") continue;
                        ImGui::SameLine();
                        if (ImGui::Button("Usar o RetroArch")) pending = [this, id = system.id, retroArch] { ChooseEngine(library.Root(), id, retroArch); RefreshEngines(); };
                        break;
                    }
                }
                if (engine.exe.empty()) {
                    std::string names;
                    for (const auto& preset : system.presets) {
                        const std::string name = preset.exePrefixes == L"retroarch" ? "RetroArch" : Utf8(preset.name);
                        if (names.find(name) == std::string::npos) names += (names.empty() ? "" : ", ") + name;
                    }
                    ImGui::TextDisabled("Funciona com: %s", names.c_str());
                } else {
                    // The arguments stay editable: a new emulator version may want others than the ones the client knows.
                    auto& args = argsEdit[system.id];
                    auto& fullscreen = fullscreenEdit[system.id];
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Argumentos");
                    ImGui::SameLine(110 * scale);
                    ImGui::SetNextItemWidth(std::max(200 * scale, ImGui::GetContentRegionAvail().x - pad - 90 * scale));
                    ImGui::InputText("##args", args.data(), args.size());
                    const bool argsDone = ImGui::IsItemDeactivatedAfterEdit();
                    ImGui::SameLine();
                    if (ImGui::Button("Padrão")) pending = [this, id = system.id] { ResetEngineArgs(library.Root(), id); RefreshEngines(); };
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("Tela cheia");
                    ImGui::SameLine(110 * scale);
                    ImGui::SetNextItemWidth(260 * scale);
                    ImGui::InputText("##fullscreen", fullscreen.data(), fullscreen.size());
                    const bool fullscreenDone = ImGui::IsItemDeactivatedAfterEdit();
                    if (argsDone || fullscreenDone) {
                        pending = [this, id = system.id, a = std::string(args.data()), f = std::string(fullscreen.data())] {
                            SaveEngineArgs(library.Root(), id, Wide(a), Wide(f));
                            RefreshEngines();
                        };
                    }
                    ImGui::TextDisabled("{rom} é o jogo; {fullscreen} vira o argumento de tela cheia quando o jogo está em tela cheia.");
                }
            }
            ImGui::TextDisabled("%s", system.note.c_str());
            ImGui::Dummy(ImVec2(0, 4 * scale));
            ImGui::PopID();
        }
        ImGui::PopTextWrapPos();
        ImGui::EndGroup();
        ImGui::Dummy(ImVec2(0, 20 * scale));
        ImGui::EndChild();
    }

    void Play(Game& game) {
        Save();
        RefreshEngines();
        runner.Start(library.Root(), game, options);
        logDirty = true;
    }

    void Detail(float scale, const Nav& nav) {
        Game* game = Current();
        if (game == nullptr) {
            page = Page::Home;
            return;
        }
        if (nav.back) {
            pending = [this] { OpenConsole(); };
            return;
        }
        ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_None);
        const Profile profile = ProfileFor(game->titleId);
        const float width = ImGui::GetContentRegionAvail().x;
        const float bannerHeight = std::min(300 * scale, ImGui::GetContentRegionAvail().y * 0.4f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const Texture& banner = Banner(*game);
        if (banner.view == nullptr || banner.width < banner.height * 1.3f) {
            // No wide art of its own: the console's colors, with the box art standing on the right.
            const Look& look = LookFor(game->system);
            GradientRect(draw, origin, ImVec2(origin.x + width, origin.y + bannerHeight), Mix(look.top, kBackground, 0.35f), Mix(look.bottom, kBackground, 0.4f), 0);
            Glow(draw, ImVec2(origin.x + width * 0.7f, origin.y + bannerHeight * 0.4f), bannerHeight * 1.4f, look.top, 0.25f);
            if (const Texture& poster = Icon(*game); poster.view != nullptr) {
                const float h = bannerHeight - 48 * scale, w = h * poster.width / poster.height;
                const ImVec2 at(origin.x + width - 40 * scale - w, origin.y + 24 * scale);
                draw->AddImageRounded(poster.Ref(), at, ImVec2(at.x + w, at.y + h), ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8 * scale);
            }
        } else {
            // The art is cropped to the banner's shape around its middle.
            const float imageAspect = static_cast<float>(banner.width) / static_cast<float>(banner.height), boxAspect = width / bannerHeight;
            ImVec2 uv0(0, 0), uv1(1, 1);
            if (imageAspect < boxAspect) {
                const float visible = imageAspect / boxAspect;
                uv0.y = (1 - visible) / 2;
                uv1.y = uv0.y + visible;
            } else {
                const float visible = boxAspect / imageAspect;
                uv0.x = (1 - visible) / 2;
                uv1.x = uv0.x + visible;
            }
            draw->AddImage(banner.Ref(), origin, ImVec2(origin.x + width, origin.y + bannerHeight), uv0, uv1);
        }
        draw->AddRectFilledMultiColor(ImVec2(origin.x, origin.y + bannerHeight * 0.35f), ImVec2(origin.x + width, origin.y + bannerHeight),
                                      Packed(kBackground, 0), Packed(kBackground, 0), Packed(kBackground), Packed(kBackground));
        ImGui::SetCursorScreenPos(ImVec2(origin.x + 20 * scale, origin.y + 18 * scale));
        if (ImGui::Button(("<  " + SystemName(game->system)).c_str(), ImVec2(0, 34 * scale))) pending = [this] { OpenConsole(); };
        ImGui::SetCursorScreenPos(origin);
        ImGui::Dummy(ImVec2(width, bannerHeight - 40 * scale));

        const float pad = 32 * scale;
        ImGui::SetCursorPosX(pad);
        ImGui::BeginGroup();
        ImGui::PushFont(semibold, 30);
        ImGui::TextUnformatted(Utf8(game->title).c_str());
        ImGui::PopFont();
        std::string meta = SystemName(game->system);
        // Other consoles' keys are a hash of the path; their serial, when the disc had one, says more.
        const std::wstring id = External(*game) || game->system == L"PS3" ? game->serial : game->titleId;
        if (!id.empty()) meta += "   ·   " + Utf8(id);
        if (!game->version.empty()) meta += "   ·   v" + Utf8(game->version);
        ImGui::TextDisabled("%s", meta.c_str());
        ImGui::SameLine();
        std::string tag = profile.bloodborne ? "experimento" : profile.tested ? "testado" : "experimental";
        bool good = profile.tested;
        if (External(*game)) {
            const Engine& engine = EngineFor(game->system);
            const bool missing = engine.ready && !MissingFor(game->system, game->folder).empty();
            good = engine.ready && !missing;
            tag = !engine.ready ? "falta emulador" : missing ? "falta arquivo do console"
                                                            : "via " + (engine.presetName.empty() ? Utf8(FileName(engine.exe)) : Utf8(engine.presetName));
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Color(good ? kGreen : kAmber));
        ImGui::TextUnformatted(tag.c_str());
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 6 * scale));

        const bool busy = runner.Busy();
        const bool thisGame = runner.TitleId() == game->titleId;
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
        ImGui::PushFont(semibold, 18);
        ImGui::BeginDisabled(busy);
        const char* playLabel = !busy ? "Jogar" : runner.Current() == Runner::State::Preparing ? "Preparando..." : "Rodando...";
        // The page opens with the focus on Jogar, so Cross / A (or Enter) plays, as on a console; the d-pad goes down
        // to the options from there.
        const bool focusNow = focusPlay;
        if (focusPlay) {
            ImGui::SetKeyboardFocusHere();
            focusPlay = false;
        }
        if (ImGui::Button(playLabel, ImVec2(170 * scale, 44 * scale))) Play(*game);
        // With a gamepad the focus shows from the start; A with nothing focused (the mouse clicked elsewhere) still plays.
        if (focusNow) ShowRing();
        if (nav.padAccept && !busy && !ImGui::IsAnyItemFocused()) Play(*game);
        ImGui::EndDisabled();
        ImGui::PopFont();
        ImGui::PopStyleColor(4);
        if (thisGame && runner.Current() != Runner::State::Idle) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * scale);
            const auto state = runner.Current();
            ImGui::PushStyleColor(ImGuiCol_Text, Color(state == Runner::State::Failed ? kRed : state == Runner::State::Finished ? kDim : kText));
            ImGui::PushTextWrapPos(width - pad);
            ImGui::TextWrapped("%s", Utf8(runner.Message()).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        } else if (busy) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12 * scale);
            ImGui::TextDisabled("Outro jogo está aberto.");
        }
        ImGui::EndGroup();

        ImGui::Dummy(ImVec2(0, 10 * scale));
        ImGui::SetCursorPosX(pad);
        ImGui::BeginChild("##tabs", ImVec2(width - 2 * pad, 0), ImGuiChildFlags_NavFlattened);
        if (ImGui::BeginTabBar("##tabbar")) {
            if (ImGui::BeginTabItem("Opções")) {
                ImGui::BeginChild("##options", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                OptionsTab(*game, profile, scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (profile.sonsOfSparta && ImGui::BeginTabItem("Cheats")) {
                ImGui::BeginChild("##cheats", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                CheatsTab(scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            // Add-ons are mounted by the PS5 runtime; the Bloodborne port and other emulators have no such mount.
            if (game->system == L"PS5" && ImGui::BeginTabItem("DLC")) {
                ImGui::BeginChild("##dlc", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                DlcTab(*game, scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (Current() != nullptr && ImGui::BeginTabItem("Log")) {
                LogTab(*Current(), scale);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        ImGui::EndChild();
    }

    bool NeedsFrames() const {
        return (gamepads.Connected() && GetForegroundWindow() == window) || std::fabs(homeTarget - homeScroll) > 0.0f;
    }

    // The client gets out of the way while a game runs and comes back when it ends.
    void Watch() {
        const auto state = runner.Current();
        if (state == lastState) return;
        if (state == Runner::State::Running) ShowWindow(window, SW_MINIMIZE);
        if (lastState == Runner::State::Running) {
            ShowWindow(window, SW_RESTORE);
            SetForegroundWindow(window);
        }
        if (lastState != Runner::State::Idle) logDirty = true;
        lastState = state;
    }

    void Frame(float scale) {
        // Library changes asked for while drawing run here, before anything holds on to a game.
        if (pending) {
            auto action = std::move(pending);
            pending = nullptr;
            action();
        }
        for (const auto& titleId : covers.TakeDone()) {
            for (const auto& game : library.Games()) if (game.titleId == titleId) ForgetArt(game);
        }
        // The controllers belong to the game while it runs.
        if (runner.Busy()) gamepads.Stop();
        else gamepads.Start();
        const Nav nav = ReadNav();
        // Arrows move the keyboard focus only where there are fields and tabs; on the console row and the grid they
        // move the selection.
        ImGuiIO& io = ImGui::GetIO();
        // A page left behind takes the gamepad's place on its top row with it.
        static Page lastPage = page;
        static bool lastEngines = enginesPage;
        if (page != lastPage || enginesPage != lastEngines) barFocus = -1;
        if (enginesPage && !lastEngines) focusEngines = true;
        lastPage = page;
        lastEngines = enginesPage;
        // ImGui's own focus also runs an open list, such as Adicionar jogo's.
        const bool fieldNav = enginesPage || page == Page::Game || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
        if (ringFrames > 0 && ringFrames--) ImGui::SetNavCursorVisible(true);
        if (fieldNav) io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        else io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        // There the gamepad moves the same focus as the arrows: the d-pad or left stick goes between fields, A (Cross)
        // presses or opens one, B (Circle) closes an open list. The events reach ImGui on the next frame.
        static const std::pair<Gamepads::Button, ImGuiKey> padKeys[] = {
            {Gamepads::Left, ImGuiKey_GamepadDpadLeft}, {Gamepads::Right, ImGuiKey_GamepadDpadRight}, {Gamepads::Up, ImGuiKey_GamepadDpadUp},
            {Gamepads::Down, ImGuiKey_GamepadDpadDown}, {Gamepads::Accept, ImGuiKey_GamepadFaceDown}, {Gamepads::Back, ImGuiKey_GamepadFaceRight},
        };
        // A button still held from the page before (the A that opened the game) is no press here: it would hit Jogar,
        // which has the focus, and start the game.
        static std::array<bool, Gamepads::Count> padSent{}, padStale{};
        const bool padFront = _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr || GetForegroundWindow() == window;
        for (const auto& [button, key] : padKeys) {
            const bool held = gamepads.Held(button);
            if (!held) padStale[button] = false;
            else if (!fieldNav) padStale[button] = true;
            const bool down = fieldNav && padFront && held && !padStale[button];
            if (down != padSent[button]) io.AddKeyEvent(key, padSent[button] = down);
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (enginesPage) {
            if (nav.back) enginesPage = false;
            EnginesPage(scale);
        } else if (page == Page::Home) {
            Home(scale, nav);
        } else if (page == Page::Console) {
            ConsolePage(scale, nav);
        } else {
            Detail(scale, nav);
        }
        AskSystem(scale);
        NoticeWindow(scale);
        ImGui::End();
    }
};

Client* g_client = nullptr;
bool g_closeConfirmed = false;

LRESULT WINAPI WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, message, wParam, lParam)) return true;
    switch (message) {
        case WM_SIZE:
            if (wParam == SIZE_MINIMIZED) return 0;
            g_resizeWidth = LOWORD(lParam);
            g_resizeHeight = HIWORD(lParam);
            return 0;
        case WM_GETMINMAXINFO: {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize = {760, 520};
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
            break;
        case WM_CLOSE:
            // Closing while a game is being prepared asks first, inside Ranga's window; Sim closes for real.
            if (g_runner != nullptr && g_runner->Current() == Runner::State::Preparing && !g_closeConfirmed && g_client != nullptr) {
                g_client->Confirm(L"O jogo ainda está sendo preparado. Fechar agora interrompe o preparo, que recomeça na próxima vez. Fechar mesmo assim?",
                                  [hwnd] {
                                      g_closeConfirmed = true;
                                      PostMessageW(hwnd, WM_CLOSE, 0, 0);
                                  });
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    // One Ranga at a time; the interface tests (RANGA_CLIENT_TEST_INPUT) run their copy beside the user's.
    HANDLE single = CreateMutexW(nullptr, TRUE, kMutex);
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS && _wgetenv(L"RANGA_CLIENT_TEST_INPUT") == nullptr) {
        if (HWND existing = FindWindowW(kWindowClass, nullptr)) {
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }
    wchar_t modulePath[MAX_PATH * 2] = {};
    GetModuleFileNameW(nullptr, modulePath, MAX_PATH * 2);
    std::wstring root = modulePath;
    root.resize(root.find_last_of(L"\\/"));
    SetCurrentDirectoryW(root.c_str());
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW windowClass{sizeof(windowClass), CS_CLASSDC, WindowProc, 0, 0, instance, nullptr, LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, kWindowClass, nullptr};
    RegisterClassExW(&windowClass);
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    GetMonitorInfoW(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY), &monitor);
    const RECT work = monitor.rcWork;
    const int width = std::min(static_cast<int>(1280 * scale), static_cast<int>(work.right - work.left));
    const int height = std::min(static_cast<int>(760 * scale), static_cast<int>(work.bottom - work.top));
    HWND hwnd = CreateWindowW(kWindowClass, kAppName, WS_OVERLAPPEDWINDOW, work.left + (work.right - work.left - width) / 2, work.top + (work.bottom - work.top - height) / 2,
                              width, height, nullptr, nullptr, instance, nullptr);
    // The dark title bar matches the window (DWMWA_USE_IMMERSIVE_DARK_MODE, Windows 10 20H1 and later).
    if (HMODULE dwm = LoadLibraryW(L"dwmapi.dll")) {
        using SetAttribute = HRESULT(WINAPI*)(HWND, DWORD, LPCVOID, DWORD);
        if (auto set = reinterpret_cast<SetAttribute>(reinterpret_cast<void*>(GetProcAddress(dwm, "DwmSetWindowAttribute")))) {
            const BOOL dark = TRUE;
            set(hwnd, 20, &dark, sizeof(dark));
        }
    }
    if (!CreateDevice(hwnd)) {
        ReleaseDevice();
        MessageBoxW(nullptr, L"Não deu para iniciar o Direct3D 11 nesta placa de vídeo.", kAppName, MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    wchar_t windows[MAX_PATH] = {};
    GetWindowsDirectoryW(windows, MAX_PATH);
    const std::string fonts = Utf8(std::wstring(windows) + L"\\Fonts\\");
    ImGui::GetStyle().FontSizeBase = 16.0f;
    ImFont* regular = Exists(Wide(fonts + "segoeui.ttf")) ? io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str()) : nullptr;
    if (regular == nullptr) regular = io.Fonts->AddFontDefault();
    ImFont* semibold = Exists(Wide(fonts + "seguisb.ttf")) ? io.Fonts->AddFontFromFileTTF((fonts + "seguisb.ttf").c_str()) : nullptr;
    ApplyTheme(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    Client client(root);
    client.window = hwnd;
    client.semibold = semibold != nullptr ? semibold : regular;
    g_runner = &client.runner;
    g_client = &client;
    client.Reload(client.library.Selected());
    if (const Game* game = client.Current()) client.console = game->system;

    bool done = false;
    while (!done) {
        MSG message;
        // Idle frames wait for input, so the client costs nothing while a game runs; a busy runner keeps it ticking for status.
        // A controller is read, not told by a message, and the console row slides: those need frames meanwhile.
        if (!client.runner.Busy()) MsgWaitForMultipleObjects(0, nullptr, FALSE, client.NeedsFrames() ? 16 : 250, QS_ALLINPUT);
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;
        if (IsIconic(hwnd) || (g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            Sleep(50);
            if (!IsIconic(hwnd)) g_occluded = false;
            client.Watch();
            continue;
        }
        g_occluded = false;
        client.Watch();
        if (g_resizeWidth != 0 && g_resizeHeight != 0) {
            ReleaseTarget();
            g_swapChain->ResizeBuffers(0, g_resizeWidth, g_resizeHeight, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeWidth = g_resizeHeight = 0;
            CreateTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        // The Win32 backend sees only XInput pads and takes this flag away without one; Ranga feeds every SDL gamepad
        // to ImGui itself (Frame), so a DualSense moves the focus too.
        ImGui::GetIO().BackendFlags |= ImGuiBackendFlags_HasGamepad;
        ImGui::NewFrame();
        client.Frame(scale);
        ImGui::Render();
        const ImVec4 clear = Color(kBackground);
        const float clearColor[4] = {clear.x, clear.y, clear.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_occluded = g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
    }
    g_runner = nullptr;
    g_client = nullptr;
    for (auto* cache : {&client.icons, &client.banners}) {
        for (auto& entry : *cache) if (entry.second.view != nullptr) entry.second.view->Release();
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    ReleaseDevice();
    DestroyWindow(hwnd);
    UnregisterClassW(kWindowClass, instance);
    return 0;
}
