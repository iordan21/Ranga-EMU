// Ranga Emu client: a library of games of every console on the left, the chosen game on the right, with Play and its
// options, cheats and logs, and an Emulators page where each console other than PS5 gets the emulator that runs it.
#include "Common.hpp"
#include "Enderecos.hpp"
#include "Covers.hpp"
#include "Dlss5.hpp"
#include "Library.hpp"
#include "Runner.hpp"
#include "Systems.hpp"
#include "Rooms.hpp"
#include "ConsoleGraphics.hpp"
#include "Update.hpp"
#include "Store.hpp"
#include "Http.hpp"
#include "PlayerProfile.hpp"
#include "Lobby.hpp"
#include "Media.hpp"
#include "Backup.hpp"
#include "Cloud.hpp"

#include <d3d11.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <optional>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <cmath>
#include <functional>
#include <atomic>
#include <mutex>
#include <thread>
#include <array>
#include <map>
#include <set>
#include <ctime>
#include <filesystem>
#include <cfloat>

#include "imgui.h"
#include "misc/freetype/imgui_freetype.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"

#define SDL_MAIN_HANDLED
#include <SDL.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG // the Switch games' own icons
#define STBI_WINDOWS_UTF8
#include "stb_image.h"

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam);

namespace {

using namespace Ranga;

constexpr const wchar_t* kWindowClass = L"RangaEmuClient";
constexpr const wchar_t* kMutex = L"Local\\RangaEmuLauncher";
// The oldest Ranga that has everything of the chat this one has (groups, invites, pictures): friends on an older one
// get a warning in the chat. Also this Ranga's own number when engines.ini has none yet (a copy from the zip).
constexpr const char* kSocialVersion = "1.4.0";

// "1.10.2" after "1.9.0": numbers part by part.
bool OlderVersion(const std::string& a, const std::string& b) {
    const auto parts = [](const std::string& text) {
        std::vector<int> out;
        for (size_t at = 0; at <= text.size();) {
            const size_t dot = text.find('.', at);
            out.push_back(std::atoi(text.substr(at, dot == std::string::npos ? std::string::npos : dot - at).c_str()));
            if (dot == std::string::npos) break;
            at = dot + 1;
        }
        return out;
    };
    return parts(a) < parts(b);
}

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

// RANGA_CLIENT_SHOT=<file.bmp>: the window's own picture written there once a second, straight from what it draws, for
// the interface tests; with it the window draws even when Windows says no one sees it (a screen asleep).
void WriteShot(const wchar_t* path) {
    ID3D11Texture2D* back = nullptr;
    if (FAILED(g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back)))) return;
    D3D11_TEXTURE2D_DESC description;
    back->GetDesc(&description);
    description.Usage = D3D11_USAGE_STAGING;
    description.BindFlags = 0;
    description.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    description.MiscFlags = 0;
    ID3D11Texture2D* copy = nullptr;
    if (SUCCEEDED(g_device->CreateTexture2D(&description, nullptr, &copy))) {
        g_context->CopyResource(copy, back);
        D3D11_MAPPED_SUBRESOURCE mapped;
        if (SUCCEEDED(g_context->Map(copy, 0, D3D11_MAP_READ, 0, &mapped))) {
            const int width = static_cast<int>(description.Width), height = static_cast<int>(description.Height);
            std::vector<unsigned char> pixels(static_cast<size_t>(width) * height * 4);
            for (int y = 0; y < height; ++y) {
                const auto* row = static_cast<const unsigned char*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
                unsigned char* out = pixels.data() + static_cast<size_t>(height - 1 - y) * width * 4;
                for (int x = 0; x < width; ++x) {
                    out[x * 4 + 0] = row[x * 4 + 2];
                    out[x * 4 + 1] = row[x * 4 + 1];
                    out[x * 4 + 2] = row[x * 4 + 0];
                    out[x * 4 + 3] = 255;
                }
            }
            g_context->Unmap(copy, 0);
            BITMAPFILEHEADER file{};
            BITMAPINFOHEADER info{};
            info.biSize = sizeof(info);
            info.biWidth = width;
            info.biHeight = height;
            info.biPlanes = 1;
            info.biBitCount = 32;
            file.bfType = 0x4D42;
            file.bfOffBits = sizeof(file) + sizeof(info);
            file.bfSize = static_cast<DWORD>(file.bfOffBits + pixels.size());
            if (FILE* out = _wfopen(path, L"wb")) {
                std::fwrite(&file, sizeof(file), 1, out);
                std::fwrite(&info, sizeof(info), 1, out);
                std::fwrite(pixels.data(), 1, pixels.size(), out);
                std::fclose(out);
            }
        }
        copy->Release();
    }
    back->Release();
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

// Flat and nearly black; the accent is kept for what is chosen and for the one button to press.
constexpr unsigned kBackground = 0x0b0c0e, kSidebar = 0x101114, kSurface = 0x17181c, kHover = 0x202126, kSelected = 0x2a2b31;
constexpr unsigned kText = 0xe8e8ea, kDim = 0x8b8d93, kAccent = 0xe5484d, kAccentHover = 0xf0595e, kGreen = 0x46a758, kAmber = 0xf5a524, kRed = 0xff6369;

// The whole look, at a size (the screen's, or bigger in the TV mode): from ImGui's own start each time, so it can be
// applied again when the size changes.
void ApplyTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    style.FontSizeBase = 16.0f;
    ImGui::StyleColorsDark(&style);
    style.WindowPadding = ImVec2(0, 0);
    style.WindowBorderSize = 0;
    style.ChildBorderSize = 0;
    style.FramePadding = ImVec2(10, 7);
    style.ItemSpacing = ImVec2(10, 10);
    style.FrameRounding = 8;
    style.ChildRounding = 0;
    style.PopupRounding = 10;
    style.GrabRounding = 6;
    style.TabRounding = 8;
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
// Pad, Handheld, Cartridge, Disc and Arcade are kinds; the rest are one console's own shape (still no logos): Slab a
// flat ribbed box (PS2), Arch a box with a round top (PS3), Tablet a pad with a screen (Wii U), Remote a slim upright
// remote (Wii), Cube a cube with a handle (GameCube), Chest a big box with a round gem on top (Xbox).
enum class Glyph { Pad, Handheld, Cartridge, Disc, Arcade, Slab, Arch, Tablet, Remote, Cube, Chest, Hybrid, Wide, Clam };

struct Look {
    const wchar_t* id;
    const char* tag;
    unsigned top, bottom;
    Glyph glyph;
    const char* maker; // who made it ("" for arcade)
    int year;          // first released, anywhere (0: none)
    unsigned name;     // the full name's color on the home screen, light enough to read on the dark background
};

constexpr Look kLooks[] = {
    {L"PS5", "PS5", 0x3a7bf0, 0x0b1d4a, Glyph::Pad, "Sony", 2020, 0x5b93ff}, {L"PS4", "PS4", 0x2459b8, 0x081633, Glyph::Pad, "Sony", 2013, 0x4f86e8},
    {L"PS3", "PS3", 0x4a4e57, 0x0c0d10, Glyph::Arch, "Sony", 2006, 0xc9cdd6}, {L"PS2", "PS2", 0x2b3f8f, 0x070b1f, Glyph::Slab, "Sony", 2000, 0x6e8cff},
    {L"PS1", "PS1", 0x9097a3, 0x2a2c33, Glyph::Disc, "Sony", 1994, 0xc2c7d0}, {L"PSP", "PSP", 0x585c66, 0x111216, Glyph::Wide, "Sony", 2004, 0xb0b5bf},
    {L"SWITCH", "Switch", 0xe4002b, 0x4a0010, Glyph::Hybrid, "Nintendo", 2017, 0xff4560}, {L"WIIU", "Wii U", 0x0aa6d6, 0x00303f, Glyph::Tablet, "Nintendo", 2012, 0x3cc6f2},
    {L"WII", "Wii", 0xa9bfcf, 0x2e3a44, Glyph::Remote, "Nintendo", 2006, 0xdce8f2}, {L"GC", "GC", 0x7357c0, 0x1d1238, Glyph::Cube, "Nintendo", 2001, 0xa48cf0},
    {L"3DS", "3DS", 0xd8303b, 0x3d070b, Glyph::Clam, "Nintendo", 2011, 0xff5a64}, {L"NDS", "DS", 0x878a93, 0x24262b, Glyph::Clam, "Nintendo", 2004, 0xc8ccd4},
    {L"GBA", "GBA", 0x6550c8, 0x170f3d, Glyph::Wide, "Nintendo", 2001, 0x9d88f0}, {L"GB", "GB", 0x93b31c, 0x1f3a0b, Glyph::Handheld, "Nintendo", 1989, 0xb2d43a},
    {L"N64", "N64", 0x24a05a, 0x06281a, Glyph::Cartridge, "Nintendo", 1996, 0x45cc80}, {L"SNES", "SNES", 0x7a6abd, 0x1e1838, Glyph::Cartridge, "Nintendo", 1990, 0xa99bea},
    {L"NES", "NES", 0xd21a38, 0x3b0610, Glyph::Cartridge, "Nintendo", 1983, 0xff5a6a}, {L"MD", "MD", 0x45474d, 0x0a0a0a, Glyph::Cartridge, "Sega", 1988, 0xd6d8de},
    {L"SMS", "SMS", 0xd03434, 0x2d0808, Glyph::Cartridge, "Sega", 1985, 0xff6464}, {L"SEGACD", "Sega CD", 0x3a4aa0, 0x0a0f2b, Glyph::Disc, "Sega", 1991, 0x7d8ef0},
    {L"SATURN", "Saturn", 0x4a4e94, 0x101129, Glyph::Disc, "Sega", 1994, 0x8a8ee6}, {L"DC", "DC", 0xf47a30, 0x4a1a03, Glyph::Disc, "Sega", 1998, 0xffa25c},
    {L"XBOX", "Xbox", 0x36a336, 0x0a2a0a, Glyph::Chest, "Microsoft", 2001, 0x62d262}, {L"X360", "360", 0x78cc56, 0x1b3a10, Glyph::Pad, "Microsoft", 2005, 0x96e070},
    {L"PCE", "PCE", 0xf2962a, 0x46230a, Glyph::Cartridge, "NEC", 1987, 0xffad4d}, {L"A2600", "2600", 0xa8784a, 0x2b1a0c, Glyph::Cartridge, "Atari", 1977, 0xd8a472},
    {L"ARCADE", "Arcade", 0xe0306e, 0x3a0620, Glyph::Arcade, "", 0, 0xff5a96},
};

const Look& LookFor(const std::wstring& id) {
    for (const Look& look : kLooks) if (id == look.id) return look;
    static const Look other{L"", "?", 0x55575e, 0x16171a, Glyph::Pad, "", 0, 0xe6e7ea};
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
        case Glyph::Slab: {
            // Lying flat: ribs on the left half, the tray's line and two small buttons in front.
            const ImVec2 min(c.x - s * 0.46f, c.y - s * 0.17f), max(c.x + s * 0.46f, c.y + s * 0.17f);
            draw->AddRectFilled(min, max, color, s * 0.03f);
            for (int i = 0; i < 5; ++i) {
                const float x = min.x + s * (0.06f + i * 0.075f);
                draw->AddRectFilled(ImVec2(x, min.y + s * 0.05f), ImVec2(x + s * 0.035f, max.y - s * 0.05f), shade, s * 0.01f);
            }
            draw->AddRectFilled(ImVec2(c.x, c.y - s * 0.05f), ImVec2(max.x - s * 0.06f, c.y - s * 0.02f), shade, s * 0.01f);
            draw->AddRectFilled(ImVec2(max.x - s * 0.17f, c.y + s * 0.05f), ImVec2(max.x - s * 0.12f, c.y + s * 0.1f), shade, s * 0.01f);
            draw->AddRectFilled(ImVec2(max.x - s * 0.1f, c.y + s * 0.05f), ImVec2(max.x - s * 0.05f, c.y + s * 0.1f), shade, s * 0.01f);
            break;
        }
        case Glyph::Arch: {
            // The round-topped box: the disc slot along the front and a light at the side.
            const ImVec2 min(c.x - s * 0.46f, c.y - s * 0.24f), max(c.x + s * 0.46f, c.y + s * 0.2f);
            draw->AddRectFilled(min, max, color, s * 0.2f, ImDrawFlags_RoundCornersTop);
            draw->AddRectFilled(ImVec2(c.x - s * 0.24f, c.y + s * 0.02f), ImVec2(c.x + s * 0.24f, c.y + s * 0.05f), shade, s * 0.015f);
            draw->AddCircleFilled(ImVec2(max.x - s * 0.09f, c.y + s * 0.035f), s * 0.025f, shade, 12);
            draw->AddLine(ImVec2(min.x + s * 0.06f, max.y - s * 0.07f), ImVec2(max.x - s * 0.06f, max.y - s * 0.07f), shade, s * 0.012f);
            break;
        }
        case Glyph::Tablet: {
            // A wide pad with a screen in the middle, sticks and buttons at the sides.
            const ImVec2 min(c.x - s * 0.47f, c.y - s * 0.26f), max(c.x + s * 0.47f, c.y + s * 0.26f);
            draw->AddRectFilled(min, max, color, s * 0.12f);
            draw->AddRectFilled(ImVec2(c.x - s * 0.24f, c.y - s * 0.18f), ImVec2(c.x + s * 0.24f, c.y + s * 0.16f), shade, s * 0.02f);
            draw->AddCircleFilled(ImVec2(min.x + s * 0.12f, c.y - s * 0.1f), s * 0.05f, shade, 16);
            const float arm = s * 0.035f, length = s * 0.11f;
            const ImVec2 dpad(min.x + s * 0.12f, c.y + s * 0.09f);
            draw->AddRectFilled(ImVec2(dpad.x - length / 2, dpad.y - arm / 2), ImVec2(dpad.x + length / 2, dpad.y + arm / 2), shade, arm * 0.3f);
            draw->AddRectFilled(ImVec2(dpad.x - arm / 2, dpad.y - length / 2), ImVec2(dpad.x + arm / 2, dpad.y + length / 2), shade, arm * 0.3f);
            draw->AddCircleFilled(ImVec2(max.x - s * 0.12f, c.y - s * 0.1f), s * 0.05f, shade, 16);
            const ImVec2 face(max.x - s * 0.12f, c.y + s * 0.09f);
            const float r = s * 0.022f, d = s * 0.045f;
            for (const ImVec2 offset : {ImVec2(0, -d), ImVec2(0, d), ImVec2(-d, 0), ImVec2(d, 0)}) draw->AddCircleFilled(ImVec2(face.x + offset.x, face.y + offset.y), r, shade, 12);
            break;
        }
        case Glyph::Remote: {
            // Upright and slim: the cross near the top, the big button under it, two small ones low and the lights.
            const ImVec2 min(c.x - s * 0.11f, c.y - s * 0.46f), max(c.x + s * 0.11f, c.y + s * 0.46f);
            draw->AddRectFilled(min, max, color, s * 0.09f);
            const float arm = s * 0.04f, length = s * 0.13f;
            const ImVec2 dpad(c.x, min.y + s * 0.15f);
            draw->AddRectFilled(ImVec2(dpad.x - length / 2, dpad.y - arm / 2), ImVec2(dpad.x + length / 2, dpad.y + arm / 2), shade, arm * 0.3f);
            draw->AddRectFilled(ImVec2(dpad.x - arm / 2, dpad.y - length / 2), ImVec2(dpad.x + arm / 2, dpad.y + length / 2), shade, arm * 0.3f);
            draw->AddCircleFilled(ImVec2(c.x, min.y + s * 0.34f), s * 0.055f, shade, 20);
            draw->AddCircleFilled(ImVec2(c.x, c.y + s * 0.18f), s * 0.03f, shade, 12);
            draw->AddCircleFilled(ImVec2(c.x, c.y + s * 0.27f), s * 0.03f, shade, 12);
            for (int i = 0; i < 4; ++i) draw->AddCircleFilled(ImVec2(c.x - s * 0.06f + i * s * 0.04f, max.y - s * 0.06f), s * 0.012f, shade, 8);
            break;
        }
        case Glyph::Cube: {
            // A cube seen from a corner, with the handle at the back and the lid's round mark on top.
            const float h = s * 0.4f, w = h * 0.866f;
            const ImVec2 top(c.x, c.y - h * 0.9f), left(c.x - w, c.y - h * 0.4f), right(c.x + w, c.y - h * 0.4f), middle(c.x, c.y + h * 0.1f);
            const ImVec2 bottomLeft(c.x - w, c.y + h * 0.55f), bottomRight(c.x + w, c.y + h * 0.55f), bottom(c.x, c.y + h * 1.05f);
            draw->AddRect(ImVec2(c.x - w * 0.55f, top.y - h * 0.32f), ImVec2(c.x + w * 0.1f, top.y + h * 0.1f), color, s * 0.04f, 0, s * 0.05f);
            const ImVec2 hull[] = {top, right, bottomRight, bottom, bottomLeft, left};
            draw->AddConvexPolyFilled(hull, 6, color);
            draw->AddLine(left, middle, shade, s * 0.025f);
            draw->AddLine(right, middle, shade, s * 0.025f);
            draw->AddLine(middle, bottom, shade, s * 0.025f);
            draw->AddCircleFilled(ImVec2(c.x, c.y - h * 0.4f), s * 0.07f, shade, 24);
            draw->AddCircleFilled(ImVec2(c.x, c.y - h * 0.4f), s * 0.035f, color, 16);
            break;
        }
        case Glyph::Chest: {
            // A big box with a round gem in the middle of its top and the tray's line below.
            const ImVec2 min(c.x - s * 0.46f, c.y - s * 0.16f), max(c.x + s * 0.46f, c.y + s * 0.24f);
            draw->AddRectFilled(min, max, color, s * 0.05f);
            draw->AddRectFilled(ImVec2(c.x - s * 0.3f, c.y + s * 0.1f), ImVec2(c.x + s * 0.3f, c.y + s * 0.13f), shade, s * 0.01f);
            draw->AddCircleFilled(ImVec2(c.x, min.y), s * 0.16f, color, 40);
            draw->AddCircleFilled(ImVec2(c.x, min.y), s * 0.11f, shade, 40);
            draw->AddCircleFilled(ImVec2(c.x, min.y), s * 0.06f, color, 24);
            break;
        }
        case Glyph::Hybrid: {
            // The tablet with a Joy-Con on each side, a gap between them and the screen: stick and buttons on the
            // left one, buttons and stick on the right.
            const float half = s * 0.48f, tall = s * 0.21f, rail = s * 0.13f, gap = s * 0.025f;
            draw->AddRectFilled(ImVec2(c.x - half, c.y - tall), ImVec2(c.x - half + rail, c.y + tall), color, s * 0.09f, ImDrawFlags_RoundCornersLeft);
            draw->AddRectFilled(ImVec2(c.x + half - rail, c.y - tall), ImVec2(c.x + half, c.y + tall), color, s * 0.09f, ImDrawFlags_RoundCornersRight);
            const ImVec2 body(c.x - half + rail + gap, c.y - tall), bodyEnd(c.x + half - rail - gap, c.y + tall);
            draw->AddRectFilled(body, bodyEnd, color, s * 0.01f);
            draw->AddRectFilled(ImVec2(body.x + s * 0.035f, body.y + s * 0.04f), ImVec2(bodyEnd.x - s * 0.035f, bodyEnd.y - s * 0.04f), shade, s * 0.01f);
            const float lx = c.x - half + rail * 0.5f, rx = c.x + half - rail * 0.5f;
            draw->AddCircleFilled(ImVec2(lx, c.y - tall * 0.45f), s * 0.035f, shade, 16);
            draw->AddCircleFilled(ImVec2(rx, c.y + tall * 0.35f), s * 0.035f, shade, 16);
            const float r = s * 0.016f, d = s * 0.032f;
            for (const ImVec2 offset : {ImVec2(0, -d), ImVec2(0, d), ImVec2(-d, 0), ImVec2(d, 0)}) {
                draw->AddCircleFilled(ImVec2(lx + offset.x, c.y + tall * 0.35f + offset.y), r, shade, 10);
                draw->AddCircleFilled(ImVec2(rx + offset.x, c.y - tall * 0.45f + offset.y), r, shade, 10);
            }
            break;
        }
        case Glyph::Wide: {
            // A handheld held sideways: the screen in the middle, the cross on the left, the buttons on the right.
            const ImVec2 min(c.x - s * 0.47f, c.y - s * 0.2f), max(c.x + s * 0.47f, c.y + s * 0.2f);
            draw->AddRectFilled(min, max, color, s * 0.18f);
            draw->AddRectFilled(ImVec2(c.x - s * 0.22f, c.y - s * 0.14f), ImVec2(c.x + s * 0.22f, c.y + s * 0.12f), shade, s * 0.015f);
            const float arm = s * 0.04f, length = s * 0.12f;
            const ImVec2 dpad(min.x + s * 0.12f, c.y - s * 0.01f);
            draw->AddRectFilled(ImVec2(dpad.x - length / 2, dpad.y - arm / 2), ImVec2(dpad.x + length / 2, dpad.y + arm / 2), shade, arm * 0.3f);
            draw->AddRectFilled(ImVec2(dpad.x - arm / 2, dpad.y - length / 2), ImVec2(dpad.x + arm / 2, dpad.y + length / 2), shade, arm * 0.3f);
            const ImVec2 face(max.x - s * 0.12f, c.y - s * 0.01f);
            const float r = s * 0.022f, d = s * 0.042f;
            for (const ImVec2 offset : {ImVec2(0, -d), ImVec2(0, d), ImVec2(-d, 0), ImVec2(d, 0)}) draw->AddCircleFilled(ImVec2(face.x + offset.x, face.y + offset.y), r, shade, 12);
            break;
        }
        case Glyph::Clam: {
            // Open, seen from the front: the top half with its screen, the hinge, the bottom half with the touch
            // screen between the cross and the buttons.
            const float w = s * 0.34f;
            draw->AddRectFilled(ImVec2(c.x - w, c.y - s * 0.44f), ImVec2(c.x + w, c.y - s * 0.03f), color, s * 0.05f);
            draw->AddRectFilled(ImVec2(c.x - w * 0.72f, c.y - s * 0.38f), ImVec2(c.x + w * 0.72f, c.y - s * 0.09f), shade, s * 0.01f);
            draw->AddRectFilled(ImVec2(c.x - w, c.y + s * 0.01f), ImVec2(c.x + w, c.y + s * 0.44f), color, s * 0.05f);
            draw->AddRectFilled(ImVec2(c.x - w * 0.45f, c.y + s * 0.07f), ImVec2(c.x + w * 0.45f, c.y + s * 0.34f), shade, s * 0.01f);
            const float arm = s * 0.03f, length = s * 0.09f;
            const ImVec2 dpad(c.x - w * 0.75f, c.y + s * 0.2f);
            draw->AddRectFilled(ImVec2(dpad.x - length / 2, dpad.y - arm / 2), ImVec2(dpad.x + length / 2, dpad.y + arm / 2), shade, arm * 0.3f);
            draw->AddRectFilled(ImVec2(dpad.x - arm / 2, dpad.y - length / 2), ImVec2(dpad.x + arm / 2, dpad.y + length / 2), shade, arm * 0.3f);
            const ImVec2 face(c.x + w * 0.75f, c.y + s * 0.2f);
            const float r = s * 0.017f, d = s * 0.032f;
            for (const ImVec2 offset : {ImVec2(0, -d), ImVec2(0, d), ImVec2(-d, 0), ImVec2(d, 0)}) draw->AddCircleFilled(ImVec2(face.x + offset.x, face.y + offset.y), r, shade, 10);
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
void ConsoleCard(ImDrawList* draw, ImFont* font, const Look& look, ImVec2 min, ImVec2 max, float scale, float alpha, bool chosen) {
    const float size = max.x - min.x, rounding = 14 * scale;
    const unsigned tone = chosen ? kSelected : kSurface;
    draw->AddRectFilled(min, max, Packed(tone), rounding);
    DrawGlyph(draw, look.glyph, ImVec2((min.x + max.x) / 2, min.y + size * 0.40f), size * 0.46f, Packed(0xffffff, (chosen ? 0.95f : 0.5f) * alpha), Packed(tone));
    const float text = std::max(13.0f, size * 0.12f);
    const ImVec2 extent = font->CalcTextSizeA(text, FLT_MAX, 0, look.tag);
    const ImVec2 at(min.x + (size - extent.x) / 2, max.y - size * 0.13f - text);
    draw->AddText(font, text, at, Packed(chosen ? 0xffffff : kDim, alpha), look.tag);
    if (chosen) draw->AddRectFilled(ImVec2(at.x, at.y + text + 5 * scale), ImVec2(at.x + extent.x, at.y + text + 8 * scale), Packed(kAccent), 2 * scale);
}

// DualSense, Xbox and other controllers for moving around the client, through SDL like Ranga Retro. They are let go
// while a game runs, so the game's emulator has them to itself.
class Gamepads {
public:
    // Menu is Start / Options (the TV mode), View is Select / Back / Create (the friends' panel).
    enum Button { Left, Right, Up, Down, Accept, Back, Search, Menu, View, Count };

    ~Gamepads() { Stop(); }

    void Start() {
        if (started) return;
        SDL_SetMainReady();
        // As in Ranga Retro: DirectInput's look for pads costs about 300 ms (here, every time a game closes) and only
        // old generic pads need it, so it comes in only when the first look finds no pad.
        const bool directInputChosen = std::getenv("SDL_DIRECTINPUT_ENABLED") != nullptr;
        if (!directInputChosen) SDL_SetHint("SDL_DIRECTINPUT_ENABLED", "0");
        if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return;
        if (!directInputChosen && SDL_NumJoysticks() == 0) {
            SDL_QuitSubSystem(SDL_INIT_GAMECONTROLLER);
            SDL_SetHint("SDL_DIRECTINPUT_ENABLED", "1");
            if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) return;
        }
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
            // Triangle / Y: the search, with the keyboard on screen (a console's page).
            now[Search] = now[Search] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_Y);
            now[Menu] = now[Menu] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_START);
            now[View] = now[View] || SDL_GameControllerGetButton(pad, SDL_CONTROLLER_BUTTON_BACK);
        }
        // RANGA_CLIENT_TEST_INPUT=1: the number pad stands in for a gamepad (8 4 6 2 the d-pad, 5 A, 0 B, 7 Y), so the
        // interface tests go the gamepad's way; 9 is Start and 3 Select.
        static const bool testPad = _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr;
        if (testPad) {
            const ImGuiKey keys[Count] = {ImGuiKey_Keypad4, ImGuiKey_Keypad6, ImGuiKey_Keypad8, ImGuiKey_Keypad2, ImGuiKey_Keypad5,
                                          ImGuiKey_Keypad0, ImGuiKey_Keypad7, ImGuiKey_Keypad9, ImGuiKey_Keypad3};
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

    // Any button or stick of any controller down now, read straight from SDL without a frame (the main loop asks it
    // 60 times a second while a controller is there, and draws a frame only when this says yes).
    bool AnyDown() const {
        if (!started || pads.empty()) return false;
        SDL_GameControllerUpdate();
        for (auto* pad : pads) {
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
                if (b != SDL_CONTROLLER_BUTTON_GUIDE && SDL_GameControllerGetButton(pad, static_cast<SDL_GameControllerButton>(b))) return true;
            const int x = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTX), y = SDL_GameControllerGetAxis(pad, SDL_CONTROLLER_AXIS_LEFTY);
            if (x < -20000 || x > 20000 || y < -20000 || y > 20000) return true;
        }
        return false;
    }

    bool Pressed(Button button) const { return pressed[button]; }
    bool Held(Button button) const { return held[button] != 0; }

    // Every button and trigger down now on any controller, by SDL's names ("a", "lefttrigger"...). The Guide button is
    // left out: it opens Ranga's menu in the game.
    std::vector<std::string> ButtonsDown() const {
        std::vector<std::string> down;
        for (auto* pad : pads) {
            for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b) {
                const auto button = static_cast<SDL_GameControllerButton>(b);
                if (button == SDL_CONTROLLER_BUTTON_GUIDE || !SDL_GameControllerGetButton(pad, button)) continue;
                if (const char* name = SDL_GameControllerGetStringForButton(button)) down.push_back(name);
            }
            for (const auto axis : {SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT}) {
                if (SDL_GameControllerGetAxis(pad, axis) > 16000) down.push_back(SDL_GameControllerGetStringForAxis(axis));
            }
        }
        return down;
    }

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

// A texture made from RGBA pixels in memory (the profile photos).
Texture UploadTexture(const unsigned char* pixels, int width, int height) {
    Texture texture;
    texture.width = width;
    texture.height = height;
    D3D11_TEXTURE2D_DESC description{};
    description.Width = static_cast<UINT>(width);
    description.Height = static_cast<UINT>(height);
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
    return texture;
}

// One of Ranga's own achievements: a rounded square in a color of its own with a star; grey while locked.
void DrawMedal(ImDrawList* draw, ImVec2 min, float size, const std::string& id, bool lit) {
    static const unsigned colors[] = {0xe5484d, 0xf5a524, 0x46a758, 0x3e8ed0, 0x8e4ec6, 0xd6409f};
    unsigned hash = 2166136261u;
    for (const char c : id) hash = (hash ^ static_cast<unsigned char>(c)) * 16777619u;
    draw->AddRectFilled(min, ImVec2(min.x + size, min.y + size), Packed(lit ? colors[hash % 6] : kHover), size * 0.22f);
    const ImVec2 center(min.x + size / 2, min.y + size * 0.53f);
    ImVec2 points[10];
    for (int i = 0; i < 10; ++i) {
        const float angle = -1.5707963f + i * 0.6283185f, radius = size * (i % 2 == 0 ? 0.33f : 0.14f);
        points[i] = ImVec2(center.x + std::cos(angle) * radius, center.y + std::sin(angle) * radius);
    }
    draw->AddConcavePolyFilled(points, 10, Packed(0xffffff, lit ? 0.95f : 0.16f));
}

// The color of the ring around a level, as on Steam: it changes every ten levels.
unsigned LevelColor(int level) {
    static const unsigned colors[] = {0x9a9ca3, 0xe5484d, 0xf08c2e, 0xf5c524, 0x46a758, 0x3e8ed0, 0x8e4ec6, 0xd6409f};
    return colors[std::min(level / 10, 7)];
}

std::string FormatDate(long long when) {
    if (when <= 0) return "-";
    const time_t at = static_cast<time_t>(when);
    tm local{};
    localtime_s(&local, &at);
    char text[32];
    std::snprintf(text, sizeof(text), "%02d/%02d/%04d", local.tm_mday, local.tm_mon + 1, local.tm_year + 1900);
    return text;
}

std::string FormatPlayed(long long seconds) {
    char text[32];
    if (seconds < 3600) std::snprintf(text, sizeof(text), "%lld min", seconds / 60);
    else if (seconds < 36000) std::snprintf(text, sizeof(text), "%.1f h", static_cast<double>(seconds) / 3600.0);
    else std::snprintf(text, sizeof(text), "%lld h", seconds / 3600);
    return text;
}

std::string FormatAgo(long long when) {
    const long long ago = static_cast<long long>(std::time(nullptr)) - when;
    if (when <= 0) return "nunca";
    if (ago < 120) return "há pouco";
    if (ago < 3600) return "há " + std::to_string(ago / 60) + " min";
    if (ago < 86400) return "há " + std::to_string(ago / 3600) + " h";
    return "há " + std::to_string(ago / 86400) + (ago / 86400 == 1 ? " dia" : " dias");
}

const char* kLogFiles[] = {"game.err", "game.log", "prepare.log", "launcher.log", "dlss5.log"};

struct Client {
    HWND window = nullptr;
    Library library;
    Runner runner;
    int selected = -1;
    Options options;
    std::map<std::wstring, Texture> icons, banners;
    std::map<std::wstring, ULONGLONG> switchArtChecked;
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
    // The Switch game's online room (Eden's multiplayer rooms), as the game's core options keep it: 0 off, 1 join,
    // 2 host. The core reads them when the game starts.
    int roomMode = 0;
    // Joining a Switch room: the hosting PC's own (private, through playit) or one of Eden's public rooms, picked from
    // the list (Lobby.hpp), which is fetched again every 15 s while it shows.
    bool roomPublic = false;
    std::mutex lobbyLock;
    std::vector<LobbyRoom> lobby;
    std::string lobbyProblem;
    long long lobbyAt = 0;
    std::thread lobbyThread;
    std::atomic<bool> lobbyBusy{false};
    ULONGLONG lobbyAskedAt = 0;
    bool lobbyAll = false;
    std::array<char, 128> roomAddress{}, roomPort{}, roomNickname{}, roomPassword{};
    // Online play of the classics (Ranga Retro's own, Netplay.hpp): this game's choice and the table server's state.
    std::array<char, 128> onlineAddress{}, onlineName{};
    bool onlineOn = false;
    int onlineDelay = 4;
    bool onlineDelayAuto = true;
    // The servers beside the Switch's rooms (Rooms.hpp Extra), by kind: the port typed, whether it runs, why not.
    std::array<char, 8> serverPort[3]{};
    bool serverRunning[3] = {};
    ULONGLONG serverCheckedAt[3] = {};
    std::vector<std::wstring> lanAddresses; // this PC on the home network, looked up every 10 s while shown
    ULONGLONG lanCheckedAt = 0;
    std::array<char, 64> pspLanAddress{}; // the PSP host's address typed on a PSP game's page
    std::array<char, 128> xboxHubAddress{}; // the System Link hub typed on an Xbox game's page
    std::array<char, 128> ps3ServerAddress{}; // Ranga's RPCN server typed on a PS3 game's page
    std::wstring serverProblem[3];
    // The 3DS's room: this game's choice.
    bool room3dsOn = false;
    std::array<char, 128> room3dsAddress{}, room3dsName{};
    // The Controle tab: the console's button map as read from its file, and the RetroPad button waiting for a press
    // (-1 when none), with the buttons already down when the wait began and when it gives up.
    std::wstring controlsSystem;
    std::array<std::string, 16> controls;
    int captureFor = -1;
    // The wait takes a press only once every button was let go (captureArmed); after it, the page waits for that
    // button to be let go too (captureCooling) before reacting to the gamepad again.
    bool captureArmed = false, captureCooling = false;
    double captureUntil = 0;
    // The Switch game's update and add-on files (the eden_conteudo core option), and what each one is.
    std::vector<std::wstring> contentFiles;
    std::vector<SwitchContent> contentKinds;
    // The Switch game's graphics options (eden_* core options), read when the game is chosen.
    std::map<std::string, std::string> graphics;
    // The rooms this PC keeps open (Rooms.hpp), what is being typed in each, whether each server runs (read again
    // every two seconds) and the last thing that went wrong.
    std::vector<Room> rooms;
    std::map<std::wstring, std::array<char, 64>> roomNames, roomPasswords, roomPorts;
    std::map<std::wstring, bool> roomsRunning;
    ULONGLONG roomsCheckedAt = 0;
    bool roomsLoaded = false;
    std::wstring roomsProblem;
    // A game file whose extension fits several consoles waits here until the user says which.
    std::wstring askFile;
    Detected askDetected;
    CoverFetcher covers;
    Gamepads gamepads;
    // Home is the row of consoles; Console the grid of one console's games; Game the chosen game.
    enum class Page { Home, Console, Game };
    Page page = Page::Home;
    // Smooth motion: values that glide toward where they should be (Smooth), a page that comes in when it changes,
    // and whether anything still moves (so the window keeps drawing until it settles).
    std::map<std::string, float> glides;
    bool moving = false, movedLastFrame = false;
    // An animated photo or cover was drawn (this frame / the one before): it needs ~10 frames a second, not 60.
    bool animating = false, animatedLastFrame = false;
    std::string shownPage;
    double pageChangedAt = -10;

    float Smooth(const std::string& key, float target, float speed = 14.0f) {
        float& value = glides.try_emplace(key, target).first->second;
        value += (target - value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
        if (std::fabs(target - value) < 0.002f) value = target;
        else moving = true;
        return value;
    }

    static float EaseOut(float t) {
        t = std::clamp(t, 0.0f, 1.0f);
        return 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    }
    std::wstring console = L"PS5";
    int gridFocus = 0;
    bool gridMoved = false;
    float homeScroll = 0, homeTarget = 0;
    // Where the gamepad is on the home screen: 0 the row of consoles, 1 the buttons under it, 2 the console's games;
    // the column within the row, and how many games the last frame had room for.
    int homeRow = 0, homeColumn = 0, homeGamesShown = 0;
    int recentFocus = 0; // the game in focus in Continuar jogando (homeRow 3)
    // The game page has just opened: the focus goes to Jogar.
    bool focusPlay = false, focusEngines = false;
    // Frames left in which ImGui's focus ring is kept showing (ShowRing).
    int ringFrames = 0;
    // The gamepad on the row of buttons at the top (TopBar): which one, -1 when it is elsewhere; the one A pressed
    // this frame, -1 when none. And which of the three buttons of an empty console page it is on.
    int barFocus = -1, barPress = -1, emptyFocus = 0;
    std::wstring homeConsole;

    explicit Client(const std::wstring& root) : library(root), covers(root) {
        // Every PC but the one that hosts looks for a newer Ranga there when it opens (Update.hpp).
        // RANGA_CLIENT_AUTO_UPDATE=1 (tests): checks even in test mode and takes the update without asking.
        if (!RoomServerEnabled(root) && (_wgetenv(L"RANGA_CLIENT_TEST_INPUT") == nullptr || autoUpdate)) StartUpdateCheck();
        // RANGA_CLIENT_PAGE=novidades (tests): opens on the news board.
        if (const wchar_t* start = _wgetenv(L"RANGA_CLIENT_PAGE"); start != nullptr && std::wstring(start) == L"novidades") newsOpen = true;
        // RANGA_CLIENT_PAGE=jogo:<titleId> (tests): opens on that game's page.
        // (The library fills in later, so the game is looked for in the first frames, OpenStartGame.)
        if (const wchar_t* start = _wgetenv(L"RANGA_CLIENT_PAGE"); start != nullptr && std::wstring(start).rfind(L"jogo:", 0) == 0) startGame = std::wstring(start).substr(5);
        // RANGA_CLIENT_PAGE=chat (tests): opens on the profile's Chat tab.
        if (const wchar_t* start = _wgetenv(L"RANGA_CLIENT_PAGE"); start != nullptr && std::wstring(start) == L"chat") startChat = true;
        // RANGA_CLIENT_PAGE=apps (tests): opens on the Apps store (what is installed; the list is not fetched).
        if (const wchar_t* start = _wgetenv(L"RANGA_CLIENT_PAGE"); start != nullptr && std::wstring(start) == L"apps") storeOpen = true;
    }
    ~Client() {
        if (updateThread.joinable()) updateThread.join();
        if (checkThread.joinable()) checkThread.join();
        if (backupThread.joinable()) backupThread.join();
        if (cloudThread.joinable()) cloudThread.join();
        if (packageThread.joinable()) packageThread.join();
        if (githubThread.joinable()) githubThread.join();
        if (newsThread.joinable()) newsThread.join();
        if (storeThread.joinable()) storeThread.join();
        for (const auto& running : appProcesses) CloseHandle(running.second);
        StopProfileThreads();
    }

    // ---- Novidades: what changed in each version of Ranga ----
    // The history the hosting PC keeps (salas\novidades.txt, one "version<TAB>notes" line each, oldest first) and sends
    // with every release to GitHub; every other Ranga reads it there and keeps the last copy in perfil\novidades.txt.
    struct NewsItem {
        std::string version, notes, name; // name: the number (1.0.0), empty for the versions from before the numbers
    };
    std::vector<NewsItem> news; // newest first, under newsLock
    std::mutex newsLock;
    std::thread newsThread;
    std::atomic<bool> newsBusy{false};
    bool newsOpen = false, newsToastShown = false;
    ULONGLONG newsAskedAt = 0;

    static std::vector<NewsItem> ParseNews(const std::string& text) {
        std::vector<NewsItem> items;
        size_t start = 0;
        while (start < text.size()) {
            size_t end = text.find('\n', start);
            if (end == std::string::npos) end = text.size();
            std::string line = text.substr(start, end - start);
            start = end + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            const size_t tab = line.find('\t');
            if (tab == std::string::npos || tab == 0 || line.rfind("20", 0) != 0) continue;
            NewsItem item{line.substr(0, tab), line.substr(tab + 1), ""};
            // Since the numbers, a last field after another tab: "version<TAB>notes<TAB>1.0.0".
            if (const size_t last = item.notes.rfind('\t'); last != std::string::npos && VersionName(item.notes.substr(last + 1))) {
                item.name = item.notes.substr(last + 1);
                item.notes.resize(last);
            }
            items.push_back(std::move(item));
        }
        std::reverse(items.begin(), items.end());
        return items;
    }

    // Read again at most every half hour (and when the page opens), in the background.
    void RefreshNews(bool force) {
        if (newsBusy) return;
        const ULONGLONG now = GetTickCount64();
        if (!force && newsAskedAt != 0 && now - newsAskedAt < 30 * 60 * 1000) return;
        newsAskedAt = now;
        if (newsThread.joinable()) newsThread.join();
        newsBusy = true;
        newsThread = std::thread([this, root = library.Root()] {
            std::string text = ReadBytes(root + L"\\salas\\novidades.txt");
            const std::wstring cache = root + L"\\perfil\\novidades.txt";
            if (text.empty()) {
                const HttpResult answer = HttpRequest("GET", "https://github.com/iordan21/Ranga-EMU/releases/latest/download/novidades.txt");
                if (answer.status == 200 && !ParseNews(answer.body).empty()) {
                    text = answer.body;
                    CreateDirectoryW((root + L"\\perfil").c_str(), nullptr);
                    if (FILE* file = _wfopen(cache.c_str(), L"wb")) {
                        std::fwrite(text.data(), 1, text.size(), file);
                        std::fclose(file);
                    }
                } else {
                    text = ReadBytes(cache);
                }
            }
            auto items = ParseNews(text);
            {
                std::lock_guard guard(newsLock);
                if (!items.empty()) news = std::move(items);
            }
            newsBusy = false;
        });
    }

    // engines.ini read once and kept, not from disk every frame: the top bar (Novidades), the home (the version) and
    // a console's page (every game's favorite) asked it many times a frame, which with a big library was hundreds of
    // file reads a second for a still screen. Read again only when the file changed (its date, looked at once a frame
    // by RefreshIniCache); a write here shows from the next frame on.
    mutable std::map<std::wstring, std::wstring> iniCache;
    FILETIME iniStamp{};

    void RefreshIniCache() {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExW((library.Root() + L"\\engines.ini").c_str(), GetFileExInfoStandard, &data)) return;
        if (CompareFileTime(&data.ftLastWriteTime, &iniStamp) == 0) return;
        iniStamp = data.ftLastWriteTime;
        iniCache.clear();
    }

    std::wstring CachedIni(const wchar_t* section, const wchar_t* key, const wchar_t* fallback) const {
        const std::wstring id = std::wstring(section) + L"\n" + key;
        auto found = iniCache.find(id);
        if (found == iniCache.end()) {
            // The fallback is kept apart: one key read with two fallbacks must not mix them.
            const std::wstring value = ReadIni(library.Root() + L"\\engines.ini", section, key, L"\x1");
            found = iniCache.emplace(id, value).first;
        }
        return found->second == L"\x1" ? std::wstring(fallback) : found->second;
    }

    // How many versions came since the board was last opened. The first time ever only the newest counts.
    int UnseenNews() {
        std::lock_guard guard(newsLock);
        if (news.empty()) return 0;
        const std::string seen = Utf8(CachedIni(L"Ranga", L"novidades_vistas", L""));
        if (seen.empty()) return 1;
        int count = 0;
        for (const auto& item : news) count += item.version > seen ? 1 : 0;
        return count;
    }

    void MarkNewsSeen() {
        std::string newest;
        {
            std::lock_guard guard(newsLock);
            if (!news.empty()) newest = news.front().version;
        }
        if (newest.empty()) return;
        const std::wstring ini = library.Root() + L"\\engines.ini";
        EnsureUnicodeIni(ini);
        WritePrivateProfileStringW(L"Ranga", L"novidades_vistas", Wide(newest).c_str(), ini.c_str());
    }

    // "2026.11.20-2057" as "20/11/2026 às 20:57".
    static std::string NewsDate(const std::string& version) {
        if (version.size() < 15 || version[4] != '.' || version[7] != '.' || version[10] != '-') return version;
        return version.substr(8, 2) + "/" + version.substr(5, 2) + "/" + version.substr(0, 4) + " às " + version.substr(11, 2) + ":" + version.substr(13, 2);
    }

    void OpenNews() {
        newsOpen = true;
        storeOpen = false;
        profileOpen = false;
        enginesPage = false;
        RefreshNews(true);
    }

    template <typename NavT> void NewsPage(float scale, const NavT& nav) {
        RefreshNews(false);
        if (nav.back) {
            MarkNewsSeen();
            newsOpen = false;
            storeOpen = false;
            return;
        }
        const float left = 40 * scale;
        ImGui::SetCursorPos(ImVec2(left, 22 * scale));
        if (ImGui::Button("<  Voltar", ImVec2(0, 36 * scale))) {
            MarkNewsSeen();
            newsOpen = false;
            storeOpen = false;
            return;
        }
        ImGui::SameLine(0, 18 * scale);
        ImGui::PushFont(semibold, 28);
        ImGui::TextUnformatted("Novidades do Ranga");
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(left, 76 * scale));
        ImGui::TextDisabled("O que mudou em cada versão. O Ranga se atualiza sozinho quando abre.");
        std::vector<NewsItem> items;
        {
            std::lock_guard guard(newsLock);
            items = news;
        }
        const std::string seen = Utf8(CachedIni(L"Ranga", L"novidades_vistas", L""));
        const std::string installed = Utf8(CachedIni(L"Ranga", L"versao_atualizacao", L""));
        ImGui::SetCursorPos(ImVec2(left, 108 * scale));
        ImGui::BeginChild("##news", ImVec2(ImGui::GetContentRegionAvail().x - left, 0), ImGuiChildFlags_None);
        if (items.empty()) ImGui::TextDisabled(newsBusy ? "Buscando as novidades..." : "Ainda não deu para buscar as novidades (sem internet?).");
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < items.size(); ++i) {
            const auto& item = items[i];
            const bool fresh = seen.empty() ? i == 0 : item.version > seen;
            const float width = ImGui::GetContentRegionAvail().x;
            const ImVec2 top = ImGui::GetCursorScreenPos();
            const float pad = 16 * scale;
            draw->ChannelsSplit(2);
            draw->ChannelsSetCurrent(1);
            ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + pad));
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 18);
            ImGui::TextUnformatted(item.name.empty() ? NewsDate(item.version).c_str() : ("Versão " + item.name).c_str());
            ImGui::PopFont();
            if (!item.name.empty()) {
                ImGui::SameLine(0, 12 * scale);
                ImGui::TextDisabled("%s", NewsDate(item.version).c_str());
            }
            if (fresh || item.version == installed) {
                ImGui::SameLine(0, 12 * scale);
                ImGui::PushStyleColor(ImGuiCol_Text, Color(fresh ? kAccent : kGreen));
                ImGui::TextUnformatted(fresh ? "nova" : "a sua versão");
                ImGui::PopStyleColor();
            }
            ImGui::PushTextWrapPos(top.x + width - pad - ImGui::GetWindowPos().x);
            ImGui::TextUnformatted(item.notes.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            const float bottom = ImGui::GetItemRectMax().y + pad;
            draw->ChannelsSetCurrent(0);
            draw->AddRectFilled(top, ImVec2(top.x + width, bottom), Packed(fresh ? kSelected : kSurface), 12 * scale);
            draw->ChannelsMerge();
            ImGui::SetCursorScreenPos(ImVec2(top.x, bottom + 10 * scale));
            ImGui::Dummy(ImVec2(0, 0));
        }
        // The gamepad scrolls the list with up and down.
        if (nav.down) ImGui::SetScrollY(ImGui::GetScrollY() + 120 * scale);
        if (nav.up) ImGui::SetScrollY(std::max(0.0f, ImGui::GetScrollY() - 120 * scale));
        ImGui::EndChild();
    }

    // ---- Apps: the store of the owner's Windows programs (Store.hpp); the first is Ranga TV ----
    // A page of its own (the top bar's Apps button): each app turned on (downloaded, checked, unzipped), opened,
    // updated and removed from there, all by the gamepad too. While an app runs, the controllers are its, as a game's.
    std::thread storeThread;
    std::mutex storeLock;
    std::atomic<int> storeJob{0}; // 0: nothing; 1: fetching the list; 2: installing storeJobApp
    std::atomic<uint64_t> storeDone{0}, storeTotal{0};
    std::string storeJobApp;  // under storeLock
    StoreCatalog store;       // under storeLock
    std::wstring storeNote;   // under storeLock: how the last action went
    bool storeFetched = false, storeOpen = false, storeReadInstalled = false;
    int storeFocus = 0, storeButton = 0;
    // The apps opened from here that are still running (an app may stay on in the background: the music one with
    // only its mini player).
    std::vector<std::pair<std::string, HANDLE>> appProcesses;

    // Forgets the apps that closed; when one closed, Ranga comes back to the front.
    void ReapStoreApps() {
        bool closed = false;
        for (size_t i = 0; i < appProcesses.size();) {
            if (WaitForSingleObject(appProcesses[i].second, 0) == WAIT_TIMEOUT) {
                ++i;
                continue;
            }
            CloseHandle(appProcesses[i].second);
            appProcesses.erase(appProcesses.begin() + static_cast<std::ptrdiff_t>(i));
            closed = true;
        }
        if (closed && !StoreAppInFront()) SetForegroundWindow(window);
    }

    bool StoreAppRunning(const std::string& id) {
        ReapStoreApps();
        for (const auto& running : appProcesses)
            if (running.first == id) return true;
        return false;
    }

    // One of those apps is the window in front: the controller is its (Ranga lets go of it). An app that went to the
    // background (only a mini player) gives it back.
    bool StoreAppInFront() {
        if (appProcesses.empty()) return false;
        DWORD front = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &front);
        for (const auto& running : appProcesses)
            if (GetProcessId(running.second) == front) return true;
        return false;
    }

    void OpenStoreApp(const StoreApp& app) {
        // Already on (say, only its mini player): opening it again brings its window back (the app, single, hands
        // over to the one running and closes).
        const bool again = StoreAppRunning(app.id);
        if (runner.Busy()) {
            AddToast(app.name, "Feche o jogo antes de abrir o app.", "");
            return;
        }
        const std::wstring exe = StoreAppExe(library.Root(), app), folder = StoreAppFolder(library.Root(), app.id);
        // From the TV mode it gets the TV's arguments too (Ranga TV: full screen and bigger).
        std::wstring command = L"\"" + exe + L"\"";
        if (!app.args.empty()) command += L" " + app.args;
        if (tvMode && !app.tvArgs.empty()) command += L" " + app.tvArgs;
        // An app made for Ranga (it takes --ranga-emu) also learns which monitor Ranga is on, so it opens there and not
        // on Windows' main one (the TV may be the second screen).
        if (app.args.find(L"--ranga-emu") != std::wstring::npos) {
            MONITORINFO info{sizeof(info)};
            if (GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info)) {
                const RECT& r = info.rcMonitor;
                command += L" --monitor=" + std::to_wstring(r.left) + L"," + std::to_wstring(r.top) + L"," + std::to_wstring(r.right - r.left) + L"," +
                           std::to_wstring(r.bottom - r.top);
            }
        }
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        // Ranga is in front now: Windows lets the program it opens come to the front only with this (else the app's
        // window opens behind, and the controller would go nowhere).
        AllowSetForegroundWindow(ASFW_ANY);
        if (!CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(), &startup, &process)) {
            AddToast(app.name, "Não deu para abrir o app.", "");
            return;
        }
        CloseHandle(process.hThread);
        if (again) CloseHandle(process.hProcess);
        else appProcesses.emplace_back(app.id, process.hProcess);
    }

    // install: downloads and installs (or updates) that app, then fetches the list again; else only the list.
    void StartStoreJob(const StoreApp* install) {
        if (storeJob != 0) return;
        if (storeThread.joinable()) storeThread.join();
        storeJob = install != nullptr ? 2 : 1;
        storeDone = 0;
        storeTotal = install != nullptr ? install->size : 0;
        {
            std::lock_guard guard(storeLock);
            storeJobApp = install != nullptr ? install->id : "";
        }
        std::optional<StoreApp> app;
        if (install != nullptr) app = *install;
        storeThread = std::thread([this, app] {
            const std::wstring root = library.Root();
            std::wstring note;
            if (app) {
                note = InstallStoreApp(root, *app, [this](uint64_t done, uint64_t total) {
                    storeDone = done;
                    storeTotal = total;
                });
                if (note.empty()) note = Wide(app->name) + L" está pronto: é só abrir.";
            }
            StoreCatalog fetched = FetchStore(root);
            std::lock_guard guard(storeLock);
            store = fetched;
            // Only fetching the list keeps the last note ("removido").
            if (app) storeNote = note;
            storeFetched = true;
            storeJobApp.clear();
            storeJob = 0;
        });
    }

    void OpenStore() {
        storeOpen = true;
        newsOpen = false;
        profileOpen = false;
        enginesPage = false;
        storeFocus = storeButton = 0;
        {
            std::lock_guard guard(storeLock);
            storeNote.clear();
        }
        StartStoreJob(nullptr);
    }

    template <typename NavT> void StorePage(float scale, const NavT& nav) {
        if (nav.back) {
            storeOpen = false;
            return;
        }
        const std::wstring root = library.Root();
        const float left = 40 * scale;
        ImGui::SetCursorPos(ImVec2(left, 22 * scale));
        if (ImGui::Button("<  Voltar", ImVec2(0, 36 * scale))) {
            storeOpen = false;
            return;
        }
        ImGui::SameLine(0, 18 * scale);
        ImGui::PushFont(semibold, 28);
        ImGui::TextUnformatted("Apps");
        ImGui::PopFont();
        ImGui::SetCursorPos(ImVec2(left, 76 * scale));
        ImGui::TextDisabled("Programas do Ranga para o PC. Ative o que quiser: ele baixa assinado, é conferido e abre por aqui, com o controle.");
        ReapStoreApps();
        const int job = storeJob;
        std::lock_guard guard(storeLock);
        // Before the first answer: what is installed shows at once (read once, not every frame).
        if (!storeFetched && !storeReadInstalled) {
            store.apps = InstalledApps(root);
            storeReadInstalled = true;
        }
        const auto& apps = store.apps;

        // The gamepad: up and down pick the app, left and right its button, A presses it.
        storeFocus = std::clamp(storeFocus + (nav.down ? 1 : 0) - (nav.up ? 1 : 0), 0, std::max(0, static_cast<int>(apps.size()) - 1));
        if (nav.up || nav.down) storeButton = 0;
        storeButton = std::max(0, storeButton + (nav.right ? 1 : 0) - (nav.left ? 1 : 0));

        ImGui::SetCursorPos(ImVec2(left, 110 * scale));
        ImGui::BeginChild("##store", ImVec2(ImGui::GetContentRegionAvail().x - left, 0), ImGuiChildFlags_None);
        if (apps.empty()) ImGui::TextDisabled("%s", job == 1 ? "Buscando a lista de apps..." : Utf8(store.error).c_str());
        ImDrawList* draw = ImGui::GetWindowDrawList();
        for (size_t i = 0; i < apps.size(); ++i) {
            const StoreApp& app = apps[i];
            const bool focused = static_cast<int>(i) == storeFocus;
            const bool busyHere = job == 2 && storeJobApp == app.id;
            const bool running = StoreAppRunning(app.id);
            const ImVec2 top = ImGui::GetCursorScreenPos();
            const float width = std::min(ImGui::GetContentRegionAvail().x, 900 * scale), pad = 18 * scale;
            ImGui::PushID(static_cast<int>(i));
            draw->ChannelsSplit(2);
            draw->ChannelsSetCurrent(1);
            ImGui::SetCursorScreenPos(ImVec2(top.x + pad, top.y + pad));
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 24);
            ImGui::TextUnformatted(app.name.c_str());
            ImGui::PopFont();
            const std::string megabytes = std::to_string(std::max<uint64_t>(1, app.size / 1048576)) + " MB";
            std::string status;
            if (app.installed && app.newer) status = "Instalado (" + app.installedVersion + ")  ·  versão " + app.version + " disponível";
            else if (app.installed) status = "Instalado  ·  versão " + app.installedVersion + (running ? "  ·  aberto agora" : "");
            else status = "Versão " + app.version + "  ·  " + megabytes;
            ImGui::TextDisabled("%s", status.c_str());
            // PushTextWrapPos takes the window's own x (not the screen's): the card's inner right edge.
            ImGui::PushTextWrapPos(top.x + width - pad - ImGui::GetWindowPos().x + ImGui::GetScrollX());
            if (!app.description.empty()) ImGui::TextWrapped("%s", app.description.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 2 * scale));
            if (busyHere) {
                const uint64_t total = std::max<uint64_t>(storeTotal, 1);
                ImGui::Text("Baixando... %.0f de %.0f MB", static_cast<double>(storeDone) / 1048576.0, static_cast<double>(total) / 1048576.0);
                ImGui::ProgressBar(static_cast<float>(static_cast<double>(storeDone) / static_cast<double>(total)), ImVec2(420 * scale, 0));
            } else {
                // The buttons, in the order the gamepad walks them.
                std::vector<int> actions; // 0 open, 1 turn on, 2 update, 3 remove
                if (app.installed) actions.push_back(0);
                else if (!app.sha256.empty()) actions.push_back(1);
                if (app.installed && app.newer) actions.push_back(2);
                if (app.installed) actions.push_back(3);
                if (focused) storeButton = std::min(storeButton, std::max(0, static_cast<int>(actions.size()) - 1));
                for (size_t b = 0; b < actions.size(); ++b) {
                    if (b > 0) ImGui::SameLine(0, 10 * scale);
                    const int action = actions[b];
                    // Open works with the app on too (it comes back); updating and removing wait for it to close.
                    const bool off = job != 0 || (running && action != 0);
                    ImGui::BeginDisabled(off);
                    const bool primary = b == 0 && action != 3;
                    std::string label = action == 0 ? "Abrir" : action == 1 ? "Ativar e baixar (" + megabytes + ")" : action == 2 ? "Atualizar (" + megabytes + ")" : "Remover";
                    label += "##" + std::to_string(action);
                    if (primary) {
                        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
                        ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
                    }
                    const bool pressed = ImGui::Button(label.c_str(), ImVec2(0, 40 * scale)) ||
                                         (focused && storeButton == static_cast<int>(b) && nav.accept && !off);
                    if (primary) ImGui::PopStyleColor(3);
                    ImGui::EndDisabled();
                    FocusLast(focused && storeButton == static_cast<int>(b), scale);
                    if (!pressed) continue;
                    if (action == 0) {
                        pending = [this, app] { OpenStoreApp(app); };
                    } else if (action == 1 || action == 2) {
                        pending = [this, app] { StartStoreJob(&app); };
                    } else {
                        const std::wstring error = RemoveStoreApp(root, app.id);
                        storeNote = error.empty() ? Wide(app.name) + L" removido. O que ele guarda fora da pasta (listas, favoritos) fica." : error;
                        pending = [this] { StartStoreJob(nullptr); };
                    }
                }
            }
            ImGui::EndGroup();
            const ImVec2 bottom(top.x + width, ImGui::GetItemRectMax().y + pad);
            draw->ChannelsSetCurrent(0);
            draw->AddRectFilled(top, bottom, Packed(kSurface), 12 * scale);
            if (focused) draw->AddRect(top, bottom, Packed(kAccent, 0.6f), 12 * scale, 0, 2 * scale);
            draw->ChannelsMerge();
            ImGui::SetCursorScreenPos(ImVec2(top.x, bottom.y + 14 * scale));
            ImGui::Dummy(ImVec2(0, 0));
            ImGui::PopID();
        }
        if (!storeNote.empty()) ImGui::TextDisabled("%s", Utf8(storeNote).c_str());
        if (!store.error.empty() && !apps.empty()) ImGui::TextDisabled("%s", Utf8(store.error).c_str());
        if (!apps.empty()) {
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::TextDisabled("Direcional: escolher    A: apertar    B: voltar");
        }
        ImGui::EndChild();
    }

    // The update: checked in the background; when there is one, a bar at the bottom offers it.
    enum class UpdateState { Idle, Checking, Available, Downloading, Ready, Failed };
    std::atomic<UpdateState> updateState{UpdateState::Idle};
    std::atomic<uint64_t> updateDone{0}, updateTotal{0};
    std::thread updateThread;
    std::mutex updateLock;
    UpdateInfo updateInfo;
    std::wstring updateError;
    bool updateHidden = false;
    const bool autoUpdate = _wgetenv(L"RANGA_CLIENT_AUTO_UPDATE") != nullptr;
    std::array<char, 256> publishNotes{};
    std::wstring publishResult;
    // After publishing here, the same version goes up to GitHub (PublishToGitHub) in the background.
    std::thread githubThread;
    std::atomic<bool> githubBusy{false};
    std::wstring githubResult; // under updateLock

    void StartGitHubUpload() {
        if (githubBusy) return;
        if (githubThread.joinable()) githubThread.join();
        githubBusy = true;
        githubThread = std::thread([this, root = library.Root()] {
            std::string version;
            const std::wstring error = PublishToGitHub(root, version);
            std::lock_guard guard(updateLock);
            githubResult = error.empty() ? L"No GitHub também: " + Wide(version) + L" (os amigos baixam mesmo com este PC desligado)." : error;
            githubBusy = false;
        });
    }
    // The whole Ranga for a friend with none (MakeFriendPackage), made in the background.
    std::thread packageThread;
    std::atomic<bool> packageBusy{false};
    std::wstring packageResult; // under updateLock

    void StartFriendPackage(const std::wstring& folder) {
        if (packageBusy) return;
        if (packageThread.joinable()) packageThread.join();
        packageBusy = true;
        packageThread = std::thread([this, folder, root = library.Root()] {
            std::wstring made;
            const std::wstring error = MakeFriendPackage(root, folder, made);
            std::wstring result = error.empty() ? L"Pacote pronto: " + made + L" (" + std::to_wstring(FileSize(made) / (1024 * 1024)) + L" MB)" : error;
            std::lock_guard guard(updateLock);
            packageResult = result;
            packageBusy = false;
        });
    }

    // Emuladores > Arquivos do Ranga: the files checked against the signed list (CheckIntegrity), in the background.
    enum class CheckState { Idle, Checking, Done };
    std::atomic<CheckState> checkState{CheckState::Idle};
    std::atomic<int> checkDone{0}, checkTotal{0};
    std::thread checkThread;
    IntegrityReport checkReport; // under updateLock

    // Emuladores > Backup dos saves: a copy after each game (when something changed) and on the button.
    std::thread backupThread;
    std::atomic<bool> backupBusy{false};
    std::mutex backupLock;
    std::wstring backupResult; // under backupLock

    void StartBackup(bool force) {
        if (backupBusy || BackupFolder(library.Root()).empty()) return;
        if (backupThread.joinable()) backupThread.join();
        backupBusy = true;
        backupThread = std::thread([this, force, root = library.Root()] {
            std::wstring made;
            int files = 0;
            const std::wstring error = BackupNow(root, force, made, files);
            std::wstring result;
            if (!error.empty()) result = L"Não deu: " + error;
            else if (!made.empty()) result = L"Cópia feita: " + std::to_wstring(files) + L" arquivos em " + made;
            else result = L"Nada mudou desde a última cópia.";
            {
                std::lock_guard guard(backupLock);
                backupResult = result;
            }
            backupBusy = false;
        });
    }

    void BackupTab(float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::TextDisabled("Uma cópia dos seus saves numa pasta que você escolher (por exemplo a do Google Drive no PC, que sobe sozinha para a nuvem). "
                            "Ela é feita sozinha quando você fecha um jogo, se algo mudou; ficam as últimas %d cópias, cada uma numa pasta com a data. "
                            "Vão os saves dos jogos, os cartões de memória, os estados salvos e o seu perfil. Nunca jogos, BIOS, firmware ou chaves.",
                            kBackupsKept);
        ImGui::Dummy(ImVec2(0, 6 * scale));
        const std::wstring folder = BackupFolder(library.Root());
        ImGui::Text("Pasta do backup: %s", folder.empty() ? "nenhuma (backup desligado)" : Utf8(folder).c_str());
        if (ImGui::Button(folder.empty() ? "Escolher a pasta" : "Trocar a pasta", ImVec2(0, 36 * scale))) {
            pending = [this] {
                Browse(L"Escolha a pasta do backup dos saves", L"", true, {}, [this](const std::wstring& chosen) {
                    SetBackupFolder(library.Root(), chosen);
                    StartBackup(true);
                });
            };
        }
        if (!folder.empty()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(backupBusy);
            ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
            ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
            if (ImGui::Button("Fazer cópia agora", ImVec2(0, 36 * scale))) pending = [this] { StartBackup(true); };
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Abrir a pasta", ImVec2(0, 36 * scale))) {
                const std::wstring copies = folder + L"\\Ranga-backup";
                CreateDirectoryW(copies.c_str(), nullptr);
                ShellExecuteW(nullptr, L"open", copies.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::SameLine();
            if (ImGui::Button("Desligar", ImVec2(0, 36 * scale))) SetBackupFolder(library.Root(), L"");
            const long long last = LastBackup(library.Root());
            ImGui::TextDisabled("Última cópia: %s", last == 0 ? "nenhuma ainda" : FormatDate(last).c_str());
        }
        if (backupBusy) {
            ImGui::Text("Copiando...");
            moving = true;
        } else {
            std::lock_guard guard(backupLock);
            if (!backupResult.empty()) ImGui::TextWrapped("%s", Utf8(backupResult).c_str());
        }
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::TextDisabled("Para voltar um save: feche o Ranga, abra a cópia que quiser e copie a pasta de dentro dela por cima da do Ranga.");
        CloudSection(scale);
    }

    // ---- Save na nuvem (Cloud.hpp): after each game, and on the buttons; a look at the start ----

    enum class CloudJob { Check, Send, SendMine, Get, GetTheirs, Clear };
    bool openBackupTab = false; // Emuladores opens on Backup e nuvem (a cloud notice was clicked)
    std::thread cloudThread;
    std::atomic<bool> cloudBusy{false};
    std::atomic<int> cloudProgress{0};
    std::mutex cloudLock;
    CloudReport cloudReport; // under cloudLock: the last answer
    CloudJob cloudLastJob = CloudJob::Check;
    bool cloudFresh = false, cloudLooked = false;

    void StartCloud(CloudJob job) {
        if (cloudBusy || profile.session.empty() || profile.accountId.empty()) return;
        // Saves are not swapped under a running game.
        if ((job == CloudJob::Get || job == CloudJob::GetTheirs) && runner.Busy()) {
            Notify(L"Feche o jogo antes de baixar os saves da nuvem.");
            return;
        }
        if (cloudThread.joinable()) cloudThread.join();
        cloudBusy = true;
        cloudProgress = 0;
        cloudLastJob = job;
        cloudThread = std::thread([this, job, root = library.Root(), account = profile.accountId, session = profile.session] {
            CloudReport report;
            if (job == CloudJob::Check) report = CloudCheck(root, account, session);
            else if (job == CloudJob::Send || job == CloudJob::SendMine) report = CloudUpload(root, account, session, job == CloudJob::SendMine, &cloudProgress);
            else if (job == CloudJob::Clear) report.error = CloudClear(root, account, session), report.done = report.error.empty();
            else report = CloudDownload(root, account, session, job == CloudJob::GetTheirs, &cloudProgress);
            std::lock_guard guard(cloudLock);
            cloudReport = std::move(report);
            cloudFresh = true;
            cloudBusy = false;
        });
    }

    // Each frame: the first look once signed in, and a notice when the cloud has newer saves or a send went wrong.
    void TendCloud() {
        if (!cloudLooked && profileLoaded && !profile.session.empty() && CloudEnabled(library.Root()) && !cloudBusy) {
            cloudLooked = true;
            StartCloud(CloudJob::Check);
        }
        CloudReport report;
        CloudJob job;
        {
            std::lock_guard guard(cloudLock);
            if (!cloudFresh) return;
            cloudFresh = false;
            report = cloudReport;
            job = cloudLastJob;
        }
        const auto open = [this] {
            pending = [this] {
                enginesPage = true;
                profileOpen = false;
                newsOpen = false;
                storeOpen = false;
                RefreshEngines();
                openBackupTab = true;
            };
        };
        if (!report.error.empty() && job != CloudJob::Check) {
            AddToast("Save na nuvem", report.error, "", "", open);
        } else if ((job == CloudJob::Check || job == CloudJob::Send) && !report.newer.empty()) {
            const std::string from = report.newerFrom.empty() ? "" : " (de " + report.newerFrom + ")";
            AddToast("Saves mais novos na nuvem" + from, std::to_string(report.newer.size()) + " arquivo(s)  ·  clique para baixar", "", "", [this] {
                pending = [this] {
                    Confirm(L"Baixar os saves mais novos da nuvem? Os que estão neste PC vão antes para backups\\nuvem, por garantia.", [this] { StartCloud(CloudJob::Get); });
                };
            });
        } else if (!report.conflicts.empty() && job != CloudJob::Get) {
            AddToast("Save na nuvem: escolha qual vale", std::to_string(report.conflicts.size()) + " save(s) mudaram aqui e em outro PC  ·  clique para ver", "", "", open);
        } else if (job == CloudJob::Send && report.sent > 0) {
            AddToast("Save na nuvem", std::to_string(report.sent) + " save(s) enviados", "");
        } else if ((job == CloudJob::Get || job == CloudJob::GetTheirs) && report.done) {
            AddToast("Save na nuvem", report.got > 0 ? std::to_string(report.got) + " save(s) baixados da nuvem" : "Nada novo na nuvem", "");
        }
    }

    void CloudSection(float scale) {
        ImGui::Dummy(ImVec2(0, 14 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushFont(semibold, 20);
        ImGui::TextUnformatted("Save na nuvem");
        ImGui::PopFont();
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Seus saves ficam guardados na sua conta do Ranga e você continua de outro PC: ao fechar um jogo, o que mudou sobe sozinho; "
                            "ao abrir o Ranga em outro PC, ele avisa quando a nuvem tem um save mais novo. Vão só os saves e cartões de memória "
                            "(nada de jogos, BIOS, chaves ou estados salvos), até 64 MB comprimidos por conta.");
        ImGui::PopTextWrapPos();
        if (profile.session.empty()) {
            ImGui::TextDisabled("Entre na sua conta do Ranga (Perfil) para usar.");
            return;
        }
        bool on = CloudEnabled(library.Root());
        if (ImGui::Checkbox("Guardar meus saves na nuvem", &on)) {
            SetCloudEnabled(library.Root(), on);
            if (on) StartCloud(CloudJob::Check);
        }
        if (!on) return;
        CloudReport report;
        {
            std::lock_guard guard(cloudLock);
            report = cloudReport;
        }
        ImGui::BeginDisabled(cloudBusy);
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
        ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
        if (ImGui::Button("Enviar agora", ImVec2(0, 36 * scale))) StartCloud(CloudJob::Send);
        ImGui::PopStyleColor(3);
        ImGui::SameLine();
        if (ImGui::Button("Baixar da nuvem", ImVec2(0, 36 * scale)))
            Confirm(L"Baixar os saves mais novos da nuvem? Os que estão neste PC vão antes para backups\\nuvem, por garantia.", [this] { StartCloud(CloudJob::Get); });
        ImGui::SameLine();
        if (ImGui::Button("Conferir", ImVec2(0, 36 * scale))) StartCloud(CloudJob::Check);
        ImGui::SameLine();
        if (ImGui::Button("Apagar da nuvem", ImVec2(0, 36 * scale)))
            Confirm(L"Apagar todos os seus saves da nuvem? Os deste PC ficam; só a cópia na sua conta some.", [this] { StartCloud(CloudJob::Clear); });
        ImGui::EndDisabled();
        if (cloudBusy) {
            ImGui::Text("Falando com a nuvem... %d arquivo(s)", cloudProgress.load());
            moving = true;
            return;
        }
        if (report.quota > 0) ImGui::TextDisabled("Usado: %.1f de %.0f MB", report.used / 1048576.0, report.quota / 1048576.0);
        if (!report.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextWrapped("%s", report.error.c_str());
            ImGui::PopStyleColor();
        }
        if (!report.newer.empty()) ImGui::Text("%zu save(s) mais novos na nuvem%s: use Baixar da nuvem.", report.newer.size(),
                                               report.newerFrom.empty() ? "" : (" (de " + report.newerFrom + ")").c_str());
        if (!report.conflicts.empty()) {
            // Both sides changed: the player chooses, nothing goes on its own.
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextWrapped("%zu save(s) mudaram aqui e em outro PC desde a última vez. Escolha qual vale:", report.conflicts.size());
            ImGui::PopStyleColor();
            for (size_t i = 0; i < report.conflicts.size() && i < 6; ++i) ImGui::BulletText("%s", Utf8(report.conflicts[i]).c_str());
            if (report.conflicts.size() > 6) ImGui::TextDisabled("e mais %zu", report.conflicts.size() - 6);
            if (ImGui::Button("Usar os da nuvem", ImVec2(0, 32 * scale)))
                Confirm(L"Trocar os saves deste PC pelos da nuvem? Os daqui vão antes para backups\\nuvem.", [this] { StartCloud(CloudJob::GetTheirs); });
            ImGui::SameLine();
            if (ImGui::Button("Usar os deste PC", ImVec2(0, 32 * scale)))
                Confirm(L"Mandar os saves deste PC por cima dos da nuvem? A versão do outro PC sai da nuvem.", [this] { StartCloud(CloudJob::SendMine); });
        }
        if (report.done && report.error.empty() && report.newer.empty() && report.conflicts.empty())
            ImGui::TextDisabled("Tudo em dia%s.", report.sent > 0 ? (" (" + std::to_string(report.sent) + " enviado(s) agora)").c_str() : "");
    }

    void StartIntegrityCheck() {
        if (checkState == CheckState::Checking) return;
        if (checkThread.joinable()) checkThread.join();
        checkState = CheckState::Checking;
        checkDone = 0;
        checkTotal = 0;
        checkThread = std::thread([this] {
            IntegrityReport report = CheckIntegrity(library.Root(), [this](int done, int total) {
                checkDone = done;
                checkTotal = total;
            });
            std::lock_guard guard(updateLock);
            checkReport = std::move(report);
            checkState = CheckState::Done;
        });
    }

    // Fetches the missing and broken files again, through the update's own download (the bar at the bottom shows it
    // and reopens Ranga when they are in place).
    void StartRepair() {
        {
            std::lock_guard guard(updateLock);
            updateInfo = checkReport.repair;
        }
        updateHidden = false;
        StartUpdateDownload();
    }

    void IntegrityTab(float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::TextDisabled("Confere o RangaEmu.exe, o ranga_retro.exe e os núcleos (cores) com a lista assinada da última versão: o que faltar ou estiver "
                            "diferente (corrompido) pode ser baixado de novo. Emuladores, BIOS, saves e jogos não fazem parte da atualização e não entram aqui.");
        ImGui::Dummy(ImVec2(0, 4 * scale));
        const CheckState state = checkState;
        ImGui::BeginDisabled(state == CheckState::Checking);
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
        ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
        if (ImGui::Button(state == CheckState::Done ? "Verificar de novo" : "Verificar agora", ImVec2(190 * scale, 38 * scale))) pending = [this] { StartIntegrityCheck(); };
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::Dummy(ImVec2(0, 6 * scale));
        if (state == CheckState::Checking) {
            const int total = std::max(1, checkTotal.load());
            ImGui::Text("Conferindo os arquivos... %d de %d", checkDone.load(), checkTotal.load());
            ImGui::ProgressBar(static_cast<float>(checkDone) / static_cast<float>(total), ImVec2(420 * scale, 0), "");
            moving = true; // keeps the window drawing while it counts
            return;
        }
        if (state != CheckState::Done) return;
        std::lock_guard guard(updateLock);
        const IntegrityReport& report = checkReport;
        if (!report.error.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextWrapped("%s", Utf8(report.error).c_str());
            ImGui::PopStyleColor();
            return;
        }
        if (report.missing.empty() && report.different.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
            ImGui::Text("Tudo certo: %d arquivos conferidos com a versão %s.", report.checked, report.version.c_str());
            ImGui::PopStyleColor();
            return;
        }
        ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
        ImGui::Text("%d de %d arquivos com problema (versão %s):", static_cast<int>(report.missing.size() + report.different.size()), report.checked,
                    report.version.c_str());
        ImGui::PopStyleColor();
        for (const std::wstring& path : report.missing) ImGui::BulletText("%s  -  faltando", Utf8(path).c_str());
        for (const std::wstring& path : report.different) ImGui::BulletText("%s  -  diferente (corrompido ou de outra versão)", Utf8(path).c_str());
        ImGui::Dummy(ImVec2(0, 4 * scale));
        if (report.host) {
            ImGui::TextWrapped("Este PC é o que publica: aqui a comparação é com a versão publicada. Diferença costuma ser um Ranga mais novo que ainda "
                               "não foi publicado; publique (sala dos clássicos) para igualar. Nada é baixado neste PC.");
            return;
        }
        if (report.behind) ImGui::TextDisabled("Este PC estava numa versão mais velha: parte disso é só a atualização, que vem junto.");
        const UpdateState update = updateState;
        const bool busy = runner.Busy() || update == UpdateState::Downloading || update == UpdateState::Ready;
        ImGui::BeginDisabled(busy);
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
        ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
        const std::string label = "Baixar de novo (" + std::to_string(std::max<uint64_t>(1, report.repair.download / (1024 * 1024))) + " MB)";
        if (ImGui::Button(label.c_str(), ImVec2(0, 38 * scale))) pending = [this] { StartRepair(); };
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        if (runner.Busy()) ImGui::TextDisabled("Feche o jogo para consertar.");
        else ImGui::TextDisabled("Só esses arquivos são baixados, e cada um é conferido antes de entrar no lugar. No fim o Ranga reabre já consertado.");
    }

    void StartUpdateCheck() {
        updateState = UpdateState::Checking;
        updateThread = std::thread([this] {
            UpdateInfo info = CheckUpdate(library.Root());
            std::lock_guard guard(updateLock);
            updateInfo = info;
            if (!info.error.empty()) {
                updateState = UpdateState::Idle;
            } else if (info.changed.empty()) {
                // Already this version's files: only the version is noted.
                if (info.version != InstalledUpdate(library.Root())) ApplyUpdate(library.Root(), info);
                updateState = UpdateState::Idle;
            } else {
                updateState = UpdateState::Available;
            }
        });
    }

    void StartUpdateDownload() {
        if (updateThread.joinable()) updateThread.join();
        updateState = UpdateState::Downloading;
        updateDone = 0;
        updateTotal = updateInfo.download;
        updateThread = std::thread([this] {
            UpdateInfo info;
            {
                std::lock_guard guard(updateLock);
                info = updateInfo;
            }
            const std::wstring error = DownloadUpdate(library.Root(), info, [this](uint64_t done, uint64_t total) {
                updateDone = done;
                updateTotal = total;
            });
            std::lock_guard guard(updateLock);
            updateError = error;
            updateState = error.empty() ? UpdateState::Ready : UpdateState::Failed;
        });
    }

    // Puts the files in place and opens the new Ranga; this one closes.
    void FinishUpdate() {
        const std::wstring error = ApplyUpdate(library.Root(), updateInfo);
        if (!error.empty()) {
            updateError = error;
            updateState = UpdateState::Failed;
            return;
        }
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION process{};
        std::wstring command = L"\"" + library.Root() + L"\\RangaEmu.exe\" --atualizado";
        // The new one waits for this one's single-instance lock to go (it retries for a few seconds).
        if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, library.Root().c_str(), &startup, &process)) {
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
        }
        PostQuitMessage(0);
    }

    void UpdateBar(float scale) {
        const UpdateState state = updateState;
        if (autoUpdate && !pending && state == UpdateState::Available) pending = [this] { StartUpdateDownload(); };
        if (autoUpdate && !pending && state == UpdateState::Ready) pending = [this] { FinishUpdate(); };
        if (state == UpdateState::Idle || state == UpdateState::Checking || updateHidden) return;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = std::min(620 * scale, viewport->WorkSize.x - 32 * scale);
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width - 16 * scale, viewport->WorkPos.y + viewport->WorkSize.y - 16 * scale), ImGuiCond_Always, ImVec2(0, 1));
        ImGui::SetNextWindowSize(ImVec2(width, 0));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Color(kSurface));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16 * scale, 12 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * scale);
        ImGui::Begin("##update", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize);
        std::lock_guard guard(updateLock);
        ImGui::PushTextWrapPos(0);
        if (state == UpdateState::Available) {
            const double megabytes = static_cast<double>(updateInfo.download) / (1024.0 * 1024.0);
            ImGui::Text("Atualização do Ranga disponível (%s), %.0f MB", updateInfo.version.c_str(), megabytes);
            if (!updateInfo.notes.empty()) ImGui::TextDisabled("%s", updateInfo.notes.c_str());
            const bool busy = runner.Busy();
            ImGui::BeginDisabled(busy);
            ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
            if (ImGui::Button("Atualizar agora")) pending = [this] { StartUpdateDownload(); };
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Depois")) updateHidden = true;
            if (busy) ImGui::TextDisabled("Feche o jogo para atualizar.");
        } else if (state == UpdateState::Downloading) {
            const uint64_t total = std::max<uint64_t>(updateTotal, 1);
            ImGui::Text("Baixando a atualização %s...", updateInfo.version.c_str());
            ImGui::ProgressBar(static_cast<float>(static_cast<double>(updateDone) / static_cast<double>(total)), ImVec2(-1, 0));
        } else if (state == UpdateState::Ready) {
            ImGui::Text("Atualização %s baixada e conferida.", updateInfo.version.c_str());
            ImGui::BeginDisabled(runner.Busy());
            ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
            if (ImGui::Button("Reabrir o Ranga atualizado")) pending = [this] { FinishUpdate(); };
            ImGui::PopStyleColor();
            ImGui::EndDisabled();
        } else if (state == UpdateState::Failed) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::Text("A atualização não deu certo: %s", Utf8(updateError).c_str());
            ImGui::PopStyleColor();
            ImGui::TextDisabled("Nada do Ranga foi trocado pela metade; ele segue como estava.");
            if (ImGui::Button("Tentar de novo")) pending = [this] { StartUpdateDownload(); };
            ImGui::SameLine();
            if (ImGui::Button("Fechar")) updateHidden = true;
        }
        ImGui::PopTextWrapPos();
        ImGui::End();
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
    }

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

    static bool ClassicOnline(const std::wstring& system) {
        static const wchar_t* consoles[] = {L"NES", L"SNES", L"MD", L"SMS", L"SEGACD", L"PCE", L"A2600", L"ARCADE", L"GB", L"GBA",
                                            L"PS1", L"N64", L"SATURN", L"NDS", L"DC", L"GC", L"WII", L"PS2"};
        for (const wchar_t* id : consoles) if (system == id) return true;
        return false;
    }

    std::wstring RememberedOnline(const wchar_t* key) const { return ReadIni(library.Root() + L"\\engines.ini", L"Ranga", key, L""); }
    void RememberOnline(const wchar_t* key, const std::string& value) {
        const std::wstring ini = library.Root() + L"\\engines.ini";
        EnsureUnicodeIni(ini);
        WritePrivateProfileStringW(L"Ranga", key, Wide(value).c_str(), ini.c_str());
    }

    void LoadOnline(const Game& game) {
        onlineOn = ReadCoreOption(game, "ranga_online") == "ligado";
        const auto fill = [&](std::array<char, 128>& field, const char* key, std::string remembered) {
            const std::string own = ReadCoreOption(game, key);
            const std::string value = own.empty() ? remembered : own;
            field.fill(0);
            value.copy(field.data(), field.size() - 1);
            if (value != own && !value.empty()) WriteCoreOption(game, key, value);
        };
        // What was last typed for any game; the name falls back to the Switch player's.
        std::string name = Utf8(RememberedOnline(L"online_nome"));
        if (name.empty()) name = Utf8(RememberedSwitch(L"jogador"));
        fill(onlineAddress, "ranga_online_endereco", Utf8(RememberedOnline(L"online_endereco")));
        fill(onlineName, "ranga_online_nome", name);
        const std::string delayText = ReadCoreOption(game, "ranga_online_atraso");
        const int delay = std::atoi(delayText.c_str());
        onlineDelayAuto = delayText.empty() || delayText == "auto";
        onlineDelay = delay >= 2 && delay <= 8 ? delay : 4;
        LoadServerPorts();
    }

    void LoadServerPorts() {
        for (const Extra extra : {Extra::Classics, Extra::Rooms3ds, Extra::Ps3}) {
            const std::string port = std::to_string(ExtraPort(library.Root(), extra));
            auto& field = serverPort[static_cast<int>(extra)];
            field.fill(0);
            port.copy(field.data(), field.size() - 1);
        }
    }

    void Load3dsRoom(const Game& game) {
        room3dsOn = ReadCoreOption(game, "citra_sala") == "entrar";
        const auto fill = [&](std::array<char, 128>& field, const char* key, std::string remembered) {
            const std::string own = ReadCoreOption(game, key);
            const std::string value = own.empty() ? remembered : own;
            field.fill(0);
            value.copy(field.data(), field.size() - 1);
            if (value != own && !value.empty()) WriteCoreOption(game, key, value);
        };
        std::string name = Utf8(RememberedOnline(L"online_nome"));
        if (name.empty()) name = Utf8(RememberedSwitch(L"jogador"));
        fill(room3dsAddress, "citra_sala_endereco", Utf8(RememberedOnline(L"sala3ds_endereco")));
        fill(room3dsName, "citra_sala_nome", name);
        LoadServerPorts();
    }

    Game* Current() { return selected >= 0 && selected < static_cast<int>(library.Games().size()) ? &library.Games()[static_cast<size_t>(selected)] : nullptr; }

    void Select(int index) {
        selected = index;
        logText.clear();
        logDirty = true;
        addons.clear();
        pspLanAddress.fill(0);
        xboxHubAddress.fill(0);
        ps3ServerAddress.fill(0);
        if (Game* game = Current()) {
            options = LoadOptions(*game);
            addons = LoadAddons(*game);
            dlss5Kit = InspectDlss5(library.Root());
            gameVideo = ReadBytes(game->workspace + L"\\video.txt");
            if (game->system == L"SWITCH") {
                const std::string mode = ReadCoreOption(*game, "eden_sala");
                roomMode = mode == "entrar" ? 1 : mode == "criar" ? 2 : 0;
                roomPublic = ReadCoreOption(*game, "ranga_sala_tipo") == "publica";
                // The room comes filled in: what this game has, else what was last typed for any Switch game, and
                // the room's usual port. What comes from elsewhere is written to the game, where the core reads it.
                const auto fill = [&](std::array<char, 128>& field, const char* key, const wchar_t* remembered, const char* usual) {
                    const std::string own = ReadCoreOption(*game, key);
                    std::string value = own.empty() ? Utf8(RememberedSwitch(remembered)) : own;
                    if (value.empty()) value = usual;
                    field.fill(0);
                    value.copy(field.data(), field.size() - 1);
                    if (value != own) WriteCoreOption(*game, key, value);
                };
                fill(roomAddress, "eden_sala_endereco", L"sala_endereco", roomPublic ? "" : kPrivateSwitchRoom);
                // A public room is only the one picked from the list, never the private one remembered for the others.
                if (roomPublic && roomAddress.data() == Utf8(RememberedSwitch(L"sala_endereco"))) SetRoomField(*game, roomAddress, "eden_sala_endereco", "");
                fill(roomPort, "eden_sala_porta", L"sala_porta", "24872");
                fill(roomPassword, "eden_sala_senha", L"sala_senha", "");
                // The player's name is one for every Switch game: the one last typed wins over what the game kept.
                std::string name = Utf8(RememberedSwitch(L"jogador"));
                if (name.empty()) name = ReadCoreOption(*game, "eden_nome");
                if (name.empty()) name = ReadCoreOption(*game, "eden_sala_apelido");
                roomNickname.fill(0);
                name.copy(roomNickname.data(), roomNickname.size() - 1);
                if (!name.empty()) SavePlayerName(*game, name, false);
                LoadContent(*game);
                graphics.clear();
                for (const auto& option : kSwitchGraphics) graphics[option.key] = ReadCoreOption(*game, option.key);
            }
            if (ClassicOnline(game->system)) LoadOnline(*game);
            if (game->system == L"3DS") Load3dsRoom(*game);
            if (const auto* list = ConsoleGraphics(game->system); list != nullptr && EngineFor(game->system).native) {
                graphics.clear();
                for (const auto& option : *list) graphics[option.key] = ReadCoreOption(*game, option.key);
            }
            library.Select(game->titleId);
            // The Bloodborne port and the emulators write everything to game.log; PS5 games put their errors in game.err.
            logFile = game->system != L"PS5" ? 1 : 0;
        }
    }

    // What was last typed for a Switch game (the player's name, the room), kept beside the emulators' choices so the
    // next game starts with it.
    // The hosting PC's Switch room through its playit tunnel (UDP), for anyone joining the private room.
    static constexpr const char* kPrivateSwitchRoom = RANGA_SALA_SWITCH_PRIVADA;

    void RefreshLobby(bool force) {
        if (lobbyBusy) return;
        const ULONGLONG now = GetTickCount64();
        if (!force && lobbyAskedAt != 0 && now - lobbyAskedAt < 15000) return;
        if (lobbyThread.joinable()) lobbyThread.join();
        lobbyAskedAt = now;
        lobbyBusy = true;
        lobbyThread = std::thread([this] {
            std::vector<LobbyRoom> rooms;
            std::string error;
            const bool ok = FetchSwitchLobby(rooms, error);
            if (ok) {
                // Every room's machine is asked at once (24 at a time), so the list is not held up by those that
                // never answer ping.
                std::vector<std::string> addresses;
                for (const auto& room : rooms) if (std::find(addresses.begin(), addresses.end(), room.address) == addresses.end()) addresses.push_back(room.address);
                std::vector<int> pings(addresses.size(), -1);
                for (size_t start = 0; start < addresses.size(); start += 24) {
                    std::vector<std::thread> batch;
                    for (size_t i = start; i < std::min(addresses.size(), start + 24); ++i) batch.emplace_back([&, i] { pings[i] = PingAddress(addresses[i], 700); });
                    for (auto& thread : batch) thread.join();
                }
                for (auto& room : rooms) room.ping = pings[static_cast<size_t>(std::find(addresses.begin(), addresses.end(), room.address) - addresses.begin())];
            }
            std::lock_guard guard(lobbyLock);
            if (ok) {
                lobby = std::move(rooms);
                lobbyAt = static_cast<long long>(std::time(nullptr));
                lobbyProblem.clear();
            } else {
                lobbyProblem = error;
            }
            lobbyBusy = false;
        });
    }

    void SetRoomField(const Game& game, std::array<char, 128>& field, const char* key, const std::string& value) {
        field.fill(0);
        value.copy(field.data(), field.size() - 1);
        WriteCoreOption(game, key, value);
    }

    // Private: back to the room last typed (the hosting PC's through playit when none was). Public: the list.
    void SetRoomPublic(const Game& game, bool on) {
        roomPublic = on;
        WriteCoreOption(game, "ranga_sala_tipo", on ? "publica" : "privada");
        if (on) {
            // Nothing chosen yet: the game must not go to the private room by mistake.
            SetRoomField(game, roomAddress, "eden_sala_endereco", "");
            SetRoomField(game, roomPassword, "eden_sala_senha", "");
            RefreshLobby(true);
            return;
        }
        std::string address = Utf8(RememberedSwitch(L"sala_endereco")), port = Utf8(RememberedSwitch(L"sala_porta"));
        SetRoomField(game, roomAddress, "eden_sala_endereco", address.empty() ? kPrivateSwitchRoom : address);
        SetRoomField(game, roomPort, "eden_sala_porta", port.empty() ? "24872" : port);
        SetRoomField(game, roomPassword, "eden_sala_senha", Utf8(RememberedSwitch(L"sala_senha")));
    }

    // Eden's public rooms for this game: how many are playing it in each, how many are in each, the ping, the lock.
    void PublicRooms(const Game& game, float labelWidth, float scale) {
        RefreshLobby(false);
        const uint64_t titleId = SwitchTitleId(game.folder);
        const std::string loose = LooseTitle(Utf8(game.title));
        const auto sameGame = [&](uint64_t id, const std::string& name) {
            // An update or add-on of the game differs from it only in the last three hex digits of its id.
            if (titleId != 0 && id != 0) return (id ^ titleId) < 0x1000;
            const std::string other = LooseTitle(name);
            return !loose.empty() && !other.empty() && (other.find(loose) != std::string::npos || loose.find(other) != std::string::npos);
        };
        std::vector<LobbyRoom> rooms;
        std::string problem;
        long long at = 0;
        {
            std::lock_guard guard(lobbyLock);
            rooms = lobby;
            problem = lobbyProblem;
            at = lobbyAt;
        }
        struct Row {
            const LobbyRoom* room;
            int playing;
            bool preferred;
        };
        std::vector<Row> rows;
        for (const auto& room : rooms) {
            int playing = 0;
            for (const auto& player : room.players) playing += sameGame(player.gameId, player.gameName);
            const bool preferred = sameGame(room.preferredGameId, room.preferredGameName);
            if (lobbyAll || playing > 0 || preferred) rows.push_back({&room, playing, preferred});
        }
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.playing != b.playing) return a.playing > b.playing;
            if (a.preferred != b.preferred) return a.preferred;
            const int pa = a.room->ping < 0 ? 100000 : a.room->ping, pb = b.room->ping < 0 ? 100000 : b.room->ping;
            if (pa != pb) return pa < pb;
            return a.room->players.size() > b.room->players.size();
        });

        ImGui::SetCursorPosX(labelWidth);
        ImGui::BeginGroup();
        if (ImGui::Button(lobbyBusy ? "Atualizando..." : "Atualizar")) RefreshLobby(true);
        ImGui::SameLine();
        ImGui::Checkbox("Mostrar salas de outros jogos", &lobbyAll);
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (at != 0) ImGui::TextDisabled("%d salas · atualiza sozinha a cada 15 s (%s)", static_cast<int>(rows.size()), FormatAgo(at).c_str());
        else if (lobbyBusy) ImGui::TextDisabled("Buscando as salas...");
        if (!problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextUnformatted(problem.c_str());
            ImGui::PopStyleColor();
        }
        const std::string chosenAddress = roomAddress.data();
        const int chosenPort = std::atoi(roomPort.data());
        const LobbyRoom* chosen = nullptr;
        const float width = ImGui::GetContentRegionAvail().x;
        if (ImGui::BeginTable("##publicRooms", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp,
                              ImVec2(width, 280 * scale))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Sala", ImGuiTableColumnFlags_WidthStretch, 4.0f);
            ImGui::TableSetupColumn("Neste jogo", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableSetupColumn("Na sala", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Ping", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Senha", ImGuiTableColumnFlags_WidthStretch, 0.7f);
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < rows.size(); ++i) {
                const LobbyRoom& room = *rows[i].room;
                const bool selected = room.address == chosenAddress && room.port == chosenPort;
                if (selected) chosen = &room;
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 28 * scale);
                ImGui::TableNextColumn();
                ImGui::PushID(static_cast<int>(i));
                // A name that was only emoji has no letter left: the room's address says which it is.
                const bool hasLetter = std::any_of(room.name.begin(), room.name.end(), [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; });
                const std::string name = hasLetter ? room.name : "Sala " + room.address + ":" + std::to_string(room.port);
                if (ImGui::Selectable(name.c_str(), selected, ImGuiSelectableFlags_SpanAllColumns)) {
                    SetRoomField(game, roomAddress, "eden_sala_endereco", room.address);
                    SetRoomField(game, roomPort, "eden_sala_porta", std::to_string(room.port));
                    SetRoomField(game, roomPassword, "eden_sala_senha", "");
                }
                if (ImGui::IsItemHovered()) {
                    std::string who;
                    for (const auto& player : room.players) {
                        who += player.nickname + (player.gameName.empty() ? "  (no menu)" : "  ·  " + player.gameName) + "\n";
                    }
                    ImGui::SetTooltip("%s\nJogo da sala: %s\n%s:%d\n\n%s", name.c_str(), room.preferredGameName.c_str(), room.address.c_str(), room.port,
                                      who.empty() ? "Ninguém dentro agora." : who.c_str());
                }
                ImGui::PopID();
                ImGui::TableNextColumn();
                if (rows[i].playing > 0) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
                    ImGui::Text("%d jogando", rows[i].playing);
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("0");
                }
                ImGui::TableNextColumn();
                ImGui::Text("%d / %d", static_cast<int>(room.players.size()), room.maxPlayers);
                ImGui::TableNextColumn();
                if (room.ping < 0) ImGui::TextDisabled("-");
                else {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(room.ping <= 60 ? kGreen : room.ping <= 130 ? kAmber : kRed));
                    ImGui::Text("%d ms", room.ping);
                    ImGui::PopStyleColor();
                }
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", room.hasPassword ? "sim" : "-");
            }
            ImGui::EndTable();
        }
        if (rows.empty() && at != 0) ImGui::TextDisabled("Nenhuma sala pública com este jogo agora. Marque \"Mostrar salas de outros jogos\" para ver todas.");
        if (chosen != nullptr) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
            ImGui::Text("Escolhida: %s", chosen->name.c_str());
            ImGui::PopStyleColor();
            if (chosen->hasPassword) {
                ImGui::TextDisabled("Esta sala tem senha:");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(220 * scale);
                ImGui::InputText("##publicPassword", roomPassword.data(), roomPassword.size(), ImGuiInputTextFlags_Password);
                if (ImGui::IsItemDeactivatedAfterEdit()) WriteCoreOption(game, "eden_sala_senha", roomPassword.data());
            }
        } else if (!rows.empty()) {
            ImGui::TextDisabled("Clique numa sala para entrar nela quando o jogo abrir.");
        }
        ImGui::EndGroup();
    }

    std::wstring RememberedSwitch(const wchar_t* key) const { return ReadIni(library.Root() + L"\\engines.ini", L"SWITCH", key, L""); }
    void RememberSwitch(const wchar_t* key, const std::string& value) {
        const std::wstring ini = library.Root() + L"\\engines.ini";
        EnsureUnicodeIni(ini);
        WritePrivateProfileStringW(L"SWITCH", key, value.empty() ? nullptr : Wide(value).c_str(), ini.c_str());
    }

    // The name is the console user's name in the game and the nickname in a room.
    void SavePlayerName(const Game& game, const std::string& name, bool remember) {
        if (ReadCoreOption(game, "eden_nome") != name) WriteCoreOption(game, "eden_nome", name);
        if (ReadCoreOption(game, "eden_sala_apelido") != name) WriteCoreOption(game, "eden_sala_apelido", name);
        if (remember) RememberSwitch(L"jogador", name);
    }

    void LoadContent(const Game& game) {
        contentFiles.clear();
        contentKinds.clear();
        const std::wstring list = Wide(ReadCoreOption(game, "eden_conteudo"));
        for (size_t start = 0; start <= list.size();) {
            size_t end = list.find(L'|', start);
            if (end == std::wstring::npos) end = list.size();
            if (end > start) contentFiles.push_back(list.substr(start, end - start));
            start = end + 1;
        }
        for (const auto& file : contentFiles) contentKinds.push_back(DetectSwitchContent(file));
    }

    void SaveContent(const Game& game) {
        std::wstring list;
        for (const auto& file : contentFiles) list += (list.empty() ? L"" : L"|") + file;
        WriteCoreOption(game, "eden_conteudo", Utf8(list));
        LoadContent(game);
    }

    // Adds an update or add-on file to the Switch game on screen; says why when the file is not one of this game's.
    void AddContentFile(const std::wstring& file) {
        Game* game = Current();
        if (game == nullptr) return;
        const SwitchContent content = DetectSwitchContent(file), base = DetectSwitchContent(game->folder);
        if (content.kind == SwitchKind::Game) {
            Notify(L"Este arquivo é um jogo inteiro, não uma atualização nem DLC:\n\n" + FileName(file));
            return;
        }
        if (!content.baseId.empty() && base.kind == SwitchKind::Game && content.baseId != base.titleId) {
            Notify(L"Este arquivo é de outro jogo (" + content.baseId + L"), não de " + game->title + L" (" + base.titleId + L").");
            return;
        }
        for (const auto& have : contentFiles) if (_wcsicmp(have.c_str(), file.c_str()) == 0) return;
        contentFiles.push_back(file);
        SaveContent(*game);
    }

    // The console's folder of games (jogos\<console>), opened in Explorer for the user to drop games in.
    void OpenGamesFolder() {
        const std::wstring folder = library.GamesFolder(console);
        CreateDirectoryW((library.Root() + L"\\jogos").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
        if (game.system == L"PS3" && !IsDirectory(game.folder)) {
            // A PS3 ISO's own art, taken out of the disc the first time it is asked for (games added before this).
            static std::set<std::wstring> taken;
            const std::wstring wanted = art + (icon ? L"\\ICON0.PNG" : L"\\PIC1.PNG");
            if (!Exists(wanted) && taken.insert(game.titleId).second) ExtractArt(game.system, game.folder, art);
            files.push_back(wanted);
        }
        if (game.system == L"PSP") files.push_back(art + (icon ? L"\\ICON0.PNG" : L"\\PIC1.PNG"));
        // A Wii U game in a folder carries its menu icon and its TV boot picture in meta (TGA, which the loader reads).
        if (game.system == L"WIIU" && IsDirectory(game.folder)) files.push_back(game.folder + (icon ? L"\\meta\\iconTex.tga" : L"\\meta\\bootTvTex.tga"));
        // The Switch core keeps the game's own icon here the first time the game runs: no box art server has the Switch.
        if (game.system == L"SWITCH" && icon) files.push_back(art + L"\\icon.jpg");
        files.push_back(BoxartFile(game));
        for (const auto& file : files) if (Exists(file)) return file;
        return L"";
    }

    const Texture& Icon(const Game& game) {
        auto found = icons.find(game.titleId);
        if (found == icons.end()) found = icons.emplace(game.titleId, LoadTexture(ArtFile(game, true), 512)).first;
        if (found->second.view == nullptr && game.system == L"SWITCH") {
            // The icon arrives while the game runs, not from the cover fetcher: looked for again every two seconds.
            ULONGLONG& checked = switchArtChecked[game.titleId];
            if (const ULONGLONG now = GetTickCount64(); now - checked > 2000) {
                checked = now;
                if (const std::wstring file = ArtFile(game, true); !file.empty()) found->second = LoadTexture(file, 512);
            }
        }
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
        const wchar_t* title = ShowPs4Ps5(library.Root()) ? L"Escolha a pasta do jogo (PS5, Bloodborne, PS3, Xbox 360 ou Wii U)" : L"Escolha a pasta do jogo (PS3, Xbox 360 ou Wii U)";
        Browse(title, library.Root(), true, {}, [this](const std::wstring& folder) {
            std::wstring error;
            const int index = library.Add(folder, error);
            if (index < 0) Notify(error);
            else OpenGame(index);
        });
    }

    void AddGameFile() {
        Browse(L"Escolha o arquivo do jogo (ISO, ROM, NSP...)", L"", false, GameFileFilters(), [this](const std::wstring& path) { AddPickedFile(path); });
    }

    void AddPickedFile(const std::wstring& path) {
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
        Browse(L"Escolha a pasta com os jogos (ROMs, ISOs, CHDs...)", L"", true, {}, [this](const std::wstring& folder) { AddPickedRomFolder(folder); });
    }

    void AddPickedRomFolder(const std::wstring& folder) {
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

    // Ranga's own way to choose a folder or a file, instead of the Windows dialog, in its colors and on the gamepad:
    // the d-pad goes along the list, A opens a folder or picks a file, B goes up a folder and, from the drives,
    // cancels. done gets the path chosen.
    struct Browser {
        std::wstring title;
        // Empty: the drives and the usual places.
        std::wstring folder;
        bool pickFolder = false;
        // Lower-case, with their dot; empty takes every file.
        std::vector<std::wstring> extensions;
        std::function<void(const std::wstring&)> done;
        struct Entry {
            std::wstring name, path;
            bool folder = true;
        };
        std::vector<Entry> entries;
        bool listed = false, focusFirst = false;
        // The folder just left going up, which gets the focus in its parent.
        std::wstring cameFrom;
    };
    std::optional<Browser> browser;

    void Browse(const std::wstring& title, const std::wstring& start, bool pickFolder,
                const std::vector<std::pair<std::wstring, std::wstring>>& filters, std::function<void(const std::wstring&)> done) {
        Browser b;
        b.title = title;
        b.pickFolder = pickFolder;
        b.done = std::move(done);
        // The first filter is every kind of file the caller takes ("*.iso;*.chd;..."); "*.*" takes every file.
        if (!filters.empty()) {
            std::wstring patterns = filters.front().second;
            for (size_t at = 0; at <= patterns.size();) {
                size_t end = patterns.find(L';', at);
                if (end == std::wstring::npos) end = patterns.size();
                const std::wstring pattern = Lower(patterns.substr(at, end - at));
                if (pattern.rfind(L"*.", 0) == 0 && pattern != L"*.*") b.extensions.push_back(pattern.substr(1));
                at = end + 1;
            }
        }
        std::wstring folder = start;
        if (!folder.empty() && !IsDirectory(folder)) folder = ParentFolder(folder);
        b.folder = IsDirectory(folder) ? folder : L"";
        browser = std::move(b);
    }

    static std::wstring JoinPath(const std::wstring& folder, const std::wstring& name) {
        return !folder.empty() && folder.back() == L'\\' ? folder + name : folder + L"\\" + name;
    }

    // The folder above; empty above a drive's root, where the drives are.
    static std::wstring ParentOf(const std::wstring& folder) {
        if (folder.size() <= 3) return L"";
        std::wstring parent = folder.substr(0, folder.find_last_of(L'\\', folder.size() - 2));
        if (parent.size() == 2 && parent[1] == L':') parent += L'\\';
        return parent;
    }

    void ListBrowser() {
        Browser& b = *browser;
        b.entries.clear();
        b.listed = true;
        b.focusFirst = true;
        if (b.folder.empty()) {
            const std::pair<const KNOWNFOLDERID*, const wchar_t*> places[] = {
                {&FOLDERID_Downloads, L"Downloads"}, {&FOLDERID_Desktop, L"Área de trabalho"}, {&FOLDERID_Documents, L"Documentos"}};
            for (const auto& [id, name] : places) {
                PWSTR path = nullptr;
                if (SUCCEEDED(SHGetKnownFolderPath(*id, 0, nullptr, &path)) && IsDirectory(path)) b.entries.push_back({name, path, true});
                CoTaskMemFree(path);
            }
            b.entries.push_back({L"Pasta do Ranga", library.Root(), true});
            wchar_t drives[256] = {};
            GetLogicalDriveStringsW(255, drives);
            for (const wchar_t* drive = drives; *drive != 0; drive += wcslen(drive) + 1) {
                std::wstring name = L"Disco " + std::wstring(drive, 2);
                // A disc drive with nothing in it would make the volume name wait.
                wchar_t label[MAX_PATH] = {};
                if (GetDriveTypeW(drive) != DRIVE_CDROM && GetVolumeInformationW(drive, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0) && label[0] != 0)
                    name = std::wstring(label) + L" (" + std::wstring(drive, 2) + L")";
                b.entries.push_back({name, drive, true});
            }
            return;
        }
        std::vector<Browser::Entry> folders, files;
        WIN32_FIND_DATAW data;
        const HANDLE find = FindFirstFileExW(JoinPath(b.folder, L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (find != INVALID_HANDLE_VALUE) {
            do {
                const std::wstring name = data.cFileName;
                if (name == L"." || name == L".." || (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) continue;
                if (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                    folders.push_back({name, JoinPath(b.folder, name), true});
                } else if (!b.pickFolder) {
                    const std::wstring ext = Extension(name);
                    if (b.extensions.empty() || std::find(b.extensions.begin(), b.extensions.end(), ext) != b.extensions.end())
                        files.push_back({name, JoinPath(b.folder, name), false});
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
        const auto byName = [](const Browser::Entry& a, const Browser::Entry& c) { return StrCmpLogicalW(a.name.c_str(), c.name.c_str()) < 0; };
        std::sort(folders.begin(), folders.end(), byName);
        std::sort(files.begin(), files.end(), byName);
        b.entries = std::move(folders);
        b.entries.insert(b.entries.end(), files.begin(), files.end());
    }

    void BrowserWindow(float scale) {
        if (!browser) return;
        if (!browser->listed) ListBrowser();
        if (!ImGui::IsPopupOpen("##browse")) ImGui::OpenPopup("##browse");
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.84f, viewport->WorkSize.y * 0.86f), ImGuiCond_Always);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * scale, 22 * scale));
        if (ImGui::BeginPopupModal("##browse", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings)) {
            Browser& b = *browser;
            std::optional<std::wstring> go, chosen;
            bool cancel = false;
            ImGui::PushFont(semibold, 24);
            ImGui::TextUnformatted(Utf8(b.title).c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s", b.folder.empty() ? "Este PC" : Utf8(b.folder).c_str());
            ImGui::Dummy(ImVec2(0, 4 * scale));
            if (b.pickFolder && !b.folder.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
                ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(kAccent));
                if (ImGui::Button("Escolher esta pasta", ImVec2(200 * scale, 36 * scale))) chosen = b.folder;
                ImGui::PopStyleColor(3);
                ImGui::SameLine(0, 10 * scale);
            }
            if (!b.folder.empty()) {
                if (ImGui::Button("Voltar uma pasta", ImVec2(170 * scale, 36 * scale))) go = ParentOf(b.folder);
                ImGui::SameLine(0, 10 * scale);
            }
            if (ImGui::Button("Cancelar", ImVec2(130 * scale, 36 * scale))) cancel = true;
            ImGui::SameLine(0, 20 * scale);
            ImGui::AlignTextToFramePadding();
            ImGui::TextDisabled(b.pickFolder ? "A: abrir a pasta    B: voltar uma pasta" : "A: abrir a pasta ou escolher o arquivo    B: voltar uma pasta");
            ImGui::Dummy(ImVec2(0, 6 * scale));
            ImGui::BeginChild("##entries", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
            if (b.entries.empty()) ImGui::TextDisabled(b.pickFolder ? "Nenhuma pasta aqui." : "Nenhuma pasta nem arquivo que sirva aqui.");
            int focus = 0;
            for (size_t i = 0; i < b.entries.size(); ++i) {
                if (!b.cameFrom.empty() && b.entries[i].path == b.cameFrom) focus = static_cast<int>(i);
            }
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float row = 40 * scale;
            for (size_t i = 0; i < b.entries.size(); ++i) {
                const auto& entry = b.entries[i];
                ImGui::PushID(static_cast<int>(i));
                if (b.focusFirst && static_cast<int>(i) == focus) {
                    ImGui::SetKeyboardFocusHere();
                    ShowRing();
                }
                if (ImGui::Selectable("##entry", false, ImGuiSelectableFlags_None, ImVec2(0, row))) {
                    if (entry.folder) go = entry.path;
                    else chosen = entry.path;
                }
                const ImVec2 min = ImGui::GetItemRectMin();
                // A folder is a tab over a box in the accent color; a file, a dim page.
                const float x = min.x + 10 * scale, y = min.y + row / 2;
                if (entry.folder) {
                    draw->AddRectFilled(ImVec2(x, y - 9 * scale), ImVec2(x + 10 * scale, y - 5 * scale), Packed(kAccent), 2 * scale);
                    draw->AddRectFilled(ImVec2(x, y - 6 * scale), ImVec2(x + 22 * scale, y + 9 * scale), Packed(kAccent), 3 * scale);
                } else {
                    draw->AddRect(ImVec2(x + 3 * scale, y - 10 * scale), ImVec2(x + 19 * scale, y + 10 * scale), Packed(kDim), 2 * scale, 0, 1.5f * scale);
                }
                draw->AddText(ImVec2(x + 36 * scale, y - ImGui::GetFontSize() / 2), Packed(kText), Utf8(entry.name).c_str());
                ImGui::PopID();
            }
            b.focusFirst = false;
            ImGui::EndChild();
            if (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                ImGui::IsKeyPressed(ImGuiKey_Backspace, false)) {
                if (b.folder.empty()) cancel = true;
                else go = ParentOf(b.folder);
            }
            if (chosen || cancel) {
                const auto done = b.done;
                const std::wstring path = chosen ? *chosen : L"";
                browser.reset();
                ImGui::CloseCurrentPopup();
                if (chosen) pending = [done, path] { done(path); };
            } else if (go) {
                // Going up, the folder left keeps the focus.
                b.cameFrom = go->size() < b.folder.size() ? b.folder : L"";
                b.folder = *go;
                b.listed = false;
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
        bool left = false, right = false, up = false, down = false, accept = false, back = false, padAccept = false, search = false, menu = false, view = false;
    };

    Nav ReadNav() {
        Nav nav;
        // RANGA_CLIENT_TEST_INPUT=1: keys posted to a window in the back count too (the interface tests).
        static const bool testInput = _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr;
        const bool front = testInput || GetForegroundWindow() == window;
        // While the Controle tab waits for a button, the press is the new button, not a move.
        const bool free = front && captureFor < 0 && !captureCooling && !ImGui::GetIO().WantTextInput && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                          !ImGui::IsAnyItemActive();
        gamepads.Update(free);
        if (!free) return nav;
        nav.left = ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || gamepads.Pressed(Gamepads::Left);
        nav.right = ImGui::IsKeyPressed(ImGuiKey_RightArrow) || gamepads.Pressed(Gamepads::Right);
        nav.up = ImGui::IsKeyPressed(ImGuiKey_UpArrow) || gamepads.Pressed(Gamepads::Up);
        nav.down = ImGui::IsKeyPressed(ImGuiKey_DownArrow) || gamepads.Pressed(Gamepads::Down);
        nav.padAccept = gamepads.Pressed(Gamepads::Accept);
        nav.accept = ImGui::IsKeyPressed(ImGuiKey_Enter, false) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false) || nav.padAccept;
        nav.back = ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) || gamepads.Pressed(Gamepads::Back);
        nav.search = gamepads.Pressed(Gamepads::Search);
        nav.menu = gamepads.Pressed(Gamepads::Menu) || ImGui::IsKeyPressed(ImGuiKey_F11, false);
        nav.view = gamepads.Pressed(Gamepads::View);
        return nav;
    }

    // The consoles on the home row: PS5 first, the ones with games before the ones without.
    struct Shelf {
        std::wstring id;
        int count = 0;
    };

    std::vector<Shelf> Shelves() {
        std::vector<Shelf> shelves;
        if (ShowPs4Ps5(library.Root())) {
            shelves.push_back({L"PS5"});
            for (const auto& game : library.Games()) {
                if (game.system == L"PS4") { shelves.push_back({L"PS4"}); break; }
            }
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
        librarySearch.fill(0); // each console's page opens with every game
        keyboardOpen = false;
        // What was dropped in the console's folder since the last look comes in now.
        const std::wstring keep = Current() != nullptr ? Current()->titleId : L"";
        library.ScanGamesFolders(console);
        Reload(keep);
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
        ImGui::SetCursorPos(ImVec2(right - wide - 5 * narrow - 50 * scale, 22 * scale));
        if (ImGui::Button("+  Adicionar jogo", ImVec2(wide, height)) || barPress == first) ImGui::OpenPopup("##add");
        FocusLast(barFocus == first, scale);
        AddMenu(scale);
        ImGui::SameLine(0, 10 * scale);
        if (ImGui::Button("Emuladores", ImVec2(narrow, height)) || barPress == first + 1) {
            enginesPage = true;
            profileOpen = false;
            newsOpen = false;
            storeOpen = false;
            RefreshEngines();
        }
        FocusLast(barFocus == first + 1, scale);
        ImGui::SameLine(0, 10 * scale);
        // Novidades, with how many versions came since it was last opened.
        RefreshNews(false);
        const int unseen = UnseenNews();
        // Once per opening, a notice with the newest version's notes when there is news.
        if (unseen > 0 && !newsToastShown) {
            newsToastShown = true;
            std::string notes;
            {
                std::lock_guard guard(newsLock);
                if (!news.empty()) notes = news.front().notes;
            }
            if (notes.size() > 160) notes = notes.substr(0, 157) + "...";
            AddToast("Novidades do Ranga", notes, "");
        }
        const std::string newsLabel = unseen > 0 ? "Novidades (" + std::to_string(unseen) + ")###novidades" : "Novidades###novidades";
        if (unseen > 0) ImGui::PushStyleColor(ImGuiCol_Text, Color(kAccentHover));
        if (ImGui::Button(newsLabel.c_str(), ImVec2(narrow, height)) || barPress == first + 2) OpenNews();
        if (unseen > 0) ImGui::PopStyleColor();
        FocusLast(barFocus == first + 2, scale);
        ImGui::SameLine(0, 10 * scale);
        const std::string profileLabel = chatUnread > 0 ? "Perfil (" + std::to_string(chatUnread) + ")###perfil" : "Perfil###perfil";
        if (ImGui::Button(profileLabel.c_str(), ImVec2(narrow, height)) || barPress == first + 3) OpenProfile();
        FocusLast(barFocus == first + 3, scale);
        ImGui::SameLine(0, 10 * scale);
        // Amigos: the list at the right, with how many are online.
        const int online = profile.session.empty() ? 0 : FriendsOnline();
        const std::string friendsLabel = online > 0 ? "Amigos (" + std::to_string(online) + ")###amigos" : "Amigos###amigos";
        if (online > 0) ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
        if (ImGui::Button(friendsLabel.c_str(), ImVec2(narrow, height)) || barPress == first + 4) friendsPanel = !friendsPanel;
        if (online > 0) ImGui::PopStyleColor();
        FocusLast(barFocus == first + 4, scale);
        ImGui::SameLine(0, 10 * scale);
        // Apps: the store of the owner's programs (Ranga TV and the next ones).
        if (ImGui::Button("Apps", ImVec2(narrow, height)) || barPress == first + 5) pending = [this] { OpenStore(); };
        FocusLast(barFocus == first + 5, scale);
    }

    // The art of a game in a box: its own square art fills it; box art of another shape sits whole on the console's
    // colors; no art at all is the console's colors with the title.
    void GameArt(ImDrawList* draw, const Game& game, ImVec2 min, ImVec2 max, float scale) {
        const Look& look = LookFor(game.system);
        const float rounding = 10 * scale, width = max.x - min.x, height = max.y - min.y;
        const Texture& icon = Icon(game);
        if (icon.view == nullptr) {
            // Too small for the title (Continuar jogando): only the console's outline, in its color.
            if (width < 90 * scale) {
                GradientRect(draw, min, max, look.top, look.bottom, rounding);
                DrawGlyph(draw, look.glyph, ImVec2(min.x + width * 0.5f, min.y + height * 0.5f), width * 0.62f, Packed(0xffffff, 0.85f), Packed(look.bottom));
                return;
            }
            draw->AddRectFilled(min, max, Packed(kSurface), rounding);
            DrawGlyph(draw, look.glyph, ImVec2(min.x + width * 0.5f, min.y + height * 0.36f), width * 0.42f, Packed(0xffffff, 0.14f), Packed(kSurface, 0.0f));
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
        draw->AddRectFilled(min, max, Packed(kSurface), rounding);
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
        draw->AddRect(ImVec2(min.x - grow, min.y - grow), ImVec2(max.x + grow, max.y + grow), Packed(0xffffff), rounding + grow, 0, 2 * scale);
    }

    // The games last played, newest first, as indices into the library (at most count; the console's own menu and
    // games no longer in the library left out).
    std::vector<int> RecentGames(size_t count) {
        std::vector<std::pair<int64_t, int>> played;
        for (size_t i = 0; i < library.Games().size(); ++i) {
            const auto found = profile.games.find(library.Games()[i].titleId);
            if (found != profile.games.end() && found->second.last > 0) played.emplace_back(found->second.last, static_cast<int>(i));
        }
        std::sort(played.begin(), played.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::vector<int> result;
        for (const auto& [when, index] : played) {
            if (result.size() == count) break;
            result.push_back(index);
        }
        return result;
    }

    static float RecentPanelWidth(float scale) { return 300 * scale; }

    // "agora há pouco", "há 3 h", "ontem", "há 5 dias": when a game was last played.
    static std::string PlayedWhen(int64_t last) {
        const int64_t ago = static_cast<int64_t>(std::time(nullptr)) - last;
        if (ago < 3600) return "agora há pouco";
        if (ago < 86400) return "há " + std::to_string(ago / 3600) + " h";
        if (ago < 2 * 86400) return "ontem";
        return "há " + std::to_string(ago / 86400) + " dias";
    }

    // Continuar jogando: the last games played, of any console, one card each with its art, name, console and when.
    void RecentPanel(ImDrawList* draw, const std::vector<int>& recent, ImVec2 at, float scale) {
        const float width = RecentPanelWidth(scale), card = 72 * scale, gap = 10 * scale, art = 56 * scale;
        draw->AddText(semibold, 18 * scale, at, Packed(kText), "Continuar jogando");
        float y = at.y + 32 * scale;
        for (size_t k = 0; k < recent.size(); ++k) {
            const Game& game = library.Games()[static_cast<size_t>(recent[k])];
            const ImVec2 min(at.x, y), max(at.x + width, y + card);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(static_cast<int>(k) + 100000);
            if (ImGui::InvisibleButton("##continue", ImVec2(width, card))) pending = [this, index = recent[k]] { OpenGame(index); };
            const bool lit = (homeRow == 3 && recentFocus == static_cast<int>(k)) || ImGui::IsItemHovered();
            ImGui::PopID();
            draw->AddRectFilled(min, max, Packed(lit ? kSelected : kSurface, 0.92f), 12 * scale);
            if (lit) Focus(draw, min, max, scale * 0.6f, 12 * scale);
            const ImVec2 artMin(min.x + 8 * scale, min.y + (card - art) / 2);
            GameArt(draw, game, artMin, ImVec2(artMin.x + art, artMin.y + art), scale * 0.7f);
            const float textX = artMin.x + art + 12 * scale;
            const ImVec4 clip(textX, min.y, max.x - 10 * scale, max.y);
            const std::string title = Utf8(game.title);
            draw->AddText(semibold, 15 * scale, ImVec2(textX, min.y + 15 * scale), Packed(kText), title.c_str(), nullptr, 0, &clip);
            std::string line = SystemName(game.system);
            if (const auto found = profile.games.find(game.titleId); found != profile.games.end()) line += "  ·  " + PlayedWhen(found->second.last);
            if (const std::string played = PlayedTime(game); !played.empty()) line += "  ·  " + played;
            draw->AddText(nullptr, 13 * scale, ImVec2(textX, min.y + 39 * scale), Packed(LookFor(game.system).name), line.c_str(), nullptr, 0, &clip);
            y += card + gap;
        }
    }

    // The game's art (index into the library; -1 none) over the top right of the home page, faded into the
    // background toward the left and the bottom so the text over it stays readable.
    void Backdrop(ImDrawList* draw, int index, ImVec2 origin, ImVec2 size) {
        if (index < 0 || index >= static_cast<int>(library.Games().size())) return;
        const Texture& art = Icon(library.Games()[static_cast<size_t>(index)]);
        if (art.view == nullptr || art.width <= 0 || art.height <= 0) return;
        const ImVec2 min(origin.x + size.x * 0.32f, origin.y), max(origin.x + size.x, origin.y + size.y * 0.8f);
        const float boxAspect = (max.x - min.x) / (max.y - min.y), imageAspect = static_cast<float>(art.width) / static_cast<float>(art.height);
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (imageAspect > boxAspect) {
            const float visible = boxAspect / imageAspect;
            uv0.x = (1 - visible) / 2, uv1.x = uv0.x + visible;
        } else {
            const float visible = imageAspect / boxAspect;
            uv0.y = (1 - visible) * 0.3f, uv1.y = uv0.y + visible;
        }
        draw->AddImage(art.Ref(), min, max, uv0, uv1, Packed(0xffffff, 0.2f));
        const ImU32 solid = Packed(kBackground), clear = Packed(kBackground, 0.25f);
        draw->AddRectFilledMultiColor(min, ImVec2(min.x + (max.x - min.x) * 0.6f, max.y), solid, clear, clear, solid);
        const float fade = (max.y - min.y) * 0.45f;
        draw->AddRectFilledMultiColor(ImVec2(min.x, max.y - fade), max, Packed(kBackground, 0.0f), Packed(kBackground, 0.0f), solid, solid);
    }

    void Home(float scale, const Nav& input) {
        // On the top row (Adicionar jogo, Emuladores) the gamepad moves nothing else; up from the consoles reaches it.
        const bool inBar = TopBar(input, 6);
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
        const auto games = ConsoleGames(console);
        const auto recent = RecentGames(4);
        const bool hasRecent = !recent.empty();
        // The column at the right: Continuar jogando and, under it, the friends' activity; either one keeps it.
        const bool sideColumn = hasRecent || (!profile.session.empty() && !FriendActivity(1).empty());
        // Down goes from the consoles to the buttons under them and on to the games; up and back return. Right past
        // the last button or game reaches Continuar jogando, at the right; there up and down go along, left comes back.
        bool openLibrary = false, openSystem = false;
        int openGame = -1, openRecent = -1;
        if (homeRow == 0) {
            if (nav.down) homeRow = 1, homeColumn = 0;
            else if (nav.accept) openLibrary = true;
        } else if (homeRow == 1) {
            const int buttons = systemButton ? 2 : 1;
            if (nav.left && homeColumn > 0) --homeColumn;
            if (nav.right && homeColumn + 1 >= buttons && hasRecent) homeRow = 3, recentFocus = 0;
            else if (nav.right && homeColumn + 1 < buttons) ++homeColumn;
            homeColumn = std::min(homeColumn, buttons - 1);
            if (homeRow != 1) {
            } else if (nav.up || nav.back) homeRow = 0;
            else if (nav.down && homeGamesShown > 0) homeRow = 2, homeColumn = 0;
            else if (nav.accept) (homeColumn == 0 ? openLibrary : openSystem) = true;
        } else if (homeRow == 3) {
            recentFocus = std::clamp(recentFocus, 0, std::max(0, static_cast<int>(recent.size()) - 1));
            if (!hasRecent || nav.left) homeRow = 1, homeColumn = 0;
            else if (nav.up && recentFocus > 0) --recentFocus;
            else if (nav.down && recentFocus + 1 < static_cast<int>(recent.size())) ++recentFocus;
            else if (nav.back) homeRow = 0;
            else if (nav.accept) openRecent = recentFocus;
        } else if (homeGamesShown == 0) {
            homeRow = 1, homeColumn = 0;
        } else {
            if (nav.left && homeColumn > 0) --homeColumn;
            if (nav.right && homeColumn + 1 >= homeGamesShown && hasRecent) homeRow = 3, recentFocus = 0;
            else if (nav.right && homeColumn + 1 < homeGamesShown) ++homeColumn;
            homeColumn = std::min(homeColumn, homeGamesShown - 1);
            if (homeRow != 2) {
            } else if (nav.up) homeRow = 1, homeColumn = 0;
            else if (nav.back) homeRow = 0;
            else if (nav.accept) openGame = homeColumn;
        }
        if (openLibrary) pending = [this] { OpenConsole(); };
        if (openSystem && !runner.Busy()) pending = [this] { runner.StartSystem(library.Root(), console); };
        if (openRecent >= 0) pending = [this, index = recent[static_cast<size_t>(openRecent)]] { OpenGame(index); };

        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();

        // Behind everything, the art of the game in focus (else the console's first game) spread over the right and
        // faded into the background, as a console's home does.
        {
            int backdrop = -1;
            if (homeRow == 2 && homeColumn < static_cast<int>(games.size())) backdrop = games[static_cast<size_t>(homeColumn)];
            else if (homeRow == 3 && recentFocus < static_cast<int>(recent.size())) backdrop = recent[static_cast<size_t>(recentFocus)];
            else if (!games.empty()) backdrop = games.front();
            Backdrop(draw, backdrop, origin, size);
        }

        const float left = 40 * scale;
        ImGui::SetCursorPos(ImVec2(left, 24 * scale));
        ImGui::PushFont(semibold, 24);
        ImGui::TextUnformatted("Ranga Emu");
        ImGui::PopFont();
        // Which Ranga this is: the update it runs ("2026.11.21-0025", the same name as in Novidades and on GitHub).
        // The number (1.0.0); a version from before the numbers shows its date.
        if (const std::string version = Utf8(CachedIni(L"Ranga", L"versao_atualizacao", L"")); !version.empty()) {
            const std::string number = Utf8(CachedIni(L"Ranga", L"versao_nome", L""));
            ImGui::SameLine(0, 10 * scale);
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 7 * scale);
            ImGui::TextDisabled("versão %s", number.empty() ? version.c_str() : number.c_str());
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Publicada em %s", NewsDate(version).c_str());
        }
        TopButtons(scale, size.x - left, 0);

        // The row of consoles slides so the chosen one stays in view.
        const float tile = 124 * scale, big = 160 * scale, gap = 20 * scale, rowY = origin.y + 92 * scale;
        const float view = size.x - 2 * left - big;
        const float at = focus * (tile + gap);
        if (at - homeTarget < 0) homeTarget = at;
        if (at - homeTarget > view) homeTarget = at - view;
        homeScroll += (homeTarget - homeScroll) * std::min(1.0f, ImGui::GetIO().DeltaTime * 14);
        if (std::fabs(homeTarget - homeScroll) < 0.5f) homeScroll = homeTarget;
        // How grown each card is (the chosen one toward the big size) and where each starts, once a frame.
        std::vector<float> grows(shelves.size()), offsets(shelves.size());
        float run = 0;
        for (size_t i = 0; i < shelves.size(); ++i) {
            grows[i] = Smooth("console:" + Utf8(shelves[i].id), static_cast<int>(i) == focus ? 1.0f : 0.0f);
            offsets[i] = run;
            run += tile + (big - tile) * grows[i] + gap;
        }
        for (size_t i = 0; i < shelves.size(); ++i) {
            const bool chosen = static_cast<int>(i) == focus;
            const float x = origin.x + left - homeScroll + offsets[i];
            const float side = tile + (big - tile) * grows[i];
            if (x > origin.x + size.x || x + side < origin.x) continue;
            const ImVec2 min(x, rowY), max(x + side, rowY + side);
            ConsoleCard(draw, semibold, LookFor(shelves[i].id), min, max, scale, chosen || shelves[i].count > 0 ? 1.0f : 0.45f, chosen);
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
        const Look& look = LookFor(console);
        ImGui::PushFont(semibold, 40);
        ImGui::PushStyleColor(ImGuiCol_Text, Color(look.name));
        ImGui::TextUnformatted(SystemName(console).c_str());
        ImGui::PopStyleColor();
        ImGui::PopFont();
        const std::string count = shelf.count == 0 ? "Nenhum jogo ainda" : shelf.count == 1 ? "1 jogo" : std::to_string(shelf.count) + " jogos";
        std::string made = look.maker;
        if (look.year > 0) made += (made.empty() ? "" : " · ") + std::to_string(look.year);
        if (made.empty()) ImGui::TextDisabled("%s   ·   %s", count.c_str(), HowItRuns(console).c_str());
        else ImGui::TextDisabled("%s   ·   %s   ·   %s", made.c_str(), count.c_str(), HowItRuns(console).c_str());
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
            ImGui::PushTextWrapPos(size.x - left - (sideColumn ? RecentPanelWidth(scale) + 30 * scale : 0));
            ImGui::TextWrapped("%s", Utf8(missing).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        ImGui::EndGroup();

        // A few of its games, straight to their page, each with its name under it.
        const float rowRight = origin.x + size.x - left - (sideColumn ? RecentPanelWidth(scale) + 30 * scale : 0);
        y = ImGui::GetItemRectMax().y + 28 * scale;
        const float cover = 140 * scale, coverGap = 18 * scale, caption = 26 * scale;
        homeGamesShown = 0;
        if (!games.empty() && y + cover + caption < origin.y + size.y - 30 * scale) {
            for (size_t k = 0; k < games.size(); ++k) {
                const float x = origin.x + left + k * (cover + coverGap);
                if (x + cover > rowRight) break;
                ++homeGamesShown;
                const ImVec2 min(x, y), max(x + cover, y + cover);
                const bool focused = homeRow == 2 && homeColumn == static_cast<int>(k);
                GameArt(draw, library.Games()[static_cast<size_t>(games[k])], min, max, scale);
                ImGui::SetCursorScreenPos(min);
                ImGui::PushID(games[k]);
                if (ImGui::InvisibleButton("##recent", ImVec2(cover, cover + caption)) || openGame == static_cast<int>(k)) pending = [this, index = games[k]] { OpenGame(index); };
                const bool lit = focused || ImGui::IsItemHovered();
                if (lit) Focus(draw, min, max, scale * 0.6f, 10 * scale);
                const std::string title = Utf8(library.Games()[static_cast<size_t>(games[k])].title);
                const ImVec4 clip(min.x, max.y, max.x, max.y + caption);
                draw->AddText(lit ? semibold : nullptr, ImGui::GetFontSize() * 0.95f, ImVec2(min.x + 2 * scale, max.y + 8 * scale), Packed(lit ? kText : kDim), title.c_str(), nullptr, 0, &clip);
                ImGui::PopID();
            }
        }

        if (hasRecent) RecentPanel(draw, recent, ImVec2(origin.x + size.x - left - RecentPanelWidth(scale), rowY + big + 30 * scale), scale);
        // Under it, what the friends played and unlocked lately.
        if (sideColumn) {
            const float panelX = origin.x + size.x - left - RecentPanelWidth(scale);
            const float panelY = rowY + big + 30 * scale + (hasRecent ? 32 * scale + recent.size() * 82 * scale + 18 * scale : 0.0f);
            ActivityPanel(draw, ImVec2(panelX, panelY), RecentPanelWidth(scale), origin.y + size.y - 40 * scale, scale);
        }
        const char* hint = "Setas ou direcional: escolher    Enter, X ou A: abrir    Esc, O ou B: voltar";
        if (gamepads.Connected()) draw->AddText(nullptr, ImGui::GetFontSize() * 0.9f, ImVec2(origin.x + left, origin.y + size.y - 30 * scale), Packed(kDim), hint);
    }

    std::array<char, 64> librarySearch{}; // what was typed in the console page's search

    // "Testar": asks the room at the address whether it answers (ProbeRoom, in the background) and shows how long it
    // took, beside the address field; id tells the fields apart.
    std::mutex probeLock;
    std::map<std::string, std::string> probeResults; // id -> what to show ("" while testing)

    // "Quem está na sala": the classics' server lists who plays what at its tables (GET /ranga/mesas, a server since
    // 1.0.1; an older one answers 404). Asked in the background; the answer shows under the address.
    std::string tablesShown;
    std::atomic<bool> tablesBusy{false};

    void TablesButton(const std::string& address, float labelWidth, float scale) {
        ImGui::SetCursorPosX(labelWidth);
        ImGui::BeginDisabled(tablesBusy || address.empty());
        if (ImGui::Button(tablesBusy ? "Perguntando...###mesas" : "Quem está na sala###mesas")) {
            tablesBusy = true;
            std::thread([this, address] {
                const HttpResult answer = HttpRequest("GET", "http://" + (address.find(':') == std::string::npos ? address + ":24873" : address) + "/ranga/mesas", "", "", "RangaEmu", 5000);
                std::string text;
                if (answer.status == 200) {
                    size_t start = 0;
                    while (start < answer.body.size()) {
                        size_t end = answer.body.find('\n', start);
                        if (end == std::string::npos) end = answer.body.size();
                        const std::string line = answer.body.substr(start, end - start);
                        start = end + 1;
                        if (const size_t tab = line.find('\t'); tab != std::string::npos) text += line.substr(tab + 1) + " jogando " + line.substr(0, tab) + "\n";
                    }
                    if (text.empty()) text = "Ninguém na sala agora.";
                } else {
                    text = answer.status == 404 ? "Este servidor ainda não sabe listar (precisa da versão 1.0.1)." : "A sala não respondeu.";
                }
                std::lock_guard guard(probeLock);
                tablesShown = text;
                tablesBusy = false;
            }).detach();
        }
        ImGui::EndDisabled();
        std::string shown;
        {
            std::lock_guard guard(probeLock);
            shown = tablesShown;
        }
        if (!shown.empty()) {
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("%s", shown.c_str());
            ImGui::PopTextWrapPos();
        }
    }

    void ProbeButton(const char* id, const std::string& address, int defaultPort, bool tcp, float scale) {
        ImGui::SameLine(0, 8 * scale);
        std::string shown;
        bool testing = false;
        {
            std::lock_guard guard(probeLock);
            if (const auto found = probeResults.find(id); found != probeResults.end()) {
                shown = found->second;
                testing = shown.empty();
            }
        }
        ImGui::BeginDisabled(testing || address.empty());
        if (ImGui::Button((std::string(testing ? "Testando..." : "Testar") + "###probe" + id).c_str())) {
            {
                std::lock_guard guard(probeLock);
                probeResults[id].clear();
            }
            std::thread([this, key = std::string(id), address, defaultPort, tcp] {
                const int ms = ProbeRoom(address, defaultPort, tcp);
                std::lock_guard guard(probeLock);
                probeResults[key] = ms >= 0 ? "respondeu em " + std::to_string(ms) + " ms" : "não respondeu (endereço, porta, túnel ou servidor desligado)";
            }).detach();
        }
        ImGui::EndDisabled();
        if (!shown.empty()) {
            ImGui::SameLine(0, 8 * scale);
            ImGui::AlignTextToFramePadding();
            ImGui::PushStyleColor(ImGuiCol_Text, Color(shown.rfind("respondeu em", 0) == 0 ? kGreen : kAmber));
            ImGui::TextUnformatted(shown.c_str());
            ImGui::PopStyleColor();
        }
    }

    // Favorites: engines.ini [Favoritos] <titleId>=1. They come first on their console's page, with a star.
    bool IsFavorite(const Game& game) const { return CachedIni(L"Favoritos", game.titleId.c_str(), L"") == L"1"; }
    void SetFavorite(const Game& game, bool on) {
        const std::wstring ini = library.Root() + L"\\engines.ini";
        EnsureUnicodeIni(ini);
        WritePrivateProfileStringW(L"Favoritos", game.titleId.c_str(), on ? L"1" : nullptr, ini.c_str());
    }

    // "3 h 20 min", "45 min" or empty: how long the game was played, from the profile.
    std::string PlayedTime(const Game& game) const {
        const auto found = profile.games.find(game.titleId);
        if (found == profile.games.end() || found->second.seconds < 60) return "";
        const int64_t minutes = found->second.seconds / 60;
        if (minutes < 60) return std::to_string(minutes) + " min";
        return std::to_string(minutes / 60) + " h" + (minutes % 60 ? " " + std::to_string(minutes % 60) + " min" : "");
    }

    // A small five-pointed star, filled, centered on c.
    static void DrawStar(ImDrawList* draw, ImVec2 c, float radius, ImU32 color) {
        ImVec2 points[10];
        for (int i = 0; i < 10; ++i) {
            const float angle = -1.5707963f + i * 0.6283185f, r = i % 2 == 0 ? radius : radius * 0.45f;
            points[i] = ImVec2(c.x + std::cos(angle) * r, c.y + std::sin(angle) * r);
        }
        // Not convex: the star is drawn as triangles from its center.
        for (int i = 0; i < 10; ++i) draw->AddTriangleFilled(c, points[i], points[(i + 1) % 10], color);
    }

    // The keyboard on screen for the gamepad: the search (Y / Triangle on a console's page), or the chat's box (Y on
    // the Chat tab). The d-pad picks a key, A types it, B closes. Mouse clicks work too. True while it is open (the
    // page then ignores the d-pad).
    bool keyboardOpen = false;
    int keyFocus = 0;
    // What it types into: null is the search; else the chat's box, sent by Enviar (keyDone).
    char* keyTarget = nullptr;
    size_t keyTargetSize = 0;
    std::string keyTitle;
    std::function<void()> keyDone;

    void OpenTextKeyboard(char* target, size_t size, const std::string& title, std::function<void()> done) {
        keyTarget = target;
        keyTargetSize = size;
        keyTitle = title;
        keyDone = std::move(done);
        keyboardOpen = true;
        keyFocus = 0;
    }

    bool SearchKeyboard(const Nav& nav, float scale) {
        if (!keyboardOpen) return false;
        static const char* searchKeys[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
                                           "U", "V", "W", "X", "Y", "Z", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "Espaço", "Apagar", "Limpar", "Pronto"};
        static const char* chatKeys[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N", "O", "P", "Q", "R", "S", "T",
                                         "U", "V", "W", "X", "Y", "Z", ".", ",", "?", "!", "0", "1", "2", "3", "4", "5", "6", "7", "8", "9",
                                         "Espaço", "Apagar", "Limpar", "Enviar"};
        const bool chatting = keyTarget != nullptr;
        const char* const* keys = chatting ? chatKeys : searchKeys;
        const int count = chatting ? IM_ARRAYSIZE(chatKeys) : IM_ARRAYSIZE(searchKeys), columns = 10, firstWide = count - 4;
        char* buffer = chatting ? keyTarget : librarySearch.data();
        const size_t room = chatting ? keyTargetSize : librarySearch.size();
        if (nav.left && keyFocus % columns > 0) --keyFocus;
        if (nav.right && keyFocus % columns < columns - 1 && keyFocus + 1 < count) ++keyFocus;
        if (nav.up && keyFocus >= columns) keyFocus -= columns;
        if (nav.down) keyFocus = std::min(count - 1, keyFocus + columns);
        int typed = nav.accept ? keyFocus : -1;
        const ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(ImVec2(center.x, ImGui::GetMainViewport()->Size.y - 30 * scale), ImGuiCond_Always, ImVec2(0.5f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(18 * scale, 16 * scale));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Color(kSidebar));
        ImGui::Begin("##teclado", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings);
        ImGui::TextDisabled("%s", chatting ? keyTitle.c_str() : "Buscar:");
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 560 * scale);
        ImGui::TextUnformatted(buffer[0] ? buffer : "_");
        ImGui::PopTextWrapPos();
        const ImVec2 key(54 * scale, 44 * scale);
        for (int k = 0; k < count; ++k) {
            if (k % columns != 0) ImGui::SameLine(0, 6 * scale);
            const bool wide = k >= firstWide;
            if (ImGui::Button(keys[k], wide ? ImVec2(key.x * 2.4f, key.y) : key)) typed = k;
            if (k == keyFocus) Focus(ImGui::GetWindowDrawList(), ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), scale, 6 * scale);
        }
        ImGui::TextDisabled("Direcional: escolher    A: digitar    B: fechar");
        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar();
        const size_t length = std::strlen(buffer);
        if (typed >= 0 && typed < firstWide && length + 1 < room) {
            buffer[length] = static_cast<char>(std::tolower(static_cast<unsigned char>(keys[typed][0])));
            buffer[length + 1] = 0;
        } else if (typed == firstWide && length + 1 < room) {
            buffer[length] = ' ';
            buffer[length + 1] = 0;
        } else if (typed == firstWide + 1 && length > 0) {
            // A whole character (an accent or an emoji take more than one byte).
            size_t cut = length - 1;
            while (cut > 0 && (static_cast<unsigned char>(buffer[cut]) & 0xc0) == 0x80) --cut;
            buffer[cut] = 0;
        } else if (typed == firstWide + 2) {
            std::memset(buffer, 0, room);
        }
        if (typed == firstWide + 3 || nav.back) {
            keyboardOpen = false;
            if (typed == firstWide + 3 && keyDone) keyDone();
            keyTarget = nullptr;
            keyDone = nullptr;
        }
        return true;
    }

    // Lower case without accents, for the search: "Pokémon" and "pokemon" meet.
    static std::wstring Plainer(std::wstring text) {
        static const std::wstring accented = L"àáâãäåçèéêëìíîïñòóôõöùúûüýÿ", plain = L"aaaaaaceeeeiiiinooooouuuuyy";
        for (auto& c : text) {
            c = static_cast<wchar_t>(towlower(c));
            if (const size_t at = accented.find(c); at != std::wstring::npos) c = plain[at];
        }
        return text;
    }

    void ConsolePage(float scale, const Nav& input) {
        auto games = ConsoleGames(console);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        // The top row here is Consoles, Adicionar jogo and Emuladores.
        // Y / Triangle opens the keyboard on screen for the search; while it is open the d-pad, A and B are its own.
        if (!keyboardOpen && input.search && !games.empty()) {
            keyboardOpen = true;
            keyFocus = 0;
        }
        const bool typing = SearchKeyboard(input, scale);
        const bool inBar = !typing && TopBar(input, 7);
        const Nav nav = inBar || typing ? Nav{} : input;
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
        if (!games.empty()) {
            ImGui::SameLine(0, 18 * scale);
            ImGui::SetCursorPosY(22 * scale);
            if (ImGui::Button("Pasta de jogos", ImVec2(0, 36 * scale))) pending = [this] { OpenGamesFolder(); };
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abre a pasta deste console. O que você colocar nela entra na lista sozinho.");
            // Search: the games whose name has what was typed (no matter accents or capitals).
            ImGui::SameLine(0, 12 * scale);
            ImGui::SetCursorPosY(22 * scale);
            ImGui::SetNextItemWidth(220 * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(10 * scale, 9 * scale));
            ImGui::InputTextWithHint("##busca", "Buscar jogo...", librarySearch.data(), librarySearch.size());
            ImGui::PopStyleVar();
        }
        TopButtons(scale, size.x - left, 1);

        if (games.empty()) {
            // Left and right go along the three ways to add a game; up reaches the top row.
            if (nav.left && emptyFocus > 0) --emptyFocus;
            if (nav.right && emptyFocus < 3) ++emptyFocus;
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
            ImGui::SameLine();
            if (ImGui::Button("Abrir pasta de jogos", ImVec2(190 * scale, 38 * scale)) || pressed == 3) pending = [this] { OpenGamesFolder(); };
            FocusLast(here && emptyFocus == 3, scale);
            ImGui::Dummy(ImVec2(0, 4 * scale));
            ImGui::TextDisabled("Cada console tem a sua pasta de jogos: o que você colocar nela entra na lista sozinho.");
            ImGui::TextDisabled("%s", Utf8(library.GamesFolder(console)).c_str());
            ImGui::EndGroup();
            return;
        }

        if (librarySearch[0] != 0) {
            const std::wstring wanted = Plainer(Wide(librarySearch.data()));
            std::erase_if(games, [&](int index) { return Plainer(library.Games()[static_cast<size_t>(index)].title).find(wanted) == std::wstring::npos; });
            if (games.empty()) {
                ImGui::SetCursorPos(ImVec2(left, 110 * scale));
                ImGui::TextDisabled("Nenhum jogo com \"%s\" no nome.", librarySearch.data());
                return;
            }
        }
        std::stable_partition(games.begin(), games.end(), [&](int index) { return IsFavorite(library.Games()[static_cast<size_t>(index)]); });
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
            const ImVec2 base = ImGui::GetItemRectMin();
            const float lift = Smooth("grid:" + Utf8(game.titleId), hovered || (k == gridFocus && barFocus < 0) ? 1.0f : 0.0f, 18.0f) * 5 * scale;
            const ImVec2 min(base.x - lift, base.y - lift), max(base.x + tile + lift, base.y + tile + lift);
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
            if (const std::string played = PlayedTime(game); !played.empty())
                grid->AddText(nullptr, ImGui::GetFontSize() * 0.85f, ImVec2(min.x + 2 * scale, max.y + 8 * scale + ImGui::GetFontSize() * 1.35f), Packed(kDim), played.c_str());
            ImGui::PopClipRect();
            if (IsFavorite(game)) {
                // On the cover's top right corner, on a dark round so it shows on any art.
                const ImVec2 corner(max.x - 18 * scale, min.y + 18 * scale);
                grid->AddCircleFilled(corner, 14 * scale, Packed(0x000000, 0.6f), 24);
                DrawStar(grid, corner, 10 * scale, Packed(kAmber));
            }
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
            ImGui::TextDisabled(ShowPs4Ps5(library.Root()) ? "   PS5, Bloodborne, PS3, Xbox 360, Wii U" : "   PS3, Xbox 360, Wii U");
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
        if (External(game) && EngineFor(game.system).native) {
            // One choice for this PC, for every console Ranga runs itself.
            const char* levels[] = {"Ruim (PC bem fraco)", "Fraco", "Bom (padrão)", "Ótimo (PC forte)"};
            const wchar_t* values[] = {L"ruim", L"fraco", L"bom", L"otimo"};
            const std::wstring current = Quality(library.Root());
            int level = 2;
            for (int i = 0; i < 4; ++i) if (current == values[i]) level = i;
            label("Qualidade");
            if (ImGui::Combo("##quality", &level, levels, 4)) {
                SetQuality(library.Root(), values[level]);
                RefreshEngines();
            }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::TextDisabled("Vale para todos os consoles que rodam no Ranga, a partir da próxima vez que o jogo abrir.");
            static const char* const notes[] = {
                "Resolução do console ou menor, sem esperar shaders: o mais leve possível; a imagem fica serrilhada.",
                "Resolução perto da original, sem esperar shaders: para placa de vídeo integrada ou antiga.",
                "Por volta de 1080p: o que o Ranga sempre usou.",
                "Por volta de 1440p a 4K: para placa de vídeo forte; pesa bem mais.",
            };
            ImGui::SetCursorPosX(labelWidth);
            ImGui::TextDisabled("%s", notes[level]);
            if (game.system == L"SWITCH") SwitchGraphics(game, label, labelWidth, scale);
            else if (const auto* list = ConsoleGraphics(game.system); list != nullptr && EngineFor(game.system).native)
                GraphicsSection(game, *list, ("Gráficos do " + SystemName(game.system) + " (só este jogo)").c_str(),
                                "Automático segue a Qualidade acima ou o padrão do console. Vale quando o jogo abrir de novo; no jogo, o menu (Esc "
                                "ou botão PS/Guide) > Opções do core mostra todas as opções do console.",
                                label, labelWidth, scale);
        }
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
        if (ClassicOnline(game.system) && EngineFor(game.system).native) OnlineSection(game, label, labelWidth, scale);
        if (game.system == L"PSP" && EngineFor(game.system).native) PspOnlineSection(game, label, scale);
        if (game.system == L"3DS" && EngineFor(game.system).native) Room3dsSection(game, label, labelWidth, scale);
        if (game.system == L"PS3" && EngineFor(game.system).native) Ps3OnlineSection(game, label, labelWidth, scale);
        if (game.system == L"XBOX" && EngineFor(game.system).native) XboxOnlineSection(game, label, labelWidth, scale);
        if (game.system == L"SWITCH" && EngineFor(game.system).native) ShaderCacheSection(game, label, labelWidth, scale);
        if (game.system == L"SWITCH" && EngineFor(game.system).native) {
            // Eden's multiplayer rooms: the game's local wireless / LAN mode goes through a room over UDP. The core
            // joins or hosts the room when the game starts, so nothing here changes a game that is already open.
            ImGui::Dummy(ImVec2(0, 6 * scale));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 2 * scale));
            const bool running = runner.Busy() && runner.TitleId() == game.titleId;
            ImGui::BeginDisabled(running);
            const char* modes[] = {"Desligada", "Entrar em uma sala", "Criar uma sala"};
            const char* modeValues[] = {"desligada", "entrar", "criar"};
            label("Nome do jogador");
            ImGui::InputText("##playerName", roomNickname.data(), 32);
            if (ImGui::IsItemDeactivatedAfterEdit()) SavePlayerName(game, roomNickname.data(), true);
            ImGui::SetCursorPosX(labelWidth);
            ImGui::TextDisabled("Aparece nos jogos de Switch no lugar de \"Eden\" e é o seu apelido na sala.");
            ImGui::Dummy(ImVec2(0, 2 * scale));
            label("Sala online");
            if (ImGui::Combo("##room", &roomMode, modes, 3)) WriteCoreOption(game, "eden_sala", modeValues[roomMode]);
            if (roomMode == 1) {
                label("Tipo de sala");
                if (ImGui::RadioButton("Privada (a sua, pelo playit)", !roomPublic) && roomPublic) SetRoomPublic(game, false);
                ImGui::SameLine();
                if (ImGui::RadioButton("Pública (lista do Eden)", roomPublic) && !roomPublic) SetRoomPublic(game, true);
            }
            if (roomMode == 1 && roomPublic) PublicRooms(game, labelWidth, scale);
            if (roomMode != 0) {
                const auto field = [&](const char* name, const char* id, std::array<char, 128>& text, const char* key, const wchar_t* remembered, ImGuiInputTextFlags flags = 0) {
                    label(name);
                    ImGui::InputText(id, text.data(), text.size(), flags);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        WriteCoreOption(game, key, text.data());
                        RememberSwitch(remembered, text.data());
                    }
                };
                const bool typed = !(roomMode == 1 && roomPublic);
                if (roomMode == 1 && typed) {
                    field("Endereço", "##roomAddress", roomAddress, "eden_sala_endereco", L"sala_endereco");
                    ProbeButton("switch", roomAddress.data(), std::atoi(roomPort.data()) > 0 ? std::atoi(roomPort.data()) : 24872, false, scale);
                }
                // Joining with "address:port" (as playit shows it), that port is the one used: the field is not shown,
                // so a leftover 24872 there can no longer look like it matters.
                const std::string address = roomAddress.data();
                const size_t colon = address.rfind(':');
                const bool portInAddress = colon != std::string::npos && colon + 1 < address.size() &&
                                           std::all_of(address.begin() + static_cast<std::ptrdiff_t>(colon + 1), address.end(), [](unsigned char c) { return std::isdigit(c); });
                if (!typed) {
                } else if (roomMode == 1 && portInAddress) {
                    label("Porta");
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s (a do endereço)", address.substr(colon + 1).c_str());
                } else {
                    field("Porta", "##roomPort", roomPort, "eden_sala_porta", L"sala_porta", ImGuiInputTextFlags_CharsDecimal);
                }
                if (typed) field("Senha", "##roomPassword", roomPassword, "eden_sala_senha", L"sala_senha");
                const std::string nickname = roomNickname.data();
                const bool nicknameFits = nickname.size() >= 4 && nickname.size() <= 20 &&
                    std::all_of(nickname.begin(), nickname.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' '; });
                const char* problem = !nicknameFits ? "Para a sala, o nome do jogador precisa de 4 a 20 letras ou números, sem acento." :
                                      roomMode == 1 && roomAddress[0] == 0 ? (roomPublic ? "Escolha uma sala na lista." : "Falta o endereço que o dono da sala passou.") : nullptr;
                if (problem != nullptr) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                    ImGui::TextWrapped("%s", problem);
                    ImGui::PopStyleColor();
                }
                ImGui::PushTextWrapPos(0);
                if (roomMode == 1 && !typed) ImGui::TextDisabled("Sala pública: outras pessoas podem estar nela e ver o seu apelido; no jogo, só joga com você quem entrar na mesma partida. A sala é de outra pessoa e pode sair do ar.");
                else if (roomMode == 1) ImGui::TextDisabled("Cole o endereço como o dono da sala passou; \"endereço:porta\" junto também serve, e aí a porta pode ficar vazia. Senha só se a sala tiver.");
                else ImGui::TextDisabled("A sala abre junto com o jogo, na porta escolhida (vazia = 24872), em UDP. Para amigos de fora entrarem, aponte um túnel UDP (playit, por exemplo) para 127.0.0.1 nessa porta e passe a eles o endereço do túnel.");
                ImGui::TextDisabled("No jogo, todos escolhem o modo sem fio local / LAN, não o online da Nintendo. Todos precisam da mesma versão do emulador e do jogo.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndDisabled();
            if (running) ImGui::TextDisabled("Feche o jogo para mudar a sala.");
            RoomsSection(game, label, labelWidth, scale);
        }
        if (ShowsDlss5(game)) {
            ImGui::Dummy(ImVec2(0, 6 * scale));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 2 * scale));
            Dlss5Section(game);
        }
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
            Browse(isFile ? L"Escolha o novo arquivo do jogo" : L"Escolha a nova pasta do jogo", start, !isFile,
                   isFile ? GameFileFilters() : std::vector<std::pair<std::wstring, std::wstring>>{},
                   [this, titleId = game.titleId, system = game.system](const std::wstring& path) {
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
                   });
        }
        ImGui::SameLine();
        if (ImGui::Button("Capa...")) {
            Browse(L"Escolha uma imagem PNG para a capa", L"", false, {{L"Imagem PNG", L"*.png"}}, [this](const std::wstring& image) {
                Game* current = Current();
                if (current == nullptr) return;
                MakeFolders(current->workspace);
                CreateDirectoryW((current->workspace + L"\\art").c_str(), nullptr);
                CopyFileW(image.c_str(), (current->workspace + L"\\art\\cover.png").c_str(), FALSE);
                ForgetArt(*current);
            });
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

    // What the chosen game drew with the last time it ran in Ranga Retro (video.txt): vulkan, opengl, d3d11 or proprio.
    std::string gameVideo;

    // Consoles whose cores draw in software (Direct3D 11 in Ranga Retro): DLSS 5, a Vulkan layer, never reaches them.
    static bool SoftwareConsole(const std::wstring& system) {
        static const wchar_t* consoles[] = {L"NES", L"SNES", L"MD", L"SMS", L"SEGACD", L"PCE", L"A2600", L"ARCADE", L"GB", L"GBA"};
        for (const wchar_t* id : consoles) if (system == id) return true;
        return false;
    }

    // Whether the DLSS 5 part shows for this game at all: not for a 2D console in Ranga Retro, unless it was left on.
    bool ShowsDlss5(const Game& game) {
        return options.dlss5 || !External(game) || !EngineFor(game.system).native || !SoftwareConsole(game.system);
    }

    void Dlss5Section(const Game& game) {
        // Its files go in and out when the game starts, so the switch only changes while the game is closed.
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        // In Ranga Retro the layer only takes hold of a game drawing with Vulkan; what it drew with last time says.
        const bool native = External(game) && EngineFor(game.system).native;
        const bool notVulkan = native && !gameVideo.empty() && gameVideo != "vulkan";
        ImGui::BeginDisabled(running || (notVulkan && !options.dlss5));
        if (ImGui::Checkbox("DLSS 5 (experimental)", &options.dlss5)) {
            dlss5Kit = InspectDlss5(library.Root());
            Save();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (running) {
            ImGui::TextDisabled("Feche o jogo para ligar ou desligar.");
        } else if (notVulkan) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(options.dlss5 ? kAmber : kDim));
            ImGui::TextWrapped("Este jogo desenha com %s no Ranga: o DLSS 5 só pega em jogos que desenham com Vulkan.",
                               gameVideo == "opengl" ? "OpenGL" : gameVideo == "d3d11" ? "Direct3D" : "a própria janela do emulador");
            ImGui::PopStyleColor();
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
        if (native && gameVideo.empty()) ImGui::TextDisabled("Abra o jogo uma vez: o Ranga anota se ele desenha com Vulkan, o único jeito que o DLSS 5 pega.");
        else if (External(game) && !native) ImGui::TextDisabled("Em outro emulador, só pega se ele estiver desenhando com Vulkan (escolha Vulkan nas configurações de vídeo dele).");
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

    void ReloadRooms() {
        rooms = LoadRooms(library.Root());
        roomNames.clear();
        roomPasswords.clear();
        roomPorts.clear();
        for (const Room& room : rooms) {
            Utf8(room.name).copy(roomNames[room.id].data(), 63);
            Utf8(room.password).copy(roomPasswords[room.id].data(), 63);
            std::to_string(room.port).copy(roomPorts[room.id].data(), 63);
        }
        roomsCheckedAt = 0;
        roomsLoaded = true;
    }

    // The rooms this PC keeps open for everyone (Eden's room server), each with its name, port, password and limit,
    // switched on and off here. Everyone, this PC's owner too, then joins with "Entrar em uma sala".
    // Online play of the classics: every player runs the same game in Ranga and only the buttons travel, through the
    // table server (tools\ranga-sala.exe) on the PC that hosts. The first to open the game runs it for the others.
    template <typename Label> void OnlineSection(const Game& game, Label& label, float labelWidth, float scale) {
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        label("Jogar online");
        if (ImGui::Checkbox("##online", &onlineOn)) WriteCoreOption(game, "ranga_online", onlineOn ? "ligado" : "desligado");
        ImGui::SameLine();
        ImGui::TextDisabled(onlineOn ? "ligado: o jogo entra na sala quando abrir" : "desligado");
        if (onlineOn) {
            const auto field = [&](const char* name, const char* id, std::array<char, 128>& text, const char* key, const wchar_t* remembered) {
                label(name);
                ImGui::InputText(id, text.data(), text.size());
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    WriteCoreOption(game, key, text.data());
                    RememberOnline(remembered, text.data());
                }
            };
            field("Endereço da sala", "##onlineAddress", onlineAddress, "ranga_online_endereco", L"online_endereco");
            ProbeButton("classicos", onlineAddress.data(), 24873, true, scale);
            TablesButton(onlineAddress.data(), labelWidth, scale);
            field("Seu nome", "##onlineName", onlineName, "ranga_online_nome", L"online_nome");
            label("Atraso");
            if (ImGui::Checkbox("Automático##onlineDelayAuto", &onlineDelayAuto))
                WriteCoreOption(game, "ranga_online_atraso", onlineDelayAuto ? "auto" : std::to_string(onlineDelay));
            if (!onlineDelayAuto) {
                ImGui::SameLine();
                ImGui::SetNextItemWidth(220 * scale);
                if (ImGui::SliderInt("##onlineDelay", &onlineDelay, 2, 8, "%d quadros")) WriteCoreOption(game, "ranga_online_atraso", std::to_string(onlineDelay));
            }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled(onlineDelayAuto ? "Quem hospeda mede o ping de cada amigo e ajusta o atraso sozinho durante o jogo (vale o de quem hospeda)."
                                                : "Vale o atraso de quem hospeda: menos é mais rápido, mais aguenta conexão ruim sem engasgar.");
            ImGui::PopTextWrapPos();
            if (onlineAddress[0] == 0) {
                ImGui::SetCursorPosX(labelWidth);
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                ImGui::TextUnformatted("Falta o endereço da sala (o do playit que o dono passou).");
                ImGui::PopStyleColor();
            }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Todos abrem o mesmo jogo (o mesmo arquivo) com o mesmo endereço. Quem abrir primeiro é o anfitrião: os outros "
                                "entram no jogo dele, até 4 jogadores, cada um com o próprio controle. Os estados (salvar/carregar) ficam "
                                "desligados enquanto joga online, e o save de quem entra não é mexido.");
            ImGui::SetCursorPosX(labelWidth);
            ImGui::TextDisabled("Atraso: quanto maior, menos o jogo para esperando o amigo, mas os botões respondem um pouco depois. "
                                "4 serve para quase todos; suba se o título mostrar \"esperando o outro jogador\" com frequência.");
            if (game.system == L"NDS" || game.system == L"DC" || game.system == L"N64" || game.system == L"SATURN" || game.system == L"GC" ||
                game.system == L"WII" || game.system == L"PS2") {
                ImGui::SetCursorPosX(labelWidth);
                ImGui::TextDisabled("Neste console o online é experimental: o estado do jogo é grande e pode demorar alguns segundos para o amigo entrar.");
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndDisabled();
        if (running) ImGui::TextDisabled("Feche o jogo para mudar o online.");
        ServerSection(Extra::Classics, label, labelWidth, scale);
    }

    // The 3DS's local wireless through Azahar's room (as the Switch's): every player joins the room, and in the game
    // they meet as if side by side. The room server runs on the PC that hosts (tools\azahar-room.exe, UDP).
    template <typename Label> void Room3dsSection(const Game& game, Label& label, float labelWidth, float scale) {
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        label("Sala online");
        if (ImGui::Checkbox("##room3ds", &room3dsOn)) WriteCoreOption(game, "citra_sala", room3dsOn ? "entrar" : "desligada");
        ImGui::SameLine();
        ImGui::TextDisabled(room3dsOn ? "ligada: o jogo entra na sala quando abrir" : "desligada");
        if (room3dsOn) {
            label("Endereço da sala");
            ImGui::InputText("##room3dsAddress", room3dsAddress.data(), room3dsAddress.size());
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WriteCoreOption(game, "citra_sala_endereco", room3dsAddress.data());
                RememberOnline(L"sala3ds_endereco", room3dsAddress.data());
            }
            ProbeButton("3ds", room3dsAddress.data(), 24874, false, scale);
            label("Seu nome");
            ImGui::InputText("##room3dsName", room3dsName.data(), 32);
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WriteCoreOption(game, "citra_sala_nome", room3dsName.data());
                RememberOnline(L"online_nome", room3dsName.data());
            }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Igual à sala do Switch: todos entram na mesma sala e, no jogo, escolhem o multijogador local (sem fio). "
                                "Cada um precisa de um nome diferente. É a sala do 3DS, com endereço próprio (não serve o do Switch).");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndDisabled();
        if (running) ImGui::TextDisabled("Feche o jogo para mudar a sala.");
        ServerSection(Extra::Rooms3ds, label, labelWidth, scale);
    }

    // The Xbox's System Link (its LAN play) over the internet or the house: xemu sends the Xbox's network frames to
    // the hub of the PC that hosts (ranga-sala --hub-xbox, UDP 24875 beside the classics' server) and the hub passes
    // them to the others. A playit UDP tunnel to that port carries it to friends far away.
    template <typename Label> void XboxOnlineSection(const Game& game, Label& label, float labelWidth, float scale) {
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        bool on = ReadCoreOption(game, "xemu_network_backend") == "udp";
        label("System Link (online)");
        if (ImGui::Checkbox("##xboxOnline", &on)) {
            WriteCoreOption(game, "xemu_network_backend", on ? "udp" : "disabled");
            if (on && ReadCoreOption(game, "xemu_network_udp_remote").empty()) {
                const std::string remembered = Utf8(RememberedOnline(L"xbox_hub"));
                WriteCoreOption(game, "xemu_network_udp_remote", remembered.empty() ? "127.0.0.1:" + std::to_string(XboxHubPort(library.Root())) : remembered);
            }
            xboxHubAddress.fill(0);
        }
        ImGui::SameLine();
        ImGui::TextDisabled(on ? "ligado: o Xbox entra na rede do hub quando o jogo abrir" : "desligado");
        if (on) {
            if (xboxHubAddress[0] == 0) {
                const std::string current = ReadCoreOption(game, "xemu_network_udp_remote");
                current.copy(xboxHubAddress.data(), xboxHubAddress.size() - 1);
            }
            label("Endereço do hub");
            ImGui::InputText("##xboxHub", xboxHubAddress.data(), xboxHubAddress.size());
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                WriteCoreOption(game, "xemu_network_udp_remote", xboxHubAddress.data());
                RememberOnline(L"xbox_hub", xboxHubAddress.data());
            }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Para jogos com System Link no menu (Halo, por exemplo). Todos põem o "
                                "mesmo endereço: o do túnel UDP do playit para a porta %d do PC que hospeda (ou, na mesma casa, o "
                                "192.168... desse PC com a porta %d). No jogo, escolha System Link. O hub roda junto com a sala dos "
                                "clássicos.", XboxHubPort(library.Root()), XboxHubPort(library.Root()));
            ImGui::PopTextWrapPos();
        }
        ImGui::EndDisabled();
        if (running) ImGui::TextDisabled("Feche o jogo para mudar o online.");
    }

    // Cache de shaders: the Switch game's portable shader cache (Eden's vulkan.bin) fetched from the PC that hosts
    // (its classics' server, /ranga/shaders/<id>.bin), so the first races don't stutter while shaders compile. It
    // replaces the one here only when bigger (more shaders); the GPU's own part (vulkan_pipelines.bin) is rebuilt here.
    std::atomic<bool> shaderBusy{false};
    std::string shaderResult; // under probeLock

    template <typename Label> void ShaderCacheSection(const Game& game, Label& label, float labelWidth, float scale) {
        const std::wstring id = DetectSwitchContent(game.folder).titleId;
        if (id.size() != 16) return;
        const std::wstring local = library.Root() + L"\\bios\\retroarch\\eden\\shader\\" + Lower(id) + L"\\vulkan.bin";
        unsigned long long stamp = 0, size = 0;
        FileStamp(local, stamp, size);
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        label("Cache de shaders");
        ImGui::AlignTextToFramePadding();
        if (size > 0) ImGui::Text("%.1f MB neste PC", size / 1048576.0);
        else ImGui::TextDisabled("nenhum ainda");
        if (!RoomServerEnabled(library.Root())) {
            ImGui::SameLine(0, 12 * scale);
            const bool running = runner.Busy() && runner.TitleId() == game.titleId;
            ImGui::BeginDisabled(shaderBusy || running);
            if (ImGui::Button(shaderBusy ? "Baixando...###shaders" : "Baixar dos amigos###shaders")) {
                shaderBusy = true;
                std::thread([this, root = library.Root(), id, local, size] {
                    std::string result = "O PC que hospeda não respondeu ou ainda não tem cache deste jogo.";
                    for (const std::wstring& source : UpdateSources(root)) {
                        if (source.find(L"github") != std::wstring::npos) continue;
                        const HttpResult answer = HttpRequest("GET", Utf8(source) + "/ranga/shaders/" + Utf8(Lower(id)) + ".bin", "", "", "RangaEmu", 120000);
                        if (answer.status != 200 || answer.body.size() < 64) continue;
                        if (answer.body.size() <= size) {
                            result = "O seu cache já é do mesmo tamanho ou maior: nada mudou.";
                            break;
                        }
                        CreateDirectoryW((root + L"\\bios\\retroarch\\eden\\shader").c_str(), nullptr);
                        CreateDirectoryW(local.substr(0, local.find_last_of(L'\\')).c_str(), nullptr);
                        if (FILE* file = _wfopen(local.c_str(), L"wb")) {
                            std::fwrite(answer.body.data(), 1, answer.body.size(), file);
                            std::fclose(file);
                            char text[160];
                            std::snprintf(text, sizeof(text), "Cache de %.1f MB baixado: as primeiras corridas não devem mais travar.", answer.body.size() / 1048576.0);
                            result = text;
                        }
                        break;
                    }
                    std::lock_guard guard(probeLock);
                    shaderResult = result;
                    shaderBusy = false;
                }).detach();
            }
            ImGui::EndDisabled();
        }
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushTextWrapPos(0);
        std::string shown;
        {
            std::lock_guard guard(probeLock);
            shown = shaderResult;
        }
        if (!shown.empty()) ImGui::TextUnformatted(shown.c_str());
        else if (RoomServerEnabled(library.Root()))
            ImGui::TextDisabled("Este PC hospeda: o cache dele vai para os amigos sozinho, pela sala dos clássicos.");
        else
            ImGui::TextDisabled("Os shaders que o PC que hospeda já compilou neste jogo, para as primeiras partidas não travarem. Não contém o jogo.");
        ImGui::PopTextWrapPos();
    }

    // Whether the PS3 game's code was compiled once to the end (the core leaves cache\<serial>\ranga-pronto.txt);
    // looked at again every few seconds, since it appears while the game runs.
    bool Ps3Compiled(const Game& game) {
        if (game.serial.empty()) return true;
        static std::map<std::wstring, std::pair<ULONGLONG, bool>> known;
        auto& [at, ready] = known[game.serial];
        if (!ready && GetTickCount64() - at > 5000) {
            at = GetTickCount64();
            ready = Exists(library.Root() + L"\\bios\\retroarch\\rpcs3\\cache\\" + game.serial + L"\\ranga-pronto.txt");
        }
        return ready;
    }

    // The PS3's online through RPCN, RPCS3's own free server (rpcs3.net): the game logs in with the player's RPCN
    // account and finds the others there, as on the PSN. The account lives in the core's rpcn.yml; it is created in
    // RPCS3's own window (Configuração > RPCN, with an e-mail for the token), and Ranga takes it from there.
    std::wstring RpcnFile() const { return library.Root() + L"\\bios\\retroarch\\rpcs3\\config\\rpcn.yml"; }
    std::wstring StandaloneRpcnFile() const { return library.Root() + L"\\emuladores\\rpcs3\\config\\rpcn.yml"; }

    // The NPID (RPCN's user name) and whether a password is kept, from an rpcn.yml; empty name: no account there.
    static std::pair<std::string, bool> RpcnAccount(const std::wstring& path) {
        const auto data = ReadBytes(path);
        std::string name;
        bool password = false;
        size_t start = 0;
        while (start < data.size()) {
            size_t end = data.find('\n', start);
            if (end == std::string::npos) end = data.size();
            std::string line = data.substr(start, end - start);
            start = end + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto value = [&](const char* key) -> std::optional<std::string> {
                const std::string prefix = std::string(key) + ":";
                if (line.rfind(prefix, 0) != 0) return std::nullopt;
                std::string rest = line.substr(prefix.size());
                while (!rest.empty() && (rest.front() == ' ' || rest.front() == '"')) rest.erase(0, 1);
                while (!rest.empty() && (rest.back() == ' ' || rest.back() == '"')) rest.pop_back();
                return rest;
            };
            if (const auto npid = value("NPID")) name = *npid;
            if (const auto pass = value("Password")) password = !pass->empty();
        }
        return {password ? name : std::string(), password};
    }

    template <typename Label> void Ps3OnlineSection(const Game& game, Label& label, float labelWidth, float scale) {
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        bool on = ReadCoreOption(game, "rpcs3_online") == "ligado";
        label("Online (RPCN)");
        if (ImGui::Checkbox("##ps3online", &on)) WriteCoreOption(game, "rpcs3_online", on ? "ligado" : "desligado");
        ImGui::SameLine();
        ImGui::TextDisabled(on ? "ligado: o jogo entra no RPCN quando abrir" : "desligado");
        if (on) {
            // Where: the RPCN server of the PC that hosts (Ranga's: no sign-up, the PS3 core makes the account), or the
            // official one at rpcs3.net, with an account made in RPCS3's own window.
            static const char* where[] = {"Servidor do Ranga (sem cadastro)", "RPCN oficial (conta sua no rpcs3.net)"};
            int mode = ReadCoreOption(game, "rpcs3_servidor") == "oficial" ? 1 : 0;
            label("Servidor");
            if (ImGui::Combo("##ps3where", &mode, where, 2)) WriteCoreOption(game, "rpcs3_servidor", mode == 1 ? "oficial" : "ranga");
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            if (mode == 0) {
                if (ps3ServerAddress[0] == 0) {
                    std::string current = ReadCoreOption(game, "rpcs3_servidor_endereco");
                    if (current.empty()) current = Utf8(RememberedOnline(L"ps3_servidor"));
                    current.copy(ps3ServerAddress.data(), ps3ServerAddress.size() - 1);
                }
                label("Endereço do servidor");
                ImGui::InputTextWithHint("##ps3server", "192.168.0.10:31313", ps3ServerAddress.data(), ps3ServerAddress.size());
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    WriteCoreOption(game, "rpcs3_servidor_endereco", ps3ServerAddress.data());
                    RememberOnline(L"ps3_servidor", ps3ServerAddress.data());
                }
                ImGui::SetCursorPosX(labelWidth);
                const std::string name = PlayerRpcnName(library.Root());
                ImGui::TextDisabled("Seu nome no PS3: %s (o mesmo do online do Ranga; a conta é criada sozinha na primeira vez). "
                                    "O endereço é o do PC que liga o servidor do PS3: na mesma casa, o 192.168... dele com :31313; "
                                    "pela internet, o IP público dele com :31313, com as portas abertas no roteador dele. Os PS3 se "
                                    "ligam direto um no outro; o servidor abre esse caminho sozinho na maioria das internets "
                                    "(só falha em CGNAT dos dois lados).", name.c_str());
            } else {
                const auto [account, kept] = RpcnAccount(StandaloneRpcnFile());
                label("Conta");
                ImGui::PushStyleColor(ImGuiCol_Text, Color(account.empty() ? kAmber : kGreen));
                ImGui::TextUnformatted(account.empty() ? "nenhuma ainda" : account.c_str());
                ImGui::PopStyleColor();
                const std::wstring rpcs3 = library.Root() + L"\\emuladores\\rpcs3\\rpcs3.exe";
                if (Exists(rpcs3)) {
                    ImGui::SetCursorPosX(labelWidth);
                    if (ImGui::Button(account.empty() ? "Abrir o RPCS3 para criar a conta" : "Abrir o RPCS3 (conta do RPCN)")) {
                        ShellExecuteW(nullptr, L"open", rpcs3.c_str(), nullptr, (library.Root() + L"\\emuladores\\rpcs3").c_str(), SW_SHOWNORMAL);
                    }
                }
                ImGui::SetCursorPosX(labelWidth);
                ImGui::TextDisabled("A conta do RPCN oficial é feita no RPCS3, em Configuração > RPCN, com seu e-mail e o token que chega "
                                    "por e-mail; o Ranga usa a mesma conta quando o jogo abrir. Cada roteador precisa de UPnP ligado.");
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndDisabled();
        if (running) ImGui::TextDisabled("Feche o jogo para mudar o online.");
        ServerSection(Extra::Ps3, label, labelWidth, scale);
    }

    // The PSP's own online: its local wireless (ad hoc), through a public PRO ad hoc server kept by fans. PPSSPP's
    // server takes a fixed port (27312) and a plain IP, and the players' consoles then talk to each other directly, so
    // a playit tunnel cannot carry it; the public servers can, when the players' routers let the games through (UPnP).
    // PPSSPP's core takes the server's IP as 12 options of one digit each (three per part, zeros in front).
    static void WritePspServerIp(const Game& game, const std::string& address) {
        int parts[4] = {}, count = 0;
        for (size_t at = 0; at <= address.size() && count < 4;) {
            const size_t dot = std::min(address.find('.', at), address.size());
            parts[count++] = std::clamp(std::atoi(address.substr(at, dot - at).c_str()), 0, 255);
            at = dot + 1;
        }
        if (count != 4) return;
        for (int i = 0; i < 4; ++i) {
            const int digits[3] = {parts[i] / 100, parts[i] / 10 % 10, parts[i] % 10};
            for (int d = 0; d < 3; ++d) {
                char key[48];
                std::snprintf(key, sizeof(key), "ppsspp_pro_ad_hoc_server_address%02d", i * 3 + d + 1);
                WriteCoreOption(game, key, std::to_string(digits[d]));
            }
        }
    }

    static std::string PspServerIp(const Game& game) {
        std::string ip;
        for (int i = 0; i < 4; ++i) {
            int part = 0;
            for (int d = 0; d < 3; ++d) {
                char key[48];
                std::snprintf(key, sizeof(key), "ppsspp_pro_ad_hoc_server_address%02d", i * 3 + d + 1);
                part = part * 10 + std::atoi(ReadCoreOption(game, key).c_str());
            }
            ip += (i ? "." : "") + std::to_string(part);
        }
        return ip == "0.0.0.0" ? "" : ip;
    }

    template <typename Label> void PspOnlineSection(const Game& game, Label& label, float scale) {
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::Separator();
        ImGui::Dummy(ImVec2(0, 2 * scale));
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        static const char* servers[] = {"socom.cc", "psp.gameplayer.club", "myneighborsushicat.com"};
        const bool on = ReadCoreOption(game, "ppsspp_enable_wlan") == "enabled";
        const std::string server = ReadCoreOption(game, "ppsspp_change_pro_ad_hoc_server_address");
        int picked = 0;
        for (int i = 0; i < 3; ++i) if (server == servers[i]) picked = i;
        label("Online (ad hoc)");
        bool wanted = on;
        if (ImGui::Checkbox("##pspOnline", &wanted)) {
            WriteCoreOption(game, "ppsspp_enable_wlan", wanted ? "enabled" : "disabled");
            WriteCoreOption(game, "ppsspp_change_pro_ad_hoc_server_address", servers[picked]);
            WriteCoreOption(game, "ppsspp_enable_builtin_pro_ad_hoc_server", "disabled");
        }
        if (wanted) {
            // Where the players meet: a public server on the internet, or, in the same house, the built-in server of
            // PPSSPP on one of the PCs (its port 27312) and the others pointing at that PC's address.
            static const char* modes[] = {"Internet (servidor público)", "Rede de casa: este PC hospeda", "Rede de casa: entrar no PC de outro"};
            const bool hosting = ReadCoreOption(game, "ppsspp_enable_builtin_pro_ad_hoc_server") == "enabled";
            int mode = hosting ? 1 : server == "IP address" ? 2 : 0;
            label("Onde jogar");
            if (ImGui::Combo("##pspMode", &mode, modes, 3)) {
                WriteCoreOption(game, "ppsspp_enable_builtin_pro_ad_hoc_server", mode == 1 ? "enabled" : "disabled");
                WriteCoreOption(game, "ppsspp_change_pro_ad_hoc_server_address", mode == 0 ? servers[picked] : mode == 1 ? "localhost" : "IP address");
                if (mode == 2) {
                    // The last address typed for any PSP game comes filled in.
                    const std::string remembered = Utf8(RememberedOnline(L"psp_lan"));
                    pspLanAddress.fill(0);
                    remembered.copy(pspLanAddress.data(), pspLanAddress.size() - 1);
                    if (!remembered.empty()) WritePspServerIp(game, remembered);
                }
            }
            ImGui::PushTextWrapPos(0);
            if (mode == 0) {
                label("Servidor");
                if (ImGui::Combo("##pspServer", &picked, servers, 3)) WriteCoreOption(game, "ppsspp_change_pro_ad_hoc_server_address", servers[picked]);
                ImGui::TextDisabled("Para jogos com multijogador local do PSP (Monster Hunter, Tekken, Ridge Racer...). Todos escolhem o mesmo servidor "
                                    "e, no jogo, o modo ad hoc / multijogador local. É um servidor público de fãs, não o playit: os PSPs conversam "
                                    "direto entre si, então depende do roteador de cada um deixar passar (UPnP ligado). Se não conectar, é o roteador.");
            } else if (mode == 1) {
                if (GetTickCount64() - lanCheckedAt > 10000) {
                    lanAddresses = LanAddresses();
                    lanCheckedAt = GetTickCount64();
                }
                std::string where;
                for (const auto& address : lanAddresses) where += (where.empty() ? "" : "  ou  ") + Utf8(address);
                ImGui::TextDisabled("Este PC liga o servidor do PSP quando o jogo abrir. Os outros da casa escolhem \"entrar no PC de outro\" "
                                    "com o endereço %s. Depois, no jogo, todos entram no modo ad hoc / multijogador local. Se o Windows "
                                    "perguntar, permita a rede privada.", where.empty() ? "deste PC" : where.c_str());
            } else {
                label("Endereço do PC");
                if (pspLanAddress[0] == 0) {
                    const std::string current = PspServerIp(game);
                    current.copy(pspLanAddress.data(), pspLanAddress.size() - 1);
                }
                ImGui::InputTextWithHint("##pspLan", "192.168.0.10", pspLanAddress.data(), pspLanAddress.size());
                if (ImGui::IsItemDeactivatedAfterEdit()) {
                    WritePspServerIp(game, pspLanAddress.data());
                    RememberOnline(L"psp_lan", pspLanAddress.data());
                }
                ImGui::TextDisabled("O endereço que aparece na página do jogo no PC que hospeda (192.168...).");
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::EndDisabled();
        if (running) ImGui::TextDisabled("Feche o jogo para mudar o online.");
    }

    // A server on the PC that hosts (RangaEmu.exe --servidor once), as the Switch's rooms: the classics' table server or
    // the 3DS's room server.
    template <typename Label> void ServerSection(Extra extra, Label& label, float labelWidth, float scale) {
        if (!RoomServerEnabled(library.Root())) return;
        const int k = static_cast<int>(extra);
        const bool classics = extra == Extra::Classics;
        if (GetTickCount64() - serverCheckedAt[k] > 2000) {
            serverRunning[k] = ExtraRunning(library.Root(), extra);
            serverCheckedAt[k] = GetTickCount64();
        }
        ImGui::PushID(k);
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, Color(kText));
        const bool ps3 = extra == Extra::Ps3;
        ImGui::TextUnformatted(classics ? "Sala dos clássicos deste PC (servidor)" : ps3 ? "Servidor do PS3 deste PC (RPCN)" : "Sala do 3DS deste PC (servidor)");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushTextWrapPos(0);
        if (classics)
            ImGui::TextDisabled("Uma sala só serve para todos os jogos clássicos. Precisa de um túnel TCP do playit para 127.0.0.1 nesta porta; "
                                "o endereço do túnel é o que todos (você também, ou 127.0.0.1:porta) põem em \"Endereço da sala\". "
                                "O hub do System Link do Xbox roda junto, na porta UDP %d (túnel UDP do playit para ela).", XboxHubPort(library.Root()));
        else if (ps3)
            ImGui::TextDisabled("O RPCN deste PC, sem e-mail e sem cadastro: cada PS3 cria a própria conta ao entrar. Na mesma casa, todos põem "
                                "o 192.168... deste PC com a porta 31313. Pela internet, os PS3 se ligam direto um no outro, então o túnel do "
                                "playit não serve: o roteador deste PC precisa encaminhar TCP 31313 e UDP 3657 para ele (uma vez, na "
                                "página do roteador). Os jogadores não precisam abrir nada. O endereço, então, é o IP público deste "
                                "PC com :31313.");
        else
            ImGui::TextDisabled("Uma sala serve para todos os jogos de 3DS. Precisa de um túnel UDP do playit para 127.0.0.1 nesta porta; "
                                "o endereço do túnel é o que todos (você também, ou 127.0.0.1:porta) põem em \"Endereço da sala\".");
        ImGui::PopTextWrapPos();
        label("Porta");
        // RPCN's port is the one in its rpcn.cfg (31313).
        ImGui::BeginDisabled(serverRunning[k] || ps3);
        ImGui::InputText("##serverPort", serverPort[k].data(), serverPort[k].size(), ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) SetExtraPort(library.Root(), extra, std::atoi(serverPort[k].data()));
        ImGui::EndDisabled();
        // The home network: the classics' server opens to it on request (restarted if running); the 3DS's room
        // always listens there. Either way the others in the house use this PC's address, without the tunnel.
        bool lan = !classics || ExtraLan(library.Root(), extra);
        if (classics) {
            label("Rede de casa (LAN)");
            if (ImGui::Checkbox("##serverLan", &lan)) {
                SetExtraLan(library.Root(), extra, lan);
                if (serverRunning[k]) {
                    StopExtra(library.Root(), extra);
                    serverProblem[k] = StartExtra(library.Root(), extra);
                    serverCheckedAt[k] = 0;
                }
            }
            ImGui::SameLine();
            ImGui::TextDisabled(lan ? "aceita quem está na mesma rede" : "só este PC e o túnel");
        }
        if (lan) {
            if (GetTickCount64() - lanCheckedAt > 10000) {
                lanAddresses = LanAddresses();
                lanCheckedAt = GetTickCount64();
            }
            if (!lanAddresses.empty()) {
                std::string where;
                for (const auto& address : lanAddresses) where += (where.empty() ? "" : "  ou  ") + Utf8(address) + ":" + std::to_string(ExtraPort(library.Root(), extra));
                ImGui::SetCursorPosX(labelWidth);
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Na mesma casa, os outros põem em \"Endereço da sala\": %s (se o Windows perguntar, permita a rede privada).", where.c_str());
                ImGui::PopTextWrapPos();
            }
        }
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, Color(serverRunning[k] ? kGreen : kDim));
        if (serverRunning[k]) ImGui::Text(classics ? "Ligada na porta %d (TCP)" : "Ligada na porta %d (UDP)", ExtraPort(library.Root(), extra));
        else ImGui::TextUnformatted("Desligada");
        ImGui::PopStyleColor();
        ImGui::SameLine();
        if (ImGui::Button(serverRunning[k] ? "Desligar" : "Ligar")) {
            if (serverRunning[k]) {
                StopExtra(library.Root(), extra);
                serverProblem[k].clear();
            } else {
                serverProblem[k] = StartExtra(library.Root(), extra);
            }
            serverCheckedAt[k] = 0;
        }
        if (!serverProblem[k].empty()) {
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextWrapped("%s", Utf8(serverProblem[k]).c_str());
            ImGui::PopStyleColor();
        }
        if (classics && CanPublishUpdate(library.Root())) {
            ImGui::Dummy(ImVec2(0, 4 * scale));
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kText));
            ImGui::TextUnformatted("Atualização para os amigos");
            ImGui::PopStyleColor();
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            const std::string published = InstalledUpdate(library.Root());
            ImGui::TextDisabled("Publica o Ranga deste PC (programas e núcleos, nunca chaves, BIOS ou jogos), assinado. O Ranga dos amigos acha "
                                "sozinho quando abrir, por esta mesma sala. Última publicada: %s.", published.empty() ? "nenhuma" : published.c_str());
            ImGui::PopTextWrapPos();
            label("O que mudou");
            ImGui::InputText("##publishNotes", publishNotes.data(), publishNotes.size());
            ImGui::SetCursorPosX(labelWidth);
            if (ImGui::Button("Publicar atualização")) {
                pending = [this] {
                    std::string version;
                    const std::wstring error = PublishUpdate(library.Root(), publishNotes.data(), version);
                    publishResult = error.empty() ? L"Publicada a versão " + Wide(version) + L"." : error;
                    if (error.empty()) StartGitHubUpload();
                };
            }
            if (githubBusy) {
                ImGui::SetCursorPosX(labelWidth);
                ImGui::TextDisabled("Mandando para o GitHub (uns minutos)...");
                moving = true;
            } else {
                std::lock_guard guard(updateLock);
                if (!githubResult.empty()) {
                    ImGui::SetCursorPosX(labelWidth);
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextDisabled("%s", Utf8(githubResult).c_str());
                    ImGui::PopTextWrapPos();
                }
            }
            if (!publishResult.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", Utf8(publishResult).c_str());
            }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::BeginDisabled(packageBusy);
            if (ImGui::Button(packageBusy ? "Gerando o pacote..." : "Pacote completo para um amigo")) {
                pending = [this] {
                    Browse(L"Onde salvar o pacote do amigo", L"", true, {}, [this](const std::wstring& folder) { StartFriendPackage(folder); });
                };
            }
            ImGui::EndDisabled();
            if (packageBusy) moving = true;
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushTextWrapPos(0);
            {
                std::lock_guard guard(updateLock);
                if (!packageResult.empty()) ImGui::TextDisabled("%s", Utf8(packageResult).c_str());
                else ImGui::TextDisabled("Para quem não tem o Ranga (PC novo ou formatado): programas, núcleos e emuladores num zip. Sem BIOS, "
                                         "firmware, chaves, jogos, saves, perfil ou as suas chaves de dono.");
            }
            ImGui::PopTextWrapPos();
        }
        ImGui::PopID();
    }

    template <typename Label> void RoomsSection(const Game& game, Label& label, float labelWidth, float scale) {
        // Only on the PC that hosts (RangaEmu.exe --servidor once): friends just join an address.
        if (!RoomServerEnabled(library.Root())) return;
        if (!roomsLoaded) ReloadRooms();
        if (GetTickCount64() - roomsCheckedAt > 2000) {
            for (const Room& room : rooms) roomsRunning[room.id] = RoomRunning(library.Root(), room);
            roomsCheckedAt = GetTickCount64();
        }
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, Color(kText));
        ImGui::TextUnformatted("Salas deste PC (servidor, sempre ligadas)");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("A sala fica aberta sozinha, sem jogo aberto. Todos, inclusive você, usam \"Entrar em uma sala\"; qualquer um cria o grupo no jogo. "
                            "Cada sala precisa de um túnel UDP do playit para 127.0.0.1 na porta dela.");
        ImGui::PopTextWrapPos();
        for (Room& room : rooms) {
            ImGui::PushID(Utf8(room.id).c_str());
            const bool running = roomsRunning[room.id];
            ImGui::Dummy(ImVec2(0, 2 * scale));
            label("Nome");
            ImGui::BeginDisabled(running);
            ImGui::InputText("##name", roomNames[room.id].data(), 63);
            const bool nameDone = ImGui::IsItemDeactivatedAfterEdit();
            label("Porta");
            ImGui::InputText("##port", roomPorts[room.id].data(), 6, ImGuiInputTextFlags_CharsDecimal);
            const bool portDone = ImGui::IsItemDeactivatedAfterEdit();
            label("Senha");
            ImGui::InputText("##password", roomPasswords[room.id].data(), 63);
            const bool passwordDone = ImGui::IsItemDeactivatedAfterEdit();
            label("Até");
            int members = room.members;
            const bool membersDone = ImGui::SliderInt("##members", &members, 2, 16, "%d pessoas");
            ImGui::EndDisabled();
            if (nameDone || portDone || passwordDone || membersDone) {
                room.name = Wide(roomNames[room.id].data());
                room.password = Wide(roomPasswords[room.id].data());
                room.port = std::clamp(std::atoi(roomPorts[room.id].data()), 1, 65535);
                room.members = members;
                SaveRoom(library.Root(), room);
            }
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(running ? kGreen : kDim));
            ImGui::Text(running ? "Ligada na porta %d" : "Desligada", room.port);
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button(running ? "Desligar" : "Ligar")) {
                room.on = !running;
                SaveRoom(library.Root(), room);
                if (running) {
                    StopRoomServer(library.Root(), room);
                    roomsProblem.clear();
                } else {
                    roomsProblem = StartRoomServer(library.Root(), room);
                    if (!roomsProblem.empty()) {
                        room.on = false;
                        SaveRoom(library.Root(), room);
                    }
                }
                roomsCheckedAt = 0;
            }
            ImGui::SameLine();
            // This game joins this room: the owner of the PC enters as everyone else does.
            if (ImGui::Button("Este jogo entra nela")) {
                roomMode = 1;
                const std::string address = "127.0.0.1:" + std::to_string(room.port);
                roomAddress.fill(0);
                address.copy(roomAddress.data(), roomAddress.size() - 1);
                WriteCoreOption(game, "eden_sala", "entrar");
                WriteCoreOption(game, "eden_sala_endereco", address);
                if (!room.password.empty()) {
                    roomPassword.fill(0);
                    Utf8(room.password).copy(roomPassword.data(), roomPassword.size() - 1);
                    WriteCoreOption(game, "eden_sala_senha", Utf8(room.password));
                }
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(running);
            if (ImGui::Button("Remover")) {
                Confirm(L"Remover a sala \"" + room.name + L"\"?", [this, id = room.id] {
                    RemoveRoom(library.Root(), id);
                    ReloadRooms();
                });
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::SetCursorPosX(labelWidth);
        if (ImGui::Button("+  Nova sala")) {
            AddRoom(library.Root());
            ReloadRooms();
        }
        ImGui::SameLine();
        bool startup = RoomsStartWithWindows();
        if (ImGui::Checkbox("Ligar as salas quando o Windows abrir", &startup)) SetRoomsStartWithWindows(library.Root(), startup);
        // Hiding it again: with every room off first, so nothing is left running unseen.
        bool anyRunning = false;
        for (const Room& room : rooms) anyRunning = anyRunning || roomsRunning[room.id];
        ImGui::SameLine();
        ImGui::BeginDisabled(anyRunning);
        if (ImGui::Button("Esconder (este PC não é servidor)")) {
            SetRoomsStartWithWindows(library.Root(), false);
            SetRoomServerEnabled(library.Root(), false);
        }
        ImGui::EndDisabled();
        if (anyRunning && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Desligue as salas antes.");
        if (!roomsProblem.empty()) {
            ImGui::SetCursorPosX(labelWidth);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::PushTextWrapPos(0);
            ImGui::TextUnformatted(Utf8(roomsProblem).c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        }
        // A server room and "Criar uma sala" on the same port cannot both open.
        for (const Room& room : rooms) {
            if (roomMode == 2 && roomsRunning[room.id] && std::atoi(roomPort.data()) == room.port) {
                ImGui::SetCursorPosX(labelWidth);
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                ImGui::PushTextWrapPos(0);
                ImGui::Text("A sala \"%s\" já usa a porta %d: em vez de \"Criar uma sala\", use \"Este jogo entra nela\".", Utf8(room.name).c_str(), room.port);
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
        }
    }

    // The Switch core's graphics options, each with "Automático" first (what the Qualidade level sets). The core
    // applies the filter, its sharpness and the anti-aliasing while the game runs; the rest when it opens again.
    inline static const std::vector<GraphicsOption> kSwitchGraphics = {
        {"Imagem", "eden_resolucao", "Resolução",
         {{"auto", "Automático"}, {"0.5x", "0,5x (360p, PC bem fraco)"}, {"0.75x", "0,75x (540p)"}, {"1x", "1x (720p/1080p, original)"},
          {"1.5x", "1,5x"}, {"2x", "2x (1440p/2160p)"}, {"3x", "3x"}, {"4x", "4x (pesado)"}},
         "Quantas vezes a resolução do Switch. Vale quando o jogo abrir de novo."},
        {"Imagem", "eden_modo", "Modo do console", {{"auto", "Automático (na base)"}, {"base", "Na base (1080p)"}, {"portatil", "Portátil (720p, mais leve)"}},
         "Na base os jogos desenham em 1080p; no portátil, em 720p e mais leve. Vale ao reabrir."},
        {"Imagem", "eden_proporcao", "Proporção da tela",
         {{"auto", "Automático (16:9)"}, {"16:9", "16:9"}, {"4:3", "4:3"}, {"21:9", "21:9 (ultrawide)"}, {"16:10", "16:10"}, {"esticar", "Esticar na janela"}},
         "Formato da imagem. Muda na hora."},
        {"Imagem", "eden_fsr_modo", "Modo do FSR",
         {{"auto", "Automático (pela Qualidade)"}, {"nativo", "Nativo + AA (como o DLAA): 100% + SMAA"}, {"qualidade", "Qualidade: 75%"},
          {"desempenho", "Desempenho: 50%"}, {"ultra", "Ultra desempenho: 25% (borrado)"}},
         "O jogo desenha menor e o AMD FSR aumenta até a sua tela, como nos jogos de PC. O Eden só desenha em 25, 50, 75 ou 100%, por isso não há Equilibrado. Escolher um modo liga o filtro FSR; a resolução muda ao reabrir."},
        {"Imagem", "eden_filtro", "Filtro da imagem",
         {{"auto", "Automático"}, {"fsr", "AMD FSR (mais nítido ao aumentar)"}, {"bilinear", "Bilinear (padrão)"}, {"area", "Area (bom ao diminuir)"},
          {"scaleforce", "ScaleForce"}, {"bicubic", "Bicúbico"}, {"gaussian", "Gaussiano (suave)"}, {"lanczos", "Lanczos"}, {"nearest", "Pixelado"}},
         "Como a imagem é esticada até a sua tela. O FSR da AMD deixa mais nítido quando a resolução está baixa. Muda na hora."},
        {"Imagem", "eden_nitidez_fsr", "Nitidez do FSR",
         {{"auto", "Automático"}, {"0", "0 (máxima)"}, {"25", "25 (padrão)"}, {"50", "50"}, {"75", "75"}, {"100", "100"}, {"150", "150"}, {"200", "200 (mínima)"}},
         "Só com o filtro FSR. Número menor = mais nítido. Muda na hora."},
        {"Imagem", "eden_antialiasing", "Antisserrilhado", {{"auto", "Automático"}, {"nenhum", "Nenhum"}, {"fxaa", "FXAA (leve)"}, {"smaa", "SMAA (melhor, pesa um pouco)"}},
         "Suaviza as bordas serrilhadas. Muda na hora."},
        {"Imagem", "eden_anisotropica", "Filtragem anisotrópica",
         {{"auto", "Automático"}, {"nenhuma", "Nenhuma"}, {"2x", "2x"}, {"4x", "4x"}, {"8x", "8x"}, {"16x", "16x"}},
         "Texturas do chão mais nítidas de longe. Vale ao reabrir."},
        {"Desempenho", "eden_fps", "Limite de FPS",
         {{"auto", "Automático"}, {"jogo", "O do jogo"}, {"30", "30"}, {"60", "60"}, {"90", "90"}, {"120", "120"}},
         "Ritmo em que o emulador entrega quadros. Jogando em sala, deixe igual nos dois PCs. Vale ao reabrir."},
        {"Desempenho", "eden_vsync", "VSync", {{"auto", "Automático"}, {"ligado", "Ligado (sem rasgo)"}, {"desligado", "Desligado (menos atraso)"}, {"mailbox", "Mailbox"}},
         "Sincroniza com o monitor. Vale ao reabrir."},
        {"Desempenho", "eden_precisao_gpu", "Precisão da GPU", {{"auto", "Automático"}, {"baixa", "Baixa (mais rápida)"}, {"media", "Média (padrão)"}, {"alta", "Alta (mais lenta)"}},
         "Baixa ganha FPS; alta corrige efeitos raros. Vale ao reabrir."},
        {"Desempenho", "eden_shaders_async", "Shaders em segundo plano", {{"auto", "Automático"}, {"sim", "Sim (menos travadas)"}, {"nao", "Não"}},
         "Sim tira as travadas da primeira vez que um efeito aparece. Vale ao reabrir."},
        {"Desempenho", "eden_gpu_rapida", "GPU emulada acelerada", {{"auto", "Automático"}, {"normal", "Normal"}, {"media", "Média"}, {"alta", "Alta"}},
         "Faz o jogo achar que a GPU do Switch é mais rápida; ajuda alguns jogos a ficarem em 60. Vale ao reabrir."},
        {"Avançado", "eden_cache_shaders", "Guardar shaders no disco", {{"auto", "Automático (sim)"}, {"sim", "Sim"}, {"nao", "Não"}},
         "Guarda os shaders prontos para a próxima vez. Vale ao reabrir."},
        {"Avançado", "eden_video", "Vídeos do jogo", {{"auto", "Automático (placa de vídeo)"}, {"gpu", "Na placa de vídeo"}, {"cpu", "No processador"}, {"desligado", "Desligados"}},
         "Cenas em vídeo. Se travarem, tente no processador. Vale ao reabrir."},
    };

    template <typename Label> void SwitchGraphics(const Game& game, Label& label, float labelWidth, float scale) {
        GraphicsSection(game, kSwitchGraphics, "Gráficos do Switch (só este jogo)",
                        "Automático segue a Qualidade acima. Filtro, nitidez e antisserrilhado também mudam no menu do jogo (Esc ou botão PS/Guide), na hora; o resto vale ao reabrir.",
                        label, labelWidth, scale);
    }

    // A console's graphics options on the game page, in groups, each a list of the values the core takes.
    template <typename Label> void GraphicsSection(const Game& game, const std::vector<GraphicsOption>& options, const char* title, const char* closing, Label& label,
                                                   float labelWidth, float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushStyleColor(ImGuiCol_Text, Color(kText));
        ImGui::TextUnformatted(title);
        ImGui::PopStyleColor();
        const char* group = "";
        for (const auto& option : options) {
            if (std::strcmp(group, option.group) != 0) {
                group = option.group;
                ImGui::SetCursorPosX(labelWidth);
                ImGui::TextDisabled("%s", group);
            }
            int current = 0;
            for (size_t i = 0; i < option.values.size(); ++i) {
                if (graphics[option.key] == option.values[i].first) current = static_cast<int>(i);
            }
            label(option.label);
            ImGui::PushID(option.key);
            if (ImGui::BeginCombo("##g", option.values[static_cast<size_t>(current)].second)) {
                for (size_t i = 0; i < option.values.size(); ++i) {
                    if (ImGui::Selectable(option.values[i].second, static_cast<int>(i) == current)) {
                        graphics[option.key] = option.values[i].first;
                        WriteCoreOption(game, option.key, option.values[i].first);
                        // The FSR mode and the resolution both set the size the game draws at: picking one puts the
                        // other back on Automático, so the screen never shows a choice that is being ignored.
                        const std::string key = option.key, picked = option.values[i].first;
                        const char* other = key == "eden_fsr_modo" ? "eden_resolucao" : key == "eden_resolucao" ? "eden_fsr_modo" : nullptr;
                        if (other != nullptr && picked != "auto" && graphics[other] != "auto" && !graphics[other].empty()) {
                            graphics[other] = "auto";
                            WriteCoreOption(game, other, "auto");
                        }
                    }
                }
                ImGui::EndCombo();
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", option.note);
            ImGui::PopID();
        }
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("%s", closing);
        ImGui::PopTextWrapPos();
    }

    // The buttons of a console Ranga runs itself, in the order of the console's file (Ranga Retro's RetroPad names), the
    // default pad button for each, and how the tab names them.
    static constexpr const char* kRetroButtons[16] = {"B", "Y", "SELECT", "START", "UP", "DOWN", "LEFT", "RIGHT", "A", "X", "L", "R", "L2", "R2", "L3", "R3"};
    static constexpr const char* kPadDefaults[16] = {"a", "x", "back", "start", "dpup", "dpdown", "dpleft", "dpright", "b", "y",
                                                     "leftshoulder", "rightshoulder", "lefttrigger", "righttrigger", "leftstick", "rightstick"};
    // The order on screen: face buttons, then shoulders and triggers, sticks, Start/Select, d-pad.
    static constexpr int kControlOrder[16] = {8, 0, 9, 1, 10, 11, 12, 13, 14, 15, 3, 2, 4, 5, 6, 7};

    static const char* ConsoleButtonName(const std::wstring& system, int retro) {
        // The console's own names where they differ from the RetroPad's; by position, as the cores read them.
        const bool nintendo = system == L"SWITCH" || system == L"WIIU" || system == L"SNES" || system == L"3DS" || system == L"DS" || system == L"GC" || system == L"WII";
        const bool sony = system == L"PS1" || system == L"PS2" || system == L"PS3" || system == L"PSP";
        static const char* const generic[16] = {"B (baixo)", "Y (esquerda)", "Select", "Start", "Direcional ↑", "Direcional ↓", "Direcional ←", "Direcional →",
                                                "A (direita)", "X (cima)", "L", "R", "L2", "R2", "L3 (aperta o analógico esq.)", "R3 (aperta o analógico dir.)"};
        static const char* const switchNames[16] = {"B (baixo)", "Y (esquerda)", "− (menos)", "+ (mais)", "Direcional ↑", "Direcional ↓", "Direcional ←", "Direcional →",
                                                    "A (direita)", "X (cima)", "L", "R", "ZL", "ZR", "L3 (aperta o analógico esq.)", "R3 (aperta o analógico dir.)"};
        static const char* const sonyNames[16] = {"Cruz (baixo)", "Quadrado (esquerda)", "Select", "Start", "Direcional ↑", "Direcional ↓", "Direcional ←", "Direcional →",
                                                  "Bola (direita)", "Triângulo (cima)", "L1", "R1", "L2", "R2", "L3 (aperta o analógico esq.)", "R3 (aperta o analógico dir.)"};
        (void)nintendo;
        return (system == L"SWITCH" ? switchNames : sony ? sonyNames : generic)[retro];
    }

    static std::string PadButtonName(const std::string& sdl) {
        static const std::pair<const char*, const char*> names[] = {
            {"a", "A / Cruz (baixo)"}, {"b", "B / Bola (direita)"}, {"x", "X / Quadrado (esquerda)"}, {"y", "Y / Triângulo (cima)"},
            {"back", "Select / Share"}, {"start", "Start / Options"}, {"leftshoulder", "LB / L1"}, {"rightshoulder", "RB / R1"},
            {"lefttrigger", "LT / L2"}, {"righttrigger", "RT / R2"}, {"leftstick", "L3"}, {"rightstick", "R3"},
            {"dpup", "Direcional ↑"}, {"dpdown", "Direcional ↓"}, {"dpleft", "Direcional ←"}, {"dpright", "Direcional →"},
            {"touchpad", "Touchpad"}, {"misc1", "Botão extra (Mic / Share)"}, {"paddle1", "Pá traseira 1"}, {"paddle2", "Pá traseira 2"},
            {"paddle3", "Pá traseira 3"}, {"paddle4", "Pá traseira 4"},
        };
        if (sdl.empty()) return "nenhum";
        for (const auto& [id, label] : names) {
            if (sdl == id) return label;
        }
        return sdl;
    }

    void LoadControls(const std::wstring& system) {
        controlsSystem = system;
        for (int b = 0; b < 16; ++b) controls[static_cast<size_t>(b)] = kPadDefaults[b];
        const std::string text = ReadBytes(ControlsFile(library.Root(), system));
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
                if (key == kRetroButtons[b]) controls[static_cast<size_t>(b)] = value;
            }
        }
    }

    // Writes only what differs from the default; no difference, no file.
    void SaveControls() {
        std::string out = "# Botões do controle no Ranga (aba Controle). Botão do console = botão do controle, nomes do SDL.\r\n";
        bool any = false;
        for (int b = 0; b < 16; ++b) {
            if (controls[static_cast<size_t>(b)] == kPadDefaults[b]) continue;
            out += std::string(kRetroButtons[b]) + " = " + controls[static_cast<size_t>(b)] + "\r\n";
            any = true;
        }
        const std::wstring path = ControlsFile(library.Root(), controlsSystem);
        if (!any) {
            DeleteFileW(path.c_str());
            return;
        }
        CreateDirectoryW((library.Root() + L"\\controles").c_str(), nullptr);
        FILE* file = nullptr;
        if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return;
        std::fwrite(out.data(), 1, out.size(), file);
        std::fclose(file);
    }

    // The console's buttons, each with the controller button that plays it and Mudar, which waits for the next press.
    // It holds for every game of the console that runs inside Ranga.
    void ControlsTab(const Game& game, float scale) {
        if (controlsSystem != game.system) LoadControls(game.system);
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Vale para todos os jogos de %s que rodam dentro do Ranga, a partir da próxima vez que o jogo abrir. O teclado do PC continua com as teclas de sempre.",
                            SystemName(game.system).c_str());
        ImGui::PopTextWrapPos();
        if (!gamepads.Connected()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextUnformatted("Nenhum controle ligado: ligue um para poder trocar os botões.");
            ImGui::PopStyleColor();
        }
        ImGui::Dummy(ImVec2(0, 4 * scale));

        // Waiting for a press. Every button is let go first (the one that clicked Mudar among them), then the next press
        // is the new button. After it, nothing on the page reacts until it is let go too: the same press would click
        // Mudar again and start another wait, which is what the gamepad's Cross / A did.
        const auto down = gamepads.ButtonsDown();
        if (captureFor >= 0) {
            if (!captureArmed) {
                captureArmed = down.empty();
            } else if (!down.empty()) {
                controls[static_cast<size_t>(captureFor)] = down.front();
                SaveControls();
                captureFor = -1;
                captureCooling = true;
            }
            if (captureFor >= 0 && (ImGui::GetTime() > captureUntil || ImGui::IsKeyPressed(ImGuiKey_Escape, false))) {
                captureFor = -1;
                captureCooling = !down.empty();
            }
        } else if (captureCooling && down.empty()) {
            captureCooling = false;
        }
        const bool busy = captureFor >= 0 || captureCooling;

        const float labelWidth = 260 * scale, valueWidth = 230 * scale;
        for (const int b : kControlOrder) {
            ImGui::PushID(b);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(ConsoleButtonName(game.system, b));
            ImGui::SameLine(labelWidth);
            const bool waiting = captureFor == b;
            const bool changed = controls[static_cast<size_t>(b)] != kPadDefaults[b];
            if (waiting) {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                if (!captureArmed) ImGui::TextUnformatted("solte os botões...");
                else ImGui::Text("aperte o botão novo... (%d s)", std::max(0, static_cast<int>(captureUntil - ImGui::GetTime()) + 1));
                ImGui::PopStyleColor();
            } else {
                if (changed) ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
                ImGui::TextUnformatted(PadButtonName(controls[static_cast<size_t>(b)]).c_str());
                if (changed) ImGui::PopStyleColor();
            }
            ImGui::SameLine(labelWidth + valueWidth);
            ImGui::BeginDisabled(busy || !gamepads.Connected());
            if (ImGui::Button("Mudar")) {
                captureFor = b;
                captureArmed = false;
                captureUntil = ImGui::GetTime() + 8;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::BeginDisabled(busy || controls[static_cast<size_t>(b)].empty());
            if (ImGui::Button("Nenhum")) {
                controls[static_cast<size_t>(b)].clear();
                SaveControls();
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::BeginDisabled(busy);
        if (ImGui::Button("Voltar ao padrão")) {
            for (int b = 0; b < 16; ++b) controls[static_cast<size_t>(b)] = kPadDefaults[b];
            SaveControls();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::TextDisabled("Esc cancela a espera. O botão Guide / PS fica com o menu do Ranga.");
    }

    // A Switch game's updates and add-ons. Eden reads the files where they are when the game starts (nothing is
    // installed), so the list only changes while the game is closed.
    void ContentTab(const Game& game, float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        if (ImGui::Button("+  Adicionar atualização ou DLC...")) {
            Browse(L"Escolha o arquivo da atualização ou da DLC", ParentFolder(game.folder), false, {{L"Atualização ou DLC do Switch", L"*.nsp;*.xci"}},
                   [this](const std::wstring& file) { AddContentFile(file); });
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("%s", running ? "Feche o jogo para mudar a lista." : "Vale na próxima vez que o jogo abrir.");
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("O Ranga lê cada arquivo de onde ele está, sem copiar nem instalar: não mova nem apague. Atualização e DLC colocadas na pasta de jogos do Switch entram aqui sozinhas.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 4 * scale));
        if (contentFiles.empty()) {
            ImGui::TextDisabled("Nenhuma atualização nem DLC.");
            return;
        }
        for (size_t i = 0; i < contentFiles.size(); ++i) {
            const SwitchContent& content = contentKinds[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::Separator();
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 0.0f);
            ImGui::TextUnformatted(Utf8(FileName(contentFiles[i])).c_str());
            ImGui::PopFont();
            const char* kind = content.kind == SwitchKind::Update ? "Atualização" : content.kind == SwitchKind::Addon ? "DLC" : "Tipo não identificado";
            if (content.titleId.empty()) ImGui::TextDisabled("%s", kind);
            else ImGui::TextDisabled("%s   ·   %s", kind, Utf8(content.titleId).c_str());
            if (!Exists(contentFiles[i])) {
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kRed));
                ImGui::TextUnformatted("arquivo não encontrado");
                ImGui::PopStyleColor();
            }
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetContentRegionMax().x - 90 * scale);
            ImGui::BeginDisabled(running);
            // Only the list changes; the file stays where it is.
            if (ImGui::Button("Remover", ImVec2(90 * scale, 0))) {
                pending = [this, i] {
                    Game* current = Current();
                    if (current == nullptr || i >= contentFiles.size()) return;
                    contentFiles.erase(contentFiles.begin() + static_cast<ptrdiff_t>(i));
                    SaveContent(*current);
                };
            }
            ImGui::EndDisabled();
            ImGui::PopID();
        }
    }

    void DlcTab(const Game& game, float scale) {
        ImGui::Dummy(ImVec2(0, 4 * scale));
        // The runtime reads the installed add-ons when the game starts, so they change only while it is closed.
        const bool running = runner.Busy() && runner.TitleId() == game.titleId;
        ImGui::BeginDisabled(running);
        if (ImGui::Button("+  Instalar DLC...")) {
            Browse(L"Escolha a pasta da DLC (a que tem sce_sys\\param.json)", game.folder, true, {}, [this](const std::wstring& folder) {
                Game* current = Current();
                if (current == nullptr) return;
                const std::wstring error = InstallAddon(*current, folder);
                if (!error.empty()) Notify(error);
                addons = LoadAddons(*current);
            });
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

    // Relatório de erro: the game's own logs, the emulator's log for its console and a short summary (Ranga's version,
    // the game, how its last run ended) in one .zip under relatorios\, which Explorer then shows selected. Nothing
    // personal goes in: no saves, keys, BIOS or games, only text logs.
    void MakeErrorReport(const Game& game) {
        const std::wstring root = library.Root();
        SYSTEMTIME now;
        GetLocalTime(&now);
        wchar_t stamp[32];
        std::swprintf(stamp, 32, L"%04d%02d%02d-%02d%02d%02d", now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond);
        std::wstring name;
        for (const wchar_t c : game.title) name += (iswalnum(c) || c == L' ' || c == L'-') ? c : L'_';
        const std::wstring folder = root + L"\\relatorios\\" + name + L" " + stamp;
        CreateDirectoryW((root + L"\\relatorios").c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        const auto copy = [&](const std::wstring& from, const std::wstring& as) {
            if (Exists(from)) CopyFileW(from.c_str(), (folder + L"\\" + as).c_str(), FALSE);
        };
        for (const wchar_t* log : {L"game.log", L"game.log.last-error", L"game.err", L"launcher.log", L"prepare.log", L"dlss5.log"})
            copy(game.workspace + L"\\logs\\" + log, log);
        if (game.system == L"PS3") copy(root + L"\\bios\\retroarch\\rpcs3\\log\\RPCS3.log", L"RPCS3.log");
        if (game.system == L"SWITCH") copy(root + L"\\bios\\retroarch\\eden\\log\\eden_log.txt", L"eden_log.txt");
        std::string summary = "Ranga " + InstalledVersionName(root) + " (" + InstalledUpdate(root) + ")\r\n";
        summary += "Jogo: " + Utf8(game.title) + "\r\nConsole: " + SystemName(game.system) + "\r\n";
        if (!game.serial.empty()) summary += "Serial: " + Utf8(game.serial) + "\r\n";
        summary += "Emulador: " + HowItRuns(game.system) + "\r\n";
        // The launcher log's last line says how the game ended ("ended, exit code ...").
        const std::string launcherLog = ReadBytes(game.workspace + L"\\logs\\launcher.log");
        if (const size_t ended = launcherLog.rfind("ended,"); ended != std::string::npos) {
            const size_t lineStart = launcherLog.rfind('\n', ended);
            const size_t lineEnd = launcherLog.find('\n', ended);
            summary += "Última vez: " + launcherLog.substr(lineStart == std::string::npos ? 0 : lineStart + 1, (lineEnd == std::string::npos ? launcherLog.size() : lineEnd) - (lineStart == std::string::npos ? 0 : lineStart + 1)) + "\r\n";
        }
        if (FILE* file = _wfopen((folder + L"\\resumo.txt").c_str(), L"wb")) {
            std::fwrite(summary.data(), 1, summary.size(), file);
            std::fclose(file);
        }
        // Windows' own tar makes the .zip (-a picks the format by the name).
        const std::wstring zip = folder + L".zip";
        RunHidden(L"tar.exe -a -cf \"" + zip + L"\" -C \"" + folder + L"\" .", root + L"\\relatorios", L"");
        if (Exists(zip)) {
            std::error_code ignored;
            std::filesystem::remove_all(folder, ignored);
            ShellExecuteW(window, L"open", L"explorer.exe", (L"/select,\"" + zip + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            Notify(L"Relatório pronto: " + zip + L"\nMande esse .zip para quem for ajudar.");
        } else {
            ShellExecuteW(window, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            Notify(L"Os logs estão em " + folder + L" (não deu para fazer o .zip).");
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
        if (ImGui::Button("Relatório de erro")) pending = [this, game] { MakeErrorReport(game); };
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Junta num .zip os logs deste jogo e do emulador, para mandar a quem for ajudar.");
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
        ImGui::TextDisabled("Cada console roda no emulador que você escolher aqui. O Ranga não baixa nenhum: baixe do site oficial, extraia onde quiser e aponte o .exe. BIOS, firmware e chaves vêm do seu próprio console.");
        ImGui::Dummy(ImVec2(0, 6 * scale));
        // The page opens with the focus on Voltar, so the gamepad has a place to move from.
        if (focusEngines) {
            ImGui::SetKeyboardFocusHere();
            focusEngines = false;
            ShowRing();
        }
        if (ImGui::Button("Voltar")) enginesPage = false;
        ImGui::Dummy(ImVec2(0, 4 * scale));
        bool filesTab = false, backupTab = false;
        if (ImGui::BeginTabBar("##enginesTabs")) {
            if (ImGui::BeginTabItem("Emuladores")) ImGui::EndTabItem();
            if (ImGui::BeginTabItem("Arquivos do Ranga")) {
                filesTab = true;
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Backup e nuvem", nullptr, openBackupTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                backupTab = true;
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        openBackupTab = false;
        if (filesTab || backupTab) {
            if (filesTab) IntegrityTab(scale);
            else BackupTab(scale);
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, 20 * scale));
            ImGui::EndChild();
            return;
        }
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
                    Browse(L"Escolha o .exe do emulador de " + Wide(system.name), start, false, {{L"Programa", L"*.exe"}},
                           [this, id = system.id](const std::wstring& exe) {
                               ChooseEngine(library.Root(), id, exe);
                               RefreshEngines();
                           });
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

    // The room a game will join when it opens, to test first: its address, the port when the address has none, and
    // whether it is the classics' server (TCP). False when the game plays offline, or when this PC hosts that room
    // (the game then joins it here, at 127.0.0.1: Runner).
    bool RoomToCheck(const Game& game, std::string& address, int& port, bool& tcp) {
        tcp = false;
        if (game.system == L"SWITCH" && ReadCoreOption(game, "eden_sala") == "entrar") {
            address = ReadCoreOption(game, "eden_sala_endereco");
            port = std::atoi(ReadCoreOption(game, "eden_sala_porta").c_str());
            if (port <= 0) port = 24872;
            if (RoomServerEnabled(library.Root())) {
                for (const Room& room : LoadRooms(library.Root())) if (RoomRunning(library.Root(), room)) return false;
            }
        } else if (game.system == L"3DS" && ReadCoreOption(game, "citra_sala") == "entrar") {
            address = ReadCoreOption(game, "citra_sala_endereco");
            port = 24874;
        } else if (ClassicOnline(game.system) && ReadCoreOption(game, "ranga_online") == "ligado") {
            address = ReadCoreOption(game, "ranga_online_endereco");
            port = 24873;
            tcp = true;
        } else {
            return false;
        }
        return !address.empty();
    }

    std::atomic<bool> playChecking{false}; // the room is being tested before the game opens

    void Play(Game& game) {
        // With a room to join, it is asked first (3 s at most, in the background): a room that does not answer is said
        // before the game opens, instead of the game finding out in the middle of a race.
        // (A press can reach here twice in a frame, from the button and from the pad; the second waits for the test.)
        if (playChecking) return;
        std::string address;
        int port = 0;
        bool tcp = false;
        if (RoomToCheck(game, address, port, tcp)) {
            playChecking = true;
            std::thread([this, id = game.titleId, address, port, tcp] {
                const bool answered = ProbeRoom(address, port, tcp) >= 0;
                std::lock_guard guard(probeLock);
                checkedPlay = {id, answered ? 1 : 0, address};
            }).detach();
            return;
        }
        PlayNow(game);
    }

    // The answer of the test above, picked up on the drawing thread (TakePlayCheck).
    struct PlayCheck {
        std::wstring titleId;
        int answered = -1; // -1: nothing waiting
        std::string address;
    } checkedPlay;

    void TakePlayCheck() {
        PlayCheck check;
        {
            std::lock_guard guard(probeLock);
            if (checkedPlay.answered < 0) return;
            check = std::exchange(checkedPlay, PlayCheck{});
        }
        playChecking = false;
        for (size_t i = 0; i < library.Games().size(); ++i) {
            if (library.Games()[i].titleId != check.titleId) continue;
            if (check.answered == 1) {
                PlayNow(library.Games()[i]);
            } else {
                Confirm(L"A sala " + Wide(check.address) + L" não respondeu: o online não vai funcionar agora (servidor desligado, "
                        L"túnel do playit fora ou endereço errado).\n\nJogar mesmo assim?",
                        [this, id = check.titleId] {
                            for (auto& game : library.Games()) if (game.titleId == id) PlayNow(game);
                        });
            }
            return;
        }
    }

    void PlayNow(Game& game) {
        Save();
        RefreshEngines();
        // For the profile: online or not, and what the friends see as being played.
        playOnline = OnlineChosen(game);
        playingTitle = Utf8(game.title);
        playingSystem = SystemName(game.system);
        // Playing online, the card carries the way in ("Entrar no jogo" for the friends).
        const Invite invite = InviteFor(game);
        playingInvite = playOnline && !invite.mode.empty() ? InviteText(invite) : "";
        // For Ranga Retro: where F12 keeps the captures, and the invite its menu sends to friends.
        SetEnvironmentVariableW(L"RANGA_CAPTURAS", CapturesFolder(game).c_str());
        SetEnvironmentVariableW(L"RANGA_CONVITE", Wide(InviteText(invite)).c_str());
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
            // No wide art of its own: a plain band, with the box art standing on the right.
            draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + bannerHeight), Packed(kSidebar));
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
        if (const std::string played = PlayedTime(*game); !played.empty()) meta += "   ·   " + played + " jogadas";
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
        ImGui::BeginDisabled(busy || playChecking);
        const char* playLabel = playChecking ? "Testando a sala..." : !busy ? "Jogar" : runner.Current() == Runner::State::Preparing ? "Preparando..." : "Rodando...";
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
        {
            // Favorite: it comes first on its console's page, with a star.
            const bool favorite = IsFavorite(*game);
            ImGui::SameLine(0, 10 * scale);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            if (ImGui::Button(favorite ? "      Favorito###favorito" : "      Favoritar###favorito", ImVec2(0, 44 * scale))) SetFavorite(*game, !favorite);
            DrawStar(ImGui::GetWindowDrawList(), ImVec2(at.x + 20 * scale, at.y + 22 * scale), 9 * scale, favorite ? Packed(kAmber) : Packed(kDim));
        }
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
        } else if (game->system == L"PS3" && EngineFor(L"PS3").native && !Ps3Compiled(*game)) {
            // The first boot of a PS3 game compiles its code for this processor (the PS3 core marks the end in the
            // cache, MarkCompiled): closing it before that throws the work away and it starts over.
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::PushTextWrapPos(width - pad);
            ImGui::TextWrapped("Primeira vez: o jogo é preparado para o seu processador (pode levar uns 10 minutos, com a CPU no máximo). "
                               "Espere a barra \"Compilando os módulos PPU\" terminar; se fechar antes, começa do zero.");
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
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
            if (External(*game) && EngineFor(game->system).native && ImGui::BeginTabItem("Controle")) {
                ImGui::BeginChild("##controls", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                ControlsTab(*game, scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            } else if (captureFor >= 0 && controlsSystem == game->system) {
                // The tab was left while it waited.
                captureFor = -1;
            }
            if (game->system == L"SWITCH" && EngineFor(game->system).native && ImGui::BeginTabItem("Atualizações e DLC")) {
                ImGui::BeginChild("##content", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                ContentTab(*game, scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (EngineFor(game->system).native && ImGui::BeginTabItem("Capturas")) {
                ImGui::BeginChild("##captures", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                ImGui::Dummy(ImVec2(0, 6 * scale));
                CapturesSection(*game, scale);
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

    // ---- The profile, Steam-style (PlayerProfile.hpp) ----
    PlayerProfile profile;
    bool profileLoaded = false, profileOpen = false, profileEdit = false;
    // Whose profile the page shows: a friend's id, or empty for this player's own.
    std::string profileViewing;
    // Games that closed (told by the runner's thread), waiting to be counted on the window's thread.
    struct Ended {
        Game game;
        long long started = 0, seconds = 0;
        bool online = false;
        unsigned long code = 0; // how the game's process ended
    };
    std::mutex endedLock;
    std::vector<Ended> endedQueue;
    std::atomic<bool> playOnline{false};
    std::string playingTitle, playingSystem, playingInvite;
    struct Toast {
        std::string title, text, badge;
        double until = 0, start = 0;
        std::string friendId;          // a friend's notice shows their photo
        std::function<void()> action;  // what a click on it does (open the chat, join the game)
    };
    std::vector<Toast> toasts;
    // The friends (the central's /amigos, PlayerProfile.hpp ReadFriends): their cards, the requests received and sent,
    // and what went wrong. Fetched in the background with this player's card (SyncProfiles).
    std::mutex friendsLock;
    std::vector<PlayerCard> friends;
    std::vector<std::pair<std::string, std::string>> friendsIncoming, friendsOutgoing, friendsBlocked;
    // The chat groups this account is in (the central's /grupos), under friendsLock. A group's conversation is keyed
    // "g:<id>" wherever a friend's is keyed by the friend's id.
    struct ChatGroup {
        std::string id, name, owner;
        std::vector<std::pair<std::string, std::string>> members; // account, Nome#1234
    };
    std::vector<ChatGroup> groups;
    static std::vector<ChatGroup> ReadGroups(const std::string& text) {
        std::vector<ChatGroup> out;
        for (size_t at = 0; at < text.size();) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(at, end - at);
            at = end + 1;
            std::vector<std::string> parts;
            for (size_t from = 0;;) {
                const size_t tab = line.find('\t', from);
                parts.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                if (tab == std::string::npos) break;
                from = tab + 1;
            }
            if (parts.size() == 4 && parts[0] == "grupo") out.push_back({parts[1], parts[2], parts[3], {}});
            if (parts.size() == 4 && parts[0] == "membro") {
                for (auto& group : out) if (group.id == parts[1]) group.members.emplace_back(parts[2], parts[3]);
            }
        }
        return out;
    }
    static bool IsGroup(const std::string& key) { return key.rfind("g:", 0) == 0; }
    std::string friendsProblem;
    long long friendsAt = 0;
    std::atomic<bool> sessionExpired{false};
    // The friend requests already shown (a new one gets a notice).
    std::set<std::string> incomingSeen;
    bool incomingPrimed = false;
    std::array<char, 160> centralField{};
    std::array<char, 64> friendAddField{};
    // One friend action at a time (ask, accept, refuse, cancel, remove), in the background.
    std::thread actionThread;
    std::atomic<bool> actionBusy{false};
    std::mutex actionLock;
    std::string actionResult, friendAction, actionPath;
    bool actionDone = false;
    // The conversations: every message to or from this account, fetched in the background; read up to which message
    // with each friend, and the friend whose conversation is open.
    struct DirectMessage {
        long long number = 0, when = 0;
        std::string from, to, text;
    };
    std::mutex chatLock;
    std::vector<DirectMessage> chat;
    long long chatLast = 0, chatNoticed = 0, groupLast = 0, groupNoticed = 0;
    std::atomic<bool> chatFetched{false};
    bool chatPrimed = false, openChatTab = false;
    std::map<std::string, long long> chatSeen, chatSeenSaved;
    std::string chatProblem, chatPeer;
    std::thread chatThread;
    std::atomic<bool> chatBusy{false};
    ULONGLONG chatAt = 0;
    int chatUnread = 0;
    size_t chatShown = 0;
    bool chatTabOpen = false, chatScroll = false;
    std::array<char, 501> chatInput{};
    // Entrar ou criar conta: the window, what is typed, and the answer from the central.
    bool accountOpen = false;
    int accountMode = 0; // 0 entrar, 1 criar, 2 recuperar
    std::array<char, 64> accName{}, accCode{};
    std::array<char, 129> accPass{}, accPass2{};
    std::thread accountThread;
    std::atomic<bool> accountBusy{false};
    std::mutex accountLock;
    AccountAnswer accountAnswer;
    bool accountDone = false;
    std::string accountProblem, accountRecovery;
    std::atomic<int> accountStage{0}; // 1 while the PC solves the proof of work
    // Dono do Ranga (only with salas\\chave-dono.txt): the accounts found, and banning or unbanning one.
    struct OwnerRow {
        std::string id, name, reason;
        long long created = 0, seen = 0;
        bool banned = false;
    };
    bool ownerHere = false;
    std::array<char, 64> ownerSearch{};
    std::array<char, 128> ownerReason{};
    std::mutex ownerLock;
    std::vector<OwnerRow> ownerRows;
    // The open reports (the central's /dono/denuncias): who, against whom, why, and the messages before.
    struct OwnerReport {
        std::string id, from, against, againstName, reason, excerpt;
        long long when = 0;
    };
    std::vector<OwnerReport> ownerReports;
    std::string ownerProblem;
    std::thread ownerThread;
    std::atomic<bool> ownerBusy{false};
    std::thread profileThread;
    std::atomic<bool> profileBusy{false};
    ULONGLONG profileSyncedAt = 0, friendsPolledAt = 0;
    // Minimized or covered: the central is still asked, only less often.
    bool background = false;
    uint64_t sentRevision = 0;
    std::string sentPlaying;
    // Pictures: this player's photo, the friends' (by id), and RetroAchievements' badges, downloaded once to
    // perfil\insignias.
    // Photos and covers, animated ("RANI", Media.hpp): one texture per frame. This player's come from the profile's
    // bytes; the friends' from the central, kept in perfil\\amigos by version.
    struct AnimTexture {
        std::vector<Texture> frames;
        std::vector<int> ends; // when each frame ends, in ms from the start
        int total = 1;
        void Clear() {
            for (auto& frame : frames) if (frame.view != nullptr) frame.view->Release();
            frames.clear();
            ends.clear();
            total = 1;
        }
        const Texture* Now() const {
            if (frames.empty()) return nullptr;
            const int at = static_cast<int>(GetTickCount64() % static_cast<ULONGLONG>(total));
            for (size_t i = 0; i < frames.size(); ++i) if (at < ends[i]) return &frames[i];
            return &frames.back();
        }
    };
    struct CachedMedia {
        std::string version;
        AnimTexture anim;
        double usedAt = 0; // last frame it was drawn; friends' media nobody looks at for a while leaves the video memory
    };
    // Friends' media decoded by the media thread, waiting for the window thread to turn them into textures.
    struct DecodedMedia {
        std::string key, version;
        Media media;
    };
    std::vector<DecodedMedia> mediaReady;
    std::map<std::string, CachedMedia> mediaCache;
    AnimTexture ownPhoto, ownCover;
    std::string ownPhotoHash = "-", ownCoverHash = "-";
    std::mutex mediaLock;
    std::vector<std::array<std::string, 3>> mediaQueue; // account, kind, version
    std::set<std::string> mediaAsked;
    std::thread mediaThread;
    std::atomic<bool> mediaBusy{false};
    // Editar perfil: a picture, GIF or video turned into a photo or cover in the background, and the central's word
    // that it has the latest ones.
    std::thread importThread;
    std::atomic<bool> importBusy{false};
    std::mutex importLock;
    std::string importResult, importError;
    bool importCover = false, importDone = false;
    std::string sentPhotoHash, sentCoverHash;
    bool sentMediaDone = false;
    std::map<std::string, Texture> badgeTextures;
    std::set<std::string> badgeMissing, badgeAsked;
    std::mutex badgeLock;
    std::vector<std::string> badgeQueue;
    std::thread badgeThread;
    std::atomic<bool> badgeBusy{false};
    std::atomic<int> badgeDone{0};
    int badgeSeen = 0;
    // Editar perfil, and the RetroAchievements sign-in running in the background.
    std::array<char, 64> editName{};
    std::array<char, 160> editPhrase{};
    std::array<char, 64> raUserField{};
    std::array<char, 128> raPasswordField{};
    std::thread raThread;
    std::atomic<bool> raBusy{false};
    std::mutex raLock;
    std::string raProblem, raTokenNew, raUserNew;
    bool raDone = false;

    void StopProfileThreads() {
        if (lobbyThread.joinable()) lobbyThread.join();
        if (mediaThread.joinable()) mediaThread.join();
        if (importThread.joinable()) importThread.join();
        if (ownerThread.joinable()) ownerThread.join();
        if (profileThread.joinable()) profileThread.join();
        if (chatThread.joinable()) chatThread.join();
        if (actionThread.joinable()) actionThread.join();
        if (accountThread.joinable()) accountThread.join();
        // The friends see this Ranga closed at once, not five minutes later.
        if (profileLoaded && profileSyncedAt != 0 && !profile.session.empty()) {
            PlayerCard card = profile.Card("", "");
            card.closed = true;
            CentralCall(library.Root(), "POST", "/perfil", WriteCard(card), profile.session, 2000);
        }
        if (badgeThread.joinable()) badgeThread.join();
        if (raThread.joinable()) raThread.join();
    }
    int LibraryCount() { return static_cast<int>(library.Games().size()); }

    void InitProfile() {
        profileLoaded = true;
        profile.Load(library.Root());
        runner.ended = [this](const Game& game, long long started, long long seconds, unsigned long code) {
            std::lock_guard guard(endedLock);
            endedQueue.push_back({game, started, seconds, playOnline.load(), code});
        };
        ScanProfileRa();
        for (const auto& title : profile.Evaluate(LibraryCount())) AddToast("Conquista desbloqueada", title, "");
    }

    void ScanProfileRa() {
        std::vector<std::pair<std::wstring, std::wstring>> spaces;
        std::map<std::wstring, std::pair<std::wstring, std::wstring>> info;
        for (const auto& game : library.Games()) {
            spaces.push_back({game.titleId, game.workspace});
            info[game.titleId] = {game.system, game.title};
        }
        profile.ScanRetroAchievements(spaces, info);
    }

    void AddToast(const std::string& title, const std::string& text, const std::string& badge, const std::string& friendId = "",
                  std::function<void()> action = nullptr, bool flash = true) {
        // Minimized or covered, no frames run and ImGui's clock stands still, then jumps on coming back: such a
        // notice starts its seven seconds when it is first drawn, and the taskbar button blinks meanwhile.
        if (background) {
            toasts.push_back({title, text, badge, 0.0, 0.0, friendId, std::move(action)});
            if (flash) {
                FLASHWINFO blink{sizeof(FLASHWINFO), window, FLASHW_TRAY | FLASHW_TIMERNOFG, 3, 0};
                FlashWindowEx(&blink);
            }
            return;
        }
        toasts.push_back({title, text, badge, ImGui::GetTime() + 7.0, ImGui::GetTime(), friendId, std::move(action)});
    }

    // Whether the game was set to play online when it started (any of the online choices on its page).
    static bool OnlineChosen(const Game& game) {
        const std::string room = ReadCoreOption(game, "eden_sala");
        return ReadCoreOption(game, "ranga_online") == "ligado" || ReadCoreOption(game, "citra_sala") == "entrar" ||
               ReadCoreOption(game, "rpcs3_online") == "ligado" || ReadCoreOption(game, "xemu_network_backend") == "udp" ||
               ReadCoreOption(game, "ppsspp_enable_wlan") == "enabled" || (!room.empty() && room != "desligada");
    }

    // The games that closed: their time goes to the profile, and what that unlocks shows at the bottom.
    void ProcessEnded() {
        if (!runner.Busy() && !playingTitle.empty()) {
            playingTitle.clear();
            playingSystem.clear();
            playingInvite.clear();
        }
        std::vector<Ended> done;
        {
            std::lock_guard guard(endedLock);
            done.swap(endedQueue);
        }
        if (done.empty()) return;
        for (const auto& ended : done) {
            const std::wstring& key = ended.game.titleId;
            // The console's own menu (StartSystem) is no game.
            if (key.size() > 8 && key.compare(key.size() - 8, 8, L"-SISTEMA") == 0) continue;
            profile.RecordSession(key, ended.game.system, ended.game.title, ended.started, ended.seconds, ended.online);
            // The hosting PC: the shaders this run compiled go to the friends (Cache de shaders).
            if (ended.game.system == L"SWITCH" && RoomServerEnabled(library.Root())) ShareShaderCaches(library.Root());
            // A crash (an NTSTATUS error such as 0xC0000005): the report, which keeps the logs of this very run, is
            // offered at once, before another start writes over them.
            if ((ended.code & 0xF0000000u) == 0xC0000000u && ended.code != 0xCFFFFFFFu) {
                wchar_t hex[16];
                std::swprintf(hex, 16, L"0x%lX", ended.code);
                Confirm(ended.game.title + L" fechou com erro (código " + hex + L") depois de " + std::to_wstring(ended.seconds) +
                            L" s.\n\nQuer gerar o relatório de erro (um .zip com os logs) para mandar a quem for ajudar?",
                        [this, game = ended.game] { MakeErrorReport(game); });
            }
        }
        ScanProfileRa();
        for (const auto& title : profile.Evaluate(LibraryCount())) AddToast("Conquista desbloqueada", title, "");
        // A game was played: its saves go to the backup folder, when one is set and something changed, and to the
        // cloud, when it is on.
        StartBackup(false);
        if (CloudEnabled(library.Root())) StartCloud(CloudJob::Send);
    }

    void TakeRaLogin() {
        std::lock_guard guard(raLock);
        if (!raDone) return;
        raDone = false;
        profile.SetRetroAchievements(raUserNew, raTokenNew);
        raTokenNew.clear();
        AddToast("RetroAchievements", "Conectado como " + raUserNew, "");
    }

    void StartRaLogin() {
        if (raBusy) return;
        if (raThread.joinable()) raThread.join();
        const std::string user = raUserField.data(), password = raPasswordField.data();
        // The password is used once and not kept, not even in the field.
        std::fill(raPasswordField.begin(), raPasswordField.end(), '\0');
        {
            std::lock_guard guard(raLock);
            raProblem.clear();
        }
        if (user.empty() || password.empty()) {
            std::lock_guard guard(raLock);
            raProblem = "Digite o usuário e a senha do RetroAchievements.";
            return;
        }
        raBusy = true;
        raThread = std::thread([this, user, password] {
            std::string token, name, error;
            const bool ok = RetroAchievementsLogin(user, password, token, name, error);
            std::lock_guard guard(raLock);
            if (ok) {
                raTokenNew = token;
                raUserNew = name;
                raDone = true;
            } else {
                raProblem = error;
            }
            raBusy = false;
        });
    }

    // Sends this player's card to the central and takes the friends back: at once when something changed (a game opened
    // or closed, the profile was edited), otherwise every two minutes, so "jogando agora" and "visto" stay true. Only
    // with an account.
    void SyncProfiles(bool force) {
        if (!profileLoaded || profileBusy || profile.session.empty()) return;
        const ULONGLONG now = GetTickCount64();
        const std::string playing = playingTitle.empty() ? "" : playingTitle + "\t" + playingSystem + "\t" + playingInvite;
        const bool changed = profile.Revision() != sentRevision || playing != sentPlaying;
        // Between cards only the friends are asked for: every five seconds on screen, every minute minimized or covered.
        bool friendsOnly = false;
        if (!force && profileSyncedAt != 0) {
            if (changed && now - profileSyncedAt < 5000) return;
            if (!changed && now - profileSyncedAt < 120000) {
                if (now - friendsPolledAt < (background ? 60000u : 5000u)) return;
                friendsOnly = true;
            }
        }
        if (profileThread.joinable()) profileThread.join();
        friendsPolledAt = now;
        if (friendsOnly) {
            profileBusy = true;
            profileThread = std::thread([this, root = library.Root(), session = profile.session] {
                const HttpResult got = CentralCall(root, "GET", "/amigos", "", session, 8000);
                const HttpResult gotGroups = got.status == 200 ? CentralCall(root, "GET", "/grupos", "", session, 8000) : HttpResult{};
                {
                    std::lock_guard guard(friendsLock);
                    if (gotGroups.status == 200) groups = ReadGroups(gotGroups.body);
                    if (got.status == 200) {
                        FriendsList list = ReadFriends(got.body);
                        friends = std::move(list.friends);
                        friendsIncoming = std::move(list.incoming);
                        friendsOutgoing = std::move(list.outgoing);
                        friendsBlocked = std::move(list.blocked);
                        friendsAt = static_cast<long long>(std::time(nullptr));
                        friendsProblem.clear();
                    } else if (got.status == 401) {
                        friendsProblem = "A sessão acabou: entre na sua conta de novo.";
                        sessionExpired = true;
                    }
                }
                profileBusy = false;
            });
            return;
        }
        profileSyncedAt = now;
        sentRevision = profile.Revision();
        sentPlaying = playing;
        PlayerCard ownCard = profile.Card(playingTitle, playingSystem);
        ownCard.invite = playingInvite;
        // A Ranga copied from the zip has no number of its own yet; it is at least the version this code is.
        ownCard.version = InstalledVersionName(library.Root());
        if (ownCard.version.empty()) ownCard.version = kSocialVersion;
        const std::string body = WriteCard(ownCard);
        const std::wstring root = library.Root();
        const std::string session = profile.session;
        // The photo and cover go only when they changed since the central got them (empty removes them there).
        const std::string photoHash = profile.photoHash, coverHash = profile.coverHash;
        const std::string photo = photoHash != profile.photoSent ? profile.photo : "", cover = coverHash != profile.coverSent ? profile.cover : "";
        const bool sendPhoto = photoHash != profile.photoSent, sendCover = coverHash != profile.coverSent;
        profileBusy = true;
        profileThread = std::thread([this, body, root, session, photo, cover, sendPhoto, sendCover, photoHash, coverHash] {
            bool photoOk = !sendPhoto, coverOk = !sendCover;
            if (sendPhoto) photoOk = CentralCall(root, "POST", "/midia", "tipo\tfoto\ndados\t" + Base64(photo) + "\n", session, 30000).status == 200;
            if (sendCover) coverOk = CentralCall(root, "POST", "/midia", "tipo\tcapa\ndados\t" + Base64(cover) + "\n", session, 30000).status == 200;
            if ((sendPhoto && photoOk) || (sendCover && coverOk)) {
                std::lock_guard guard(importLock);
                if (sendPhoto && photoOk) sentPhotoHash = photoHash;
                if (sendCover && coverOk) sentCoverHash = coverHash;
                sentMediaDone = true;
            }
            const HttpResult sent = CentralCall(root, "POST", "/perfil", body, session, 8000);
            const HttpResult got = sent.status == 200 ? CentralCall(root, "GET", "/amigos", "", session, 8000) : HttpResult{};
            const HttpResult gotGroups = got.status == 200 ? CentralCall(root, "GET", "/grupos", "", session, 8000) : HttpResult{};
            {
                std::lock_guard guard(friendsLock);
                if (gotGroups.status == 200) groups = ReadGroups(gotGroups.body);
                if (got.status == 200) {
                    FriendsList list = ReadFriends(got.body);
                    friends = std::move(list.friends);
                    friendsIncoming = std::move(list.incoming);
                    friendsOutgoing = std::move(list.outgoing);
                    friendsBlocked = std::move(list.blocked);
                    friendsAt = static_cast<long long>(std::time(nullptr));
                    friendsProblem.clear();
                } else if (sent.status == 401 || got.status == 401) {
                    friendsProblem = "A sessão acabou: entre na sua conta de novo.";
                    sessionExpired = true;
                } else if (sent.status == 0 || got.status == 0) {
                    friendsProblem = "O servidor do Ranga não respondeu. Confira a internet.";
                } else {
                    friendsProblem = "O servidor do Ranga respondeu com erro " + std::to_string(sent.status != 200 ? sent.status : got.status) + ".";
                }
            }
            profileBusy = false;
        });
    }

    // Sign-in, sign-up and recovery answers, friend actions done, an expired session and new friend requests.
    void ProcessAccount() {
        {
            std::lock_guard guard(importLock);
            if (importDone) {
                importDone = false;
                if (!importError.empty()) {
                    Notify(Wide(importError));
                } else if (importCover) {
                    profile.SetCover(importResult);
                } else {
                    profile.SetPhoto(importResult);
                    for (const auto& title : profile.Evaluate(LibraryCount())) AddToast("Conquista desbloqueada", title, "");
                }
                importResult.clear();
                profileSyncedAt = 0;
            }
            if (sentMediaDone) {
                sentMediaDone = false;
                if (!sentPhotoHash.empty()) profile.MarkSent(false, sentPhotoHash);
                if (!sentCoverHash.empty()) profile.MarkSent(true, sentCoverHash);
                sentPhotoHash.clear();
                sentCoverHash.clear();
            }
        }
        if (sessionExpired.exchange(false)) {
            profile.ClearAccount();
            AddToast("Conta do Ranga", "A sessão acabou. Entre de novo.", "");
        }
        {
            std::lock_guard guard(accountLock);
            if (accountDone) {
                accountDone = false;
                if (!accountAnswer.ok) {
                    accountProblem = accountAnswer.error;
                } else {
                    const std::string nameTag = accountAnswer.name + "#" + std::to_string(accountAnswer.tag);
                    profile.SetAccount(accountAnswer.id, nameTag, accountAnswer.session);
                    accountRecovery = accountAnswer.recovery;
                    if (accountRecovery.empty()) accountOpen = false;
                    AddToast("Conta do Ranga", "Entrou como " + nameTag, "");
                    ResetSocial();
                }
            }
        }
        {
            std::lock_guard guard(actionLock);
            if (actionDone) {
                actionDone = false;
                friendAction = actionResult;
                // What the chat asked for answers in a notice: the chat does not show the Amigos tab's line.
                if (!actionResult.empty() && (actionPath.rfind("/grupo", 0) == 0 || actionPath == "/denunciar" || actionPath == "/bloquear")) AddToast("Ranga", actionResult, "");
                profileSyncedAt = 0; // the friends again, at once
            }
        }
        std::vector<std::string> fresh;
        {
            std::lock_guard guard(friendsLock);
            if (friendsAt != 0) {
                for (const auto& [id, name] : friendsIncoming) {
                    if (incomingSeen.insert(id).second && incomingPrimed) fresh.push_back(name);
                }
                incomingPrimed = true;
            }
        }
        for (const auto& name : fresh) AddToast("Pedido de amizade", name, "");
    }

    // A different account (or none): nothing of the last one's friends or conversations stays.
    void ResetSocial() {
        {
            std::lock_guard guard(friendsLock);
            friends.clear();
            friendsIncoming.clear();
            friendsOutgoing.clear();
            friendsBlocked.clear();
            groups.clear();
            friendsProblem.clear();
            friendsAt = 0;
            incomingSeen.clear();
            incomingPrimed = false;
        }
        {
            std::lock_guard guard(chatLock);
            chat.clear();
            chatLast = chatNoticed = groupLast = groupNoticed = 0;
            chatSeen.clear();
            chatSeenSaved.clear();
            chatPeer.clear();
            chatProblem.clear();
            chatPrimed = false;
            chatUnread = 0;
        }
        chatFetched = false;
        chatAt = 0;
        profileSyncedAt = 0;
    }

    static std::string NoTabs(std::string text) {
        for (char& c : text) if (c == '\t' || c == '\n' || c == '\r') c = ' ';
        return text;
    }

    void StartAccount() {
        if (accountBusy) return;
        if (accountThread.joinable()) accountThread.join();
        const std::string name = NoTabs(accName.data()), password = NoTabs(accPass.data()), again = NoTabs(accPass2.data()), code = NoTabs(accCode.data());
        std::string path, body;
        accountProblem.clear();
        if (accountMode == 1 && password != again) {
            accountProblem = "As duas senhas não são iguais.";
            return;
        }
        if (accountMode == 0) path = "/conta/entrar", body = "nome\t" + name + "\nsenha\t" + password + "\n";
        else if (accountMode == 1) path = "/conta/criar", body = "nome\t" + name + "\nsenha\t" + password + "\npc\t" + PcFingerprint() + "\n";
        else path = "/conta/recuperar", body = "nome\t" + name + "\nrecuperacao\t" + code + "\nsenha\t" + password + "\n";
        // The password is used once and not kept, not even in the fields.
        accPass.fill('\0');
        accPass2.fill('\0');
        accountBusy = true;
        accountThread = std::thread([this, path, body, root = library.Root()] {
            std::string full = body;
            AccountAnswer answer;
            bool ready = true;
            if (path == "/conta/criar") {
                // The central's challenge, solved here: a few seconds for one account, a lot for a thousand.
                accountStage = 1;
                const HttpResult got = CentralCall(root, "GET", "/conta/desafio", "", "", 10000);
                const std::string challenge = ReplyField(got.body, "desafio");
                const int bits = std::atoi(ReplyField(got.body, "bits").c_str());
                const std::string solved = got.status == 200 && !challenge.empty() && bits > 0 && bits < 32 ? SolveChallenge(challenge, bits) : "";
                if (solved.empty()) {
                    ready = false;
                    answer.error = got.status == 0 ? "O servidor do Ranga não respondeu. Confira a internet." : "A verificação do PC não deu certo. Tente de novo.";
                } else {
                    full += "desafio\t" + challenge + "\nresposta\t" + solved + "\n";
                }
                accountStage = 0;
            }
            if (ready) answer = AccountRequest(root, path, full);
            std::lock_guard guard(accountLock);
            accountAnswer = std::move(answer);
            accountDone = true;
            accountBusy = false;
        });
    }

    void OpenAccount(int mode) {
        accountOpen = true;
        accountMode = mode;
        accountProblem.clear();
        accountRecovery.clear();
        accPass.fill('\0');
        accPass2.fill('\0');
        accCode.fill('\0');
    }

    void SignOut() {
        // The session is closed on the central too, in the background; here it goes at once.
        std::thread([root = library.Root(), session = profile.session] { CentralCall(root, "POST", "/conta/sair", "", session, 8000); }).detach();
        profile.ClearAccount();
        ResetSocial();
    }

    // Ask, accept, refuse, cancel or remove: one at a time, the friends fetched again when it is done.
    void FriendAction(const std::string& path, const std::string& body) {
        if (actionBusy || profile.session.empty()) return;
        if (actionThread.joinable()) actionThread.join();
        actionBusy = true;
        friendAction.clear();
        actionPath = path;
        actionThread = std::thread([this, path, body, root = library.Root(), session = profile.session] {
            const HttpResult got = CentralCall(root, "POST", path, body, session, 10000);
            std::string result;
            if (got.status == 200) {
                if (path == "/amigos/pedir") result = ReplyField(got.body, "estado") == "amigos" ? "Vocês agora são amigos!" : "Pedido enviado. Quando aceitarem, aparece aqui.";
                else if (path == "/denunciar") result = "Denúncia enviada ao dono do Ranga, com as últimas mensagens de vocês. Se quiser, bloqueie também.";
                else if (path == "/grupo/criar") result = "Grupo criado.";
                else if (path == "/grupo/adicionar") result = "Adicionado ao grupo.";
                else if (path == "/grupo/sair") result = "Você saiu do grupo.";
                else if (path == "/bloquear") result = "Bloqueado.";
            } else if (got.status == 404 && (path.rfind("/grupo", 0) == 0 || path == "/denunciar")) {
                result = "O servidor do Ranga ainda não tem isso (falta atualizar a central).";
            } else if (got.status == 0) {
                result = "O servidor do Ranga não respondeu. Confira a internet.";
            } else {
                result = ReplyField(got.body, "erro");
                if (result.empty()) result = "O servidor do Ranga respondeu com erro " + std::to_string(got.status) + ".";
                if (got.status == 401) sessionExpired = true;
            }
            std::lock_guard guard(actionLock);
            actionResult = result;
            actionDone = true;
            actionBusy = false;
        });
    }

    std::string FriendName(const std::string& id) {
        std::lock_guard guard(friendsLock);
        for (const auto& card : friends) if (card.id == id) return card.name;
        // Someone in a group who is not a friend: the name before the #.
        for (const auto& group : groups) {
            for (const auto& [account, name] : group.members) if (account == id) return name.substr(0, name.rfind('#'));
        }
        if (IsGroup(id)) {
            for (const auto& group : groups) if ("g:" + group.id == id) return group.name;
            return "Grupo";
        }
        return "Amigo";
    }
    // The conversation a message belongs to: its group, or the friend on the other side.
    std::string ConvOf(const DirectMessage& message) const {
        if (IsGroup(message.to)) return message.to;
        return message.from == profile.accountId ? message.to : message.from;
    }
    static bool BadgeName(const std::string& name) {
        if (name.empty() || name.size() > 40) return false;
        for (const char c : name) if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') return false;
        return true;
    }

    // A RetroAchievements badge (its locked look when locked), null while it downloads or when there is none.
    const Texture* Badge(const std::string& name, bool locked) {
        if (!BadgeName(name)) return nullptr;
        const std::string file = name + (locked ? "_lock" : "");
        if (badgeDone != badgeSeen) {
            badgeSeen = badgeDone;
            badgeMissing.clear();
        }
        if (const auto found = badgeTextures.find(file); found != badgeTextures.end()) return found->second.view != nullptr ? &found->second : nullptr;
        if (badgeMissing.count(file) != 0) return nullptr;
        const std::wstring path = library.Root() + L"\\perfil\\insignias\\" + Wide(file) + L".png";
        if (Exists(path)) {
            Texture& texture = badgeTextures[file] = LoadTexture(path, 128);
            return texture.view != nullptr ? &texture : nullptr;
        }
        badgeMissing.insert(file);
        if (badgeAsked.insert(file).second) {
            {
                std::lock_guard guard(badgeLock);
                badgeQueue.push_back(file);
            }
            StartBadgeDownloads();
        }
        return nullptr;
    }

    void StartBadgeDownloads() {
        if (badgeBusy) return;
        if (badgeThread.joinable()) badgeThread.join();
        badgeBusy = true;
        const std::wstring folder = library.Root() + L"\\perfil\\insignias";
        badgeThread = std::thread([this, folder] {
            MakeFolders(folder);
            for (;;) {
                std::string file;
                {
                    std::lock_guard guard(badgeLock);
                    if (badgeQueue.empty()) break;
                    file = badgeQueue.back();
                    badgeQueue.pop_back();
                }
                const HttpResult got = HttpRequest("GET", "https://media.retroachievements.org/Badge/" + file + ".png", "", "", kRaUserAgent, 10000);
                if (got.status != 200 || got.body.size() < 8 || got.body.compare(1, 3, "PNG") != 0) continue;
                const std::wstring path = folder + L"\\" + Wide(file) + L".png";
                HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (out == INVALID_HANDLE_VALUE) continue;
                DWORD put = 0;
                WriteFile(out, got.body.data(), static_cast<DWORD>(got.body.size()), &put, nullptr);
                CloseHandle(out);
            }
            ++badgeDone;
            badgeBusy = false;
        });
    }

    static AnimTexture MakeAnim(const std::string& rani) {
        Media media;
        if (!DecodeMedia(rani, media)) return {};
        return UploadAnim(media);
    }

    static AnimTexture UploadAnim(const Media& media) {
        AnimTexture anim;
        int at = 0;
        for (const auto& frame : media.frames) {
            anim.frames.push_back(UploadTexture(frame.rgba.data(), media.width, media.height));
            at += frame.delayMs;
            anim.ends.push_back(at);
        }
        anim.total = std::max(at, 1);
        return anim;
    }

    // The photo or cover of a card, as frames: this player's from the profile, a friend's from perfil\\amigos (fetched
    // from the central when the version changed; the old one shows meanwhile).
    const AnimTexture* MediaOf(const PlayerCard& card, bool cover) {
        if (card.id == profile.id || (!profile.accountId.empty() && card.id == profile.accountId)) {
            const std::string& bytes = cover ? profile.cover : profile.photo;
            std::string& shown = cover ? ownCoverHash : ownPhotoHash;
            AnimTexture& anim = cover ? ownCover : ownPhoto;
            const std::string& hash = cover ? profile.coverHash : profile.photoHash;
            if (shown != hash) {
                anim.Clear();
                if (!bytes.empty()) anim = MakeAnim(bytes);
                shown = hash;
            }
            if (anim.frames.size() > 1) animating = true;
            return anim.frames.empty() ? nullptr : &anim;
        }
        const std::string& version = cover ? card.coverVersion : card.photoVersion;
        if (version.empty() || card.id.empty()) return nullptr;
        const char* kind = cover ? "capa" : "foto";
        CachedMedia& cached = mediaCache[card.id + "|" + kind];
        cached.usedAt = ImGui::GetTime();
        // A new version is read (or fetched) and decoded by the media thread; the old one shows meanwhile.
        if (cached.version != version && mediaAsked.insert(card.id + "|" + kind + "|" + version).second) {
            {
                std::lock_guard guard(mediaLock);
                mediaQueue.push_back({card.id, kind, version});
            }
            StartMediaDownloads();
        }
        if (cached.anim.frames.size() > 1) animating = true;
        return cached.anim.frames.empty() ? nullptr : &cached.anim;
    }

    // Once a frame: decoded media become textures, and friends' media not drawn for a minute are let go (a cover
    // is ~20 MB of video memory). They come back from perfil\\amigos when shown again.
    void TendMedia() {
        std::vector<DecodedMedia> ready;
        {
            std::lock_guard guard(mediaLock);
            ready.swap(mediaReady);
        }
        for (auto& item : ready) {
            CachedMedia& cached = mediaCache[item.key];
            cached.anim.Clear();
            cached.anim = UploadAnim(item.media);
            cached.version = item.version;
            cached.usedAt = ImGui::GetTime();
        }
        const double now = ImGui::GetTime();
        for (auto it = mediaCache.begin(); it != mediaCache.end();) {
            if (now - it->second.usedAt > 60) {
                it->second.anim.Clear();
                mediaAsked.erase(it->first + "|" + it->second.version);
                it = mediaCache.erase(it);
            } else {
                ++it;
            }
        }
    }

    void StartMediaDownloads() {
        if (mediaBusy || profile.session.empty()) return;
        if (mediaThread.joinable()) mediaThread.join();
        mediaBusy = true;
        mediaThread = std::thread([this, root = library.Root(), session = profile.session] {
            const std::wstring folder = root + L"\\perfil\\amigos";
            MakeFolders(folder);
            for (;;) {
                std::array<std::string, 3> item;
                {
                    std::lock_guard guard(mediaLock);
                    if (mediaQueue.empty()) break;
                    item = mediaQueue.back();
                    mediaQueue.pop_back();
                }
                const std::wstring file = folder + L"\\" + Wide(item[0]) + L"-" + Wide(item[1]) + L"-" + Wide(item[2]) + L".rani";
                std::string bytes = ReadBytes(file);
                if (bytes.compare(0, 4, "RANI") != 0) {
                    // A chat picture by its id; a photo or cover by its owner and kind.
                    const std::string path = item[1] == "imagem" ? "/imagem?id=" + item[0] : "/midia?conta=" + item[0] + "&tipo=" + item[1];
                    const HttpResult got = CentralCall(root, "GET", path, "", session, 30000);
                    if (got.status != 200) continue;
                    bytes = Unbase64(got.body);
                    if (bytes.compare(0, 4, "RANI") != 0) continue;
                    HANDLE out = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                    if (out != INVALID_HANDLE_VALUE) {
                        DWORD put = 0;
                        WriteFile(out, bytes.data(), static_cast<DWORD>(bytes.size()), &put, nullptr);
                        CloseHandle(out);
                    }
                }
                DecodedMedia decoded{item[0] + "|" + item[1], item[2], {}};
                if (!DecodeMedia(bytes, decoded.media)) continue;
                std::lock_guard guard(mediaLock);
                mediaReady.push_back(std::move(decoded));
            }
            mediaBusy = false;
        });
    }

    // The cover across a box (cut to its shape), darkened toward the bottom so text over it reads; nothing when there
    // is none.
    bool DrawCover(ImDrawList* draw, ImVec2 min, ImVec2 max, const PlayerCard& card, float rounding, float shade) {
        const AnimTexture* anim = MediaOf(card, true);
        const Texture* frame = anim != nullptr ? anim->Now() : nullptr;
        if (frame == nullptr) return false;
        const float boxAspect = (max.x - min.x) / std::max(1.0f, max.y - min.y), imageAspect = static_cast<float>(frame->width) / frame->height;
        ImVec2 uv0(0, 0), uv1(1, 1);
        if (imageAspect > boxAspect) {
            const float visible = boxAspect / imageAspect;
            uv0.x = (1 - visible) / 2;
            uv1.x = uv0.x + visible;
        } else {
            const float visible = imageAspect / boxAspect;
            uv0.y = (1 - visible) / 2;
            uv1.y = uv0.y + visible;
        }
        draw->AddImageRounded(frame->Ref(), min, max, uv0, uv1, IM_COL32_WHITE, rounding);
        draw->AddRectFilledMultiColor(min, max, Packed(kBackground, shade * 0.35f), Packed(kBackground, shade * 0.35f), Packed(kBackground, shade), Packed(kBackground, shade));
        return true;
    }

    // Turns a picture, GIF or video into the photo or the cover, in the background (a video takes a second or two).
    void ImportProfileMedia(const std::wstring& path, bool cover) {
        if (importBusy) return;
        if (importThread.joinable()) importThread.join();
        importBusy = true;
        importThread = std::thread([this, path, cover] {
            Media media;
            std::string error, bytes;
            if (ImportMedia(path, cover ? kCoverWidth : kPhotoSide, cover ? kCoverHeight : kPhotoSide, media, error)) bytes = EncodeMedia(media);
            std::lock_guard guard(importLock);
            importResult = bytes;
            importError = bytes.empty() && error.empty() ? "Não deu para converter esse arquivo." : error;
            importCover = cover;
            importDone = true;
            importBusy = false;
        });
    }

    // The photo in a square with a ring of the level's color; no photo is the name's first letter.
    void DrawAvatar(ImDrawList* draw, ImVec2 min, float side, const PlayerCard& card, float scale) {
        const ImVec2 max(min.x + side, min.y + side);
        const float ring = std::max(2.0f, 3 * scale * side / (128 * scale));
        draw->AddRectFilled(ImVec2(min.x - ring, min.y - ring), ImVec2(max.x + ring, max.y + ring), Packed(LevelColor(LevelFor(card.xp).level)), 8 * scale);
        const AnimTexture* anim = MediaOf(card, false);
        if (const Texture* photo = anim != nullptr ? anim->Now() : nullptr) {
            draw->AddImageRounded(photo->Ref(), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6 * scale);
            return;
        }
        draw->AddRectFilled(min, max, Packed(kSelected), 6 * scale);
        std::string initial = card.name.empty() ? "?" : card.name.substr(0, 1);
        // The whole first character, when it takes more than one byte.
        for (size_t i = 1; i < card.name.size() && (static_cast<unsigned char>(card.name[i]) & 0xc0) == 0x80; ++i) initial += card.name[i];
        const float size = side * 0.5f;
        const ImVec2 extent = semibold->CalcTextSizeA(size, FLT_MAX, 0, initial.c_str());
        draw->AddText(semibold, size, ImVec2(min.x + (side - extent.x) / 2, min.y + (side - extent.y) / 2), Packed(kText), initial.c_str());
    }

    // A showcase item or achievement picture: RetroAchievements' badge, or Ranga's medal.
    void DrawAchievement(ImDrawList* draw, ImVec2 min, float side, bool ra, const std::string& badge, bool lit) {
        if (ra) {
            if (const Texture* image = Badge(badge, !lit)) {
                draw->AddImageRounded(image->Ref(), min, ImVec2(min.x + side, min.y + side), ImVec2(0, 0), ImVec2(1, 1), lit ? IM_COL32_WHITE : IM_COL32(255, 255, 255, 140), side * 0.12f);
                return;
            }
        }
        DrawMedal(draw, min, side, badge, lit);
    }

    const Game* LibraryGame(const std::wstring& key, const std::wstring& system, const std::wstring& title) {
        for (const auto& game : library.Games()) {
            if (!key.empty() && game.titleId == key) return &game;
            if (key.empty() && game.system == system && game.title == title) return &game;
        }
        return nullptr;
    }

    void OpenProfile() {
        std::snprintf(centralField.data(), centralField.size(), "%s", CentralServer(library.Root()).c_str());
        ownerHere = !OwnerKey(library.Root()).empty();
        profileOpen = true;
        enginesPage = false;
        newsOpen = false;
        storeOpen = false;
        profileViewing.clear();
        SyncProfiles(true);
    }

    void OpenProfileEditor() {
        profileEdit = true;
        std::snprintf(editName.data(), editName.size(), "%s", profile.name.c_str());
        std::snprintf(editPhrase.data(), editPhrase.size(), "%s", profile.phrase.c_str());
        if (raUserField[0] == '\0') std::snprintf(raUserField.data(), raUserField.size(), "%s", profile.raUser.c_str());
    }
    void SectionTitle(const char* text, float scale) {
        ImGui::Dummy(ImVec2(0, 10 * scale));
        ImGui::PushFont(semibold, 20);
        ImGui::TextUnformatted(text);
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(0, 4 * scale));
    }

    void ProfilePage(float scale, const Nav& nav) {
        if (nav.back && !profileEdit) {
            if (!profileViewing.empty()) profileViewing.clear();
            else profileOpen = false;
            return;
        }
        const bool own = profileViewing.empty();
        PlayerCard card;
        if (own) {
            card = profile.Card(playingTitle, playingSystem);
        } else {
            std::lock_guard guard(friendsLock);
            const auto found = std::find_if(friends.begin(), friends.end(), [&](const PlayerCard& c) { return c.id == profileViewing; });
            if (found == friends.end()) {
                profileViewing.clear();
                return;
            }
            card = *found;
        }
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
        const float left = 40 * scale, headerHeight = 220 * scale;
        draw->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + headerHeight), Packed(kSidebar));
        DrawCover(draw, origin, ImVec2(origin.x + size.x, origin.y + headerHeight), card, 0, 0.92f);
        ImGui::SetCursorPos(ImVec2(20 * scale, 18 * scale));
        if (ImGui::Button(own ? "<  Voltar" : "<  Meu perfil", ImVec2(0, 34 * scale))) {
            pending = [this, own] {
                if (own) profileOpen = false;
                else profileViewing.clear();
            };
        }

        // The photo, the name and what the player is doing; the level on the right.
        const float avatarSide = 120 * scale;
        const ImVec2 avatarAt(origin.x + left, origin.y + 70 * scale);
        DrawAvatar(draw, avatarAt, avatarSide, card, scale);
        const float textX = left + avatarSide + 28 * scale;
        ImGui::SetCursorPos(ImVec2(textX, 72 * scale));
        ImGui::PushFont(semibold, 32);
        ImGui::TextUnformatted(card.name.c_str());
        ImGui::PopFont();
        if (!card.account.empty()) {
            ImGui::SameLine();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 12 * scale);
            ImGui::TextDisabled("%s", card.account.substr(card.account.rfind('#')).c_str());
        }
        if (!card.phrase.empty()) {
            ImGui::SetCursorPosX(textX);
            ImGui::PushTextWrapPos(size.x - 260 * scale);
            ImGui::TextDisabled("%s", card.phrase.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::SetCursorPosX(textX);
        const long long now = static_cast<long long>(std::time(nullptr));
        const bool fresh = own || (!card.closed && now - card.seen < 300);
        if (!card.playing.empty() && fresh) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
            ImGui::Text("Jogando agora: %s (%s)", card.playing.c_str(), card.playingSystem.c_str());
            ImGui::PopStyleColor();
        } else if (own || fresh) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
            ImGui::TextUnformatted("Online");
            ImGui::PopStyleColor();
        } else {
            ImGui::TextDisabled("Visto %s", FormatAgo(card.seen).c_str());
        }

        const Level level = LevelFor(card.xp);
        const float radius = 32 * scale;
        const ImVec2 circle(origin.x + size.x - left - 190 * scale + radius, origin.y + 100 * scale);
        draw->AddCircleFilled(circle, radius, Packed(kSurface));
        draw->AddCircle(circle, radius, Packed(LevelColor(level.level)), 0, 3 * scale);
        const std::string number = std::to_string(level.level);
        const ImVec2 extent = semibold->CalcTextSizeA(26 * scale, FLT_MAX, 0, number.c_str());
        draw->AddText(semibold, 26 * scale, ImVec2(circle.x - extent.x / 2, circle.y - extent.y / 2), Packed(kText), number.c_str());
        ImGui::SetCursorScreenPos(ImVec2(circle.x + radius + 14 * scale, circle.y - 30 * scale));
        ImGui::BeginGroup();
        ImGui::PushFont(semibold, 18);
        ImGui::Text("Nível %d", level.level);
        ImGui::PopFont();
        ImGui::PushStyleColor(ImGuiCol_PlotHistogram, Color(LevelColor(level.level)));
        ImGui::ProgressBar(Smooth("xp:" + card.id, static_cast<float>(level.into) / static_cast<float>(level.size), 5.0f), ImVec2(110 * scale, 6 * scale), "");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%d / %d XP", level.into, level.size);
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("XP: 10 por hora jogada, 50 por conquista do Ranga e os pontos de cada conquista do RetroAchievements.");
        if (own) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - left - 190 * scale, origin.y + 150 * scale));
            if (ImGui::Button("Editar perfil", ImVec2(190 * scale, 36 * scale))) OpenProfileEditor();
            if (profile.session.empty()) {
                ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - left - 400 * scale, origin.y + 150 * scale));
                ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
                if (ImGui::Button("Entrar ou criar conta", ImVec2(200 * scale, 36 * scale))) OpenAccount(1);
                ImGui::PopStyleColor(2);
            }
        } else {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - left - 286 * scale, origin.y + 150 * scale));
            ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
            if (ImGui::Button("Conversar", ImVec2(110 * scale, 36 * scale))) {
                pending = [this, id = card.id] {
                    chatPeer = id;
                    profileViewing.clear();
                    openChatTab = true;
                };
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Desfazer", ImVec2(72 * scale, 36 * scale))) {
                const std::string id = card.id, who = card.name;
                Confirm(L"Desfazer a amizade com " + Wide(who) + L"? Vocês param de ver o perfil e de conversar.", [this, id] {
                    FriendAction("/amigos/remover", "conta\t" + id + "\n");
                    profileViewing.clear();
                });
            }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Desfazer a amizade");
            ImGui::SameLine();
            if (ImGui::Button("Bloquear", ImVec2(80 * scale, 36 * scale))) {
                const std::string id = card.id, who = card.name;
                Confirm(L"Bloquear " + Wide(who) + L"? A amizade acaba e essa pessoa não consegue mais te pedir amizade nem falar com você.", [this, id] {
                    FriendAction("/bloquear", "conta\t" + id + "\n");
                    profileViewing.clear();
                });
            }
        }

        // The numbers under the header.
        const float tilesY = headerHeight + 16 * scale, gap = 14 * scale;
        const float tileWidth = (size.x - 2 * left - 3 * gap) / 4, tileHeight = 66 * scale;
        const std::string tiles[4][2] = {
            {FormatPlayed(card.seconds), "jogados"},
            {std::to_string(card.games), card.games == 1 ? "jogo" : "jogos"},
            {std::to_string(card.consoles), card.consoles == 1 ? "console" : "consoles"},
            {std::to_string(card.achievements), card.raPoints > 0 ? "conquistas · " + std::to_string(card.raPoints) + " pontos RA" : "conquistas"},
        };
        for (int i = 0; i < 4; ++i) {
            const ImVec2 min(origin.x + left + i * (tileWidth + gap), origin.y + tilesY);
            draw->AddRectFilled(min, ImVec2(min.x + tileWidth, min.y + tileHeight), Packed(kSurface), 8 * scale);
            draw->AddText(semibold, 24 * scale, ImVec2(min.x + 16 * scale, min.y + 9 * scale), Packed(kText), tiles[i][0].c_str());
            draw->AddText(nullptr, ImGui::GetFontSize() * 0.92f, ImVec2(min.x + 16 * scale, min.y + 40 * scale), Packed(kDim), tiles[i][1].c_str());
        }

        ImGui::SetCursorPos(ImVec2(left, tilesY + tileHeight + 18 * scale));
        ImGui::BeginChild("##profileTabs", ImVec2(size.x - 2 * left, 0), ImGuiChildFlags_NavFlattened);
        if (ImGui::BeginTabBar("##profileTabBar")) {
            if (ImGui::BeginTabItem("Visão geral")) {
                ImGui::BeginChild("##overview", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                ProfileOverview(card, own, scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (own && ImGui::BeginTabItem("Conquistas")) {
                ImGui::BeginChild("##achievements", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                ProfileAchievements(scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (own && ImGui::BeginTabItem("Jogos")) {
                ImGui::BeginChild("##played", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                ProfileGames(scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            const std::string chatLabel = chatUnread > 0 ? "Chat (" + std::to_string(chatUnread) + ")###chat" : "Chat###chat";
            const ImGuiTabItemFlags chatFlags = openChatTab ? ImGuiTabItemFlags_SetSelected : 0;
            openChatTab = false;
            if (own && ImGui::BeginTabItem(chatLabel.c_str(), nullptr, chatFlags)) {
                ProfileChat(scale);
                ImGui::EndTabItem();
            }
            if (own && ImGui::BeginTabItem("Amigos")) {
                ImGui::BeginChild("##friends", ImVec2(0, 0), ImGuiChildFlags_NavFlattened);
                ProfileFriends(scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
    }

    void ProfileOverview(const PlayerCard& card, bool own, float scale) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        SectionTitle("Vitrine de conquistas", scale);
        if (card.showcase.empty()) {
            ImGui::TextDisabled(own ? "Nenhuma conquista ainda. Jogue um pouco: as primeiras aparecem aqui." : "Nenhuma conquista ainda.");
        } else {
            const float cell = 128 * scale, icon = 72 * scale;
            const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
            for (size_t i = 0; i < card.showcase.size(); ++i) {
                const ShowcaseItem& item = card.showcase[i];
                if (i % columns != 0) ImGui::SameLine(0, 0);
                ImGui::BeginGroup();
                const ImVec2 at = ImGui::GetCursorScreenPos();
                DrawAchievement(draw, ImVec2(at.x + (cell - icon) / 2, at.y), icon, item.ra, item.badge, true);
                ImGui::Dummy(ImVec2(cell, icon + 4 * scale));
                // The title centered under the picture, on two lines when it is long.
                const float wrap = cell - 12 * scale, textWidth = std::min(wrap, ImGui::CalcTextSize(item.title.c_str(), nullptr, false, wrap).x);
                const float x = ImGui::GetCursorPosX() + (cell - textWidth) / 2;
                ImGui::SetCursorPosX(x);
                ImGui::PushTextWrapPos(x + textWidth + 1);
                ImGui::TextWrapped("%s", item.title.c_str());
                ImGui::PopTextWrapPos();
                ImGui::EndGroup();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%s\n%s%s", item.title.c_str(), item.description.c_str(), FormatDate(item.when).c_str(),
                                      item.ra ? ("  ·  " + std::to_string(item.points) + " pontos (RetroAchievements)").c_str() : "  ·  Ranga");
                }
            }
        }
        // A friend's library: what the friend has, the games both have first (those can be played together online).
        if (!own && !card.owned.empty()) {
            std::set<std::pair<std::string, std::string>> mine;
            for (const auto& game : library.Games()) mine.insert({Utf8(game.system), Utf8(Plainer(game.title))});
            std::vector<std::pair<std::string, std::string>> both, theirs;
            for (const auto& item : card.owned) (mine.count({item.first, Utf8(Plainer(Wide(item.second)))}) ? both : theirs).push_back(item);
            SectionTitle(("Jogos que tem (" + std::to_string(card.owned.size()) + ")").c_str(), scale);
            ImGui::PushTextWrapPos(0);
            if (!both.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
                ImGui::TextUnformatted("Vocês dois têm (dá para jogar juntos):");
                ImGui::PopStyleColor();
                for (const auto& [system, title] : both) ImGui::BulletText("%s  ·  %s", title.c_str(), SystemName(Wide(system)).c_str());
            }
            if (!theirs.empty()) {
                if (!both.empty()) ImGui::Dummy(ImVec2(0, 4 * scale));
                ImGui::TextDisabled("Só %s tem:", card.name.c_str());
                for (const auto& [system, title] : theirs) ImGui::BulletText("%s  ·  %s", title.c_str(), SystemName(Wide(system)).c_str());
            }
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0, 8 * scale));
        }
        SectionTitle("Jogos recentes", scale);
        if (card.recent.empty()) ImGui::TextDisabled("Nenhum jogo ainda.");
        for (const PlayedGame& game : card.recent) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float side = 56 * scale;
            if (const Game* known = LibraryGame(game.key, game.system, game.title)) {
                GameArt(draw, *known, at, ImVec2(at.x + side, at.y + side), scale * 0.5f);
            } else {
                draw->AddRectFilled(at, ImVec2(at.x + side, at.y + side), Packed(kSurface), 6 * scale);
                DrawGlyph(draw, LookFor(game.system).glyph, ImVec2(at.x + side / 2, at.y + side / 2), side * 0.5f, Packed(0xffffff, 0.5f), Packed(kSurface));
            }
            ImGui::Dummy(ImVec2(side, side));
            ImGui::SameLine(0, 14 * scale);
            ImGui::BeginGroup();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * scale);
            ImGui::PushFont(semibold, 17);
            ImGui::TextUnformatted(Utf8(game.title).c_str());
            ImGui::PopFont();
            ImGui::TextDisabled("%s   ·   %s no total   ·   última vez em %s", SystemName(game.system).c_str(), FormatPlayed(game.seconds).c_str(), FormatDate(game.last).c_str());
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, 6 * scale));
        }
    }

    void ProfileAchievements(float scale) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        int lit = 0;
        for (const auto& achievement : profile.achievements) lit += achievement.unlocked != 0;
        char heading[96];
        std::snprintf(heading, sizeof(heading), "Do Ranga   (%d de %d)", lit, static_cast<int>(profile.achievements.size()));
        SectionTitle(heading, scale);
        ImGui::TextDisabled("Valem em todos os consoles, Switch e 3DS também: vêm do tempo de jogo, dos consoles, do online e da biblioteca.");
        ImGui::Dummy(ImVec2(0, 4 * scale));
        const float side = 48 * scale, width = ImGui::GetContentRegionAvail().x;
        for (const auto& achievement : profile.achievements) {
            const ImVec2 at = ImGui::GetCursorScreenPos();
            DrawMedal(draw, at, side, achievement.id, achievement.unlocked != 0);
            ImGui::Dummy(ImVec2(side, side));
            ImGui::SameLine(0, 14 * scale);
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 17);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(achievement.unlocked != 0 ? kText : kDim));
            ImGui::TextUnformatted(achievement.title);
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::TextDisabled("%s", achievement.description);
            ImGui::EndGroup();
            ImGui::SameLine(width - 190 * scale);
            ImGui::BeginGroup();
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + 6 * scale);
            if (achievement.unlocked != 0) {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
                ImGui::Text("Desbloqueada em %s", FormatDate(achievement.unlocked).c_str());
                ImGui::PopStyleColor();
            } else if (achievement.goal > 1) {
                char progress[32];
                std::snprintf(progress, sizeof(progress), "%d / %d", achievement.progress, achievement.goal);
                ImGui::ProgressBar(static_cast<float>(achievement.progress) / static_cast<float>(achievement.goal), ImVec2(180 * scale, 0), progress);
            } else {
                ImGui::TextDisabled("Bloqueada");
            }
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, 4 * scale));
        }

        SectionTitle("RetroAchievements", scale);
        if (profile.raToken.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("As conquistas de verdade de cada jogo clássico (NES, SNES, Mega Drive, Master System, Game Boy, GBA, N64, PS1, PS2, PSP, DS, "
                                "Saturn, Dreamcast e outros), feitas pela comunidade do retroachievements.org. Crie uma conta grátis lá e conecte aqui.");
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Conectar conta")) OpenProfileEditor();
            return;
        }
        if (profile.raGames.empty()) {
            ImGui::TextDisabled("Conectado como %s. Abra um jogo clássico: as conquistas dele aparecem aqui.", profile.raUser.c_str());
            return;
        }
        ImGui::TextDisabled("Conectado como %s   ·   %d pontos", profile.raUser.c_str(), profile.RaPoints());
        const float badge = 48 * scale, cell = badge + 8 * scale;
        for (const RaGame& game : profile.raGames) {
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::PushFont(semibold, 17);
            ImGui::TextUnformatted(game.title.c_str());
            ImGui::PopFont();
            int points = 0, total = 0;
            for (const auto& achievement : game.achievements) {
                total += achievement.points;
                if (achievement.unlocked != 0) points += achievement.points;
            }
            char progress[64];
            std::snprintf(progress, sizeof(progress), "%d de %d   ·   %d de %d pontos", game.Unlocked(), static_cast<int>(game.achievements.size()), points, total);
            ImGui::ProgressBar(game.achievements.empty() ? 0.0f : static_cast<float>(game.Unlocked()) / static_cast<float>(game.achievements.size()), ImVec2(std::min(width, 520 * scale), 0), progress);
            const int columns = std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / cell));
            for (size_t i = 0; i < game.achievements.size(); ++i) {
                const RaAchievement& achievement = game.achievements[i];
                if (i % columns != 0) ImGui::SameLine(0, 0);
                const ImVec2 at = ImGui::GetCursorScreenPos();
                DrawAchievement(draw, at, badge, true, achievement.badge, achievement.unlocked != 0);
                ImGui::PushID(static_cast<int>(achievement.id));
                ImGui::Dummy(ImVec2(cell, cell));
                ImGui::PopID();
                if (ImGui::IsItemHovered()) {
                    ImGui::SetTooltip("%s\n%s\n%d pontos   ·   %s", achievement.title.c_str(), achievement.description.c_str(), achievement.points,
                                      achievement.unlocked != 0 ? ("desbloqueada em " + FormatDate(achievement.unlocked)).c_str() : "bloqueada");
                }
            }
        }
    }

    void ProfileGames(float scale) {
        std::vector<const PlayedGame*> list;
        for (const auto& [key, game] : profile.games) list.push_back(&game);
        std::sort(list.begin(), list.end(), [](const PlayedGame* a, const PlayedGame* b) { return a->seconds > b->seconds; });
        if (list.empty()) {
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::TextDisabled("Nenhum jogo jogado ainda. O tempo conta a partir de agora, em cada jogo que abrir pelo Ranga.");
            return;
        }
        if (ImGui::BeginTable("##playedTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX)) {
            ImGui::TableSetupColumn("Jogo", ImGuiTableColumnFlags_WidthStretch, 3.0f);
            ImGui::TableSetupColumn("Console", ImGuiTableColumnFlags_WidthStretch, 1.4f);
            ImGui::TableSetupColumn("Tempo", ImGuiTableColumnFlags_WidthStretch, 0.8f);
            ImGui::TableSetupColumn("Vezes", ImGuiTableColumnFlags_WidthStretch, 0.6f);
            ImGui::TableSetupColumn("Última vez", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (const PlayedGame* game : list) {
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 30 * scale);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(Utf8(game->title).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", SystemName(game->system).c_str());
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(FormatPlayed(game->seconds).c_str());
                ImGui::TableNextColumn();
                ImGui::Text("%lld", static_cast<long long>(game->sessions));
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", FormatDate(game->last).c_str());
            }
            ImGui::EndTable();
        }
    }

    // Fetches the conversations (every three seconds while the Chat tab is open, five on any other screen, thirty
    // minimized or covered), sending say to the friend `to` first.
    void PollChat(const std::string& to = "", const std::string& say = "") {
        if (!profileLoaded || chatBusy || profile.session.empty()) return;
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG every = chatTabOpen ? 3000u : background ? 30000u : 5000u;
        if (say.empty() && chatAt != 0 && now - chatAt < every) return;
        if (chatThread.joinable()) chatThread.join();
        chatAt = now;
        chatBusy = true;
        long long after, afterGroups;
        {
            std::lock_guard guard(chatLock);
            after = chatLast;
            afterGroups = groupLast;
        }
        chatThread = std::thread([this, to, say, after, afterGroups, root = library.Root(), session = profile.session] {
            std::string problem;
            if (!say.empty()) {
                const HttpResult sent = IsGroup(to) ? CentralCall(root, "POST", "/grupo/mensagem", "grupo\t" + to.substr(2) + "\ntexto\t" + NoTabs(say) + "\n", session, 8000)
                                                    : CentralCall(root, "POST", "/mensagem", "para\t" + to + "\ntexto\t" + NoTabs(say) + "\n", session, 8000);
                if (sent.status == 0) problem = "A mensagem não foi: o servidor do Ranga não respondeu.";
                else if (sent.status != 200) problem = ReplyField(sent.body, "erro");
                if (sent.status == 401) sessionExpired = true;
            }
            const HttpResult got = CentralCall(root, "GET", "/mensagens?desde=" + std::to_string(after) + "&desdeg=" + std::to_string(afterGroups), "", session, 8000);
            std::vector<DirectMessage> fresh, freshGroups;
            std::set<std::string> typing;
            std::map<std::string, long long> reads;
            for (size_t at = 0; got.status == 200 && at < got.body.size();) {
                size_t end = got.body.find('\n', at);
                if (end == std::string::npos) end = got.body.size();
                const std::string line = got.body.substr(at, end - at);
                at = end + 1;
                std::vector<std::string> parts;
                for (size_t from = 0;;) {
                    const size_t tab = line.find('\t', from);
                    parts.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                    if (tab == std::string::npos) break;
                    from = tab + 1;
                }
                if (parts.size() == 6 && parts[0] == "m") fresh.push_back({std::atoll(parts[1].c_str()), std::atoll(parts[2].c_str()), parts[3], parts[4], parts[5]});
                // A group's: g, number, when, group, from, text (to is "g:<group>").
                else if (parts.size() == 6 && parts[0] == "g") freshGroups.push_back({std::atoll(parts[1].c_str()), std::atoll(parts[2].c_str()), parts[4], "g:" + parts[3], parts[5]});
                else if (parts.size() == 2 && parts[0] == "d") typing.insert(parts[1]);
                else if (parts.size() == 3 && parts[0] == "l") reads[parts[1]] = std::atoll(parts[2].c_str());
            }
            std::lock_guard guard(chatLock);
            if (got.status == 200) {
                chatTyping = std::move(typing);
                for (const auto& [id, number] : reads) chatReadBy[id] = std::max(chatReadBy[id], number);
                for (auto& message : fresh) {
                    if (message.number <= chatLast) continue;
                    chatLast = message.number;
                    chat.push_back(std::move(message));
                }
                for (auto& message : freshGroups) {
                    if (message.number <= groupLast) continue;
                    groupLast = message.number;
                    chat.push_back(std::move(message));
                }
                if (chat.size() > 2000) chat.erase(chat.begin(), chat.begin() + static_cast<std::ptrdiff_t>(chat.size() - 2000));
                chatFetched = true;
            } else if (problem.empty() && got.status == 0) {
                problem = "O servidor do Ranga não respondeu.";
            }
            chatProblem = problem;
            chatBusy = false;
        });
    }

    // Where this account's "read up to" per friend is kept on this PC, so messages that came while Ranga was closed
    // still show as new.
    std::wstring ChatSeenFile() const { return library.Root() + L"\\perfil\\conversas-" + Wide(profile.accountId) + L".txt"; }

    void SaveChatSeen() {
        std::string text;
        for (const auto& [id, number] : chatSeen) text += id + "\t" + std::to_string(number) + "\n";
        HANDLE file = CreateFileW(ChatSeenFile().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        DWORD put = 0;
        WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &put, nullptr);
        CloseHandle(file);
    }

    // New messages from friends: a notice (unless that conversation is open) and the unread counts.
    void NoticeChat() {
        std::vector<DirectMessage> fresh;
        int waiting = 0;
        {
            std::lock_guard guard(chatLock);
            if (!chatPrimed) {
                if (!chatFetched) return;
                // Read up to where this PC last saw; the first time on this PC, what is already there is not news.
                const std::string saved = ReadBytes(ChatSeenFile());
                if (saved.empty()) {
                    for (const auto& message : chat) if (message.from != profile.accountId) chatSeen[ConvOf(message)] = std::max(chatSeen[ConvOf(message)], message.number);
                } else {
                    for (size_t at = 0; at < saved.size();) {
                        size_t end = saved.find('\n', at);
                        if (end == std::string::npos) end = saved.size();
                        const std::string line = saved.substr(at, end - at);
                        at = end + 1;
                        const size_t tab = line.find('\t');
                        if (tab != std::string::npos) chatSeen[line.substr(0, tab)] = std::atoll(line.c_str() + tab + 1);
                    }
                    for (const auto& message : chat) waiting += message.from != profile.accountId && message.number > chatSeen[ConvOf(message)];
                }
                chatNoticed = chatLast;
                groupNoticed = groupLast;
                chatPrimed = true;
            }
            for (const auto& message : chat) {
                const bool inGroup = IsGroup(message.to);
                if (message.number <= (inGroup ? groupNoticed : chatNoticed) || message.from == profile.accountId) continue;
                const std::string conv = ConvOf(message);
                if (chatTabOpen && conv == chatPeer) chatSeen[conv] = std::max(chatSeen[conv], message.number);
                else fresh.push_back(message);
            }
            chatNoticed = chatLast;
            groupNoticed = groupLast;
            chatUnread = 0;
            for (const auto& message : chat) chatUnread += message.from != profile.accountId && message.number > chatSeen[ConvOf(message)];
            // Read here or in the open conversation since the last save.
            if (chatPrimed && chatSeen != chatSeenSaved) {
                SaveChatSeen();
                chatSeenSaved = chatSeen;
            }
        }
        if (waiting > 0) AddToast("Enquanto o Ranga estava fechado", waiting == 1 ? "1 mensagem nova" : std::to_string(waiting) + " mensagens novas", "");
        for (const auto& message : fresh) {
            Invite invite;
            const std::string conv = ConvOf(message);
            const std::string where = IsGroup(conv) ? " em " + FriendName(conv) : "";
            if (ReadInvite(message.text, invite)) {
                AddToast(FriendName(message.from) + " convidou para jogar" + where, invite.title + "  ·  clique para entrar", "", message.from,
                         [this, invite] { pending = [this, invite] { JoinInvite(invite); }; });
            } else {
                AddToast(IsGroup(conv) ? FriendName(message.from) + where : "Mensagem de " + FriendName(message.from), ShownText(message.text), "", message.from,
                         [this, conv] { OpenChatWith(conv); });
            }
        }
    }

    // The way in, shown where an account is needed.
    void AccountPrompt(float scale) {
        ImGui::Dummy(ImVec2(0, 10 * scale));
        ImGui::PushTextWrapPos(0);
        ImGui::TextUnformatted("Para ter amigos e conversar, crie sua conta do Ranga (ou entre, se já tiver).");
        ImGui::TextDisabled("Só nome e senha, sem e-mail. Seus amigos te acham pelo seu Nome#1234 e só eles veem o seu perfil.");
        ImGui::PopTextWrapPos();
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
        if (ImGui::Button("Criar conta", ImVec2(170 * scale, 36 * scale))) OpenAccount(1);
        ImGui::PopStyleColor(2);
        ImGui::SameLine();
        if (ImGui::Button("Entrar", ImVec2(130 * scale, 36 * scale))) OpenAccount(0);
    }

    // ---- Playing together: invites, "Entrar no jogo", pictures, "digitando..." and "visto" ----

    // A game invite travels as a chat message: "[[convite|<console>|<titleId>|<modo>|<endereço>|<título>]]". modo is the
    // online the game was set to (classicos, switch or 3ds) and endereço its room; empty: just the game. A Ranga from
    // before this shows it as text. The same text goes in the card ("sala") while playing online.
    struct Invite {
        std::string system, titleId, mode, address, title;
    };
    static std::string NoBars(std::string text) {
        for (char& c : text) if (c == '|' || c == '\t' || c == '\n' || c == '\r' || c == '[' || c == ']') c = ' ';
        return text;
    }
    static std::string InviteText(const Invite& invite) {
        return "[[convite|" + NoBars(invite.system) + "|" + NoBars(invite.titleId) + "|" + NoBars(invite.mode) + "|" + NoBars(invite.address) + "|" +
               NoBars(invite.title.substr(0, 120)) + "]]";
    }
    static bool ReadInvite(const std::string& text, Invite& out) {
        if (text.size() < 12 || text.rfind("[[convite|", 0) != 0 || text.compare(text.size() - 2, 2, "]]") != 0) return false;
        const std::string inner = text.substr(10, text.size() - 12);
        std::vector<std::string> parts;
        for (size_t from = 0;;) {
            const size_t bar = inner.find('|', from);
            parts.push_back(inner.substr(from, bar == std::string::npos ? std::string::npos : bar - from));
            if (bar == std::string::npos) break;
            from = bar + 1;
        }
        if (parts.size() != 5) return false;
        out = {parts[0], parts[1], parts[2], parts[3], parts[4]};
        return !out.system.empty() && !out.title.empty();
    }
    // A picture sent in the chat: "[[imagem|<id>]]" (the central keeps it 30 days).
    static std::string ImageId(const std::string& text) {
        if (text.size() == 43 && text.rfind("[[imagem|", 0) == 0 && text.compare(41, 2, "]]") == 0) return text.substr(9, 32);
        return "";
    }
    // How a message reads in a list or a notice.
    static std::string ShownText(const std::string& text) {
        Invite invite;
        if (ReadInvite(text, invite)) return "Convite para jogar " + invite.title;
        if (!ImageId(text).empty()) return "Imagem";
        return text;
    }

    // The invite to a game as this PC has it set: its room when it plays online.
    Invite InviteFor(const Game& game) {
        Invite invite{Utf8(game.system), Utf8(game.titleId), "", "", Utf8(game.title)};
        if (game.system == L"SWITCH" && ReadCoreOption(game, "eden_sala") == "entrar") {
            invite.mode = "switch";
            const std::string port = ReadCoreOption(game, "eden_sala_porta");
            invite.address = ReadCoreOption(game, "eden_sala_endereco");
            if (!invite.address.empty() && !port.empty()) invite.address += ":" + port;
        } else if (game.system == L"3DS" && ReadCoreOption(game, "citra_sala") == "entrar") {
            invite.mode = "3ds";
            invite.address = ReadCoreOption(game, "citra_sala_endereco");
        } else if (ClassicOnline(game.system) && ReadCoreOption(game, "ranga_online") == "ligado") {
            invite.mode = "classicos";
            invite.address = ReadCoreOption(game, "ranga_online_endereco");
        }
        if (invite.address.empty()) invite.mode.clear();
        return invite;
    }

    // The game of an invite in this library: same console and title id, else same console and name; -1 none.
    int InviteGame(const Invite& invite) {
        const std::wstring system = Wide(invite.system), titleId = Wide(invite.titleId), title = Lower(Wide(invite.title));
        int byName = -1;
        for (size_t i = 0; i < library.Games().size(); ++i) {
            const Game& game = library.Games()[i];
            if (game.system != system) continue;
            if (!titleId.empty() && game.titleId == titleId) return static_cast<int>(i);
            if (byName < 0 && Lower(game.title) == title) byName = static_cast<int>(i);
        }
        return byName;
    }

    // Accepting an invite: the game set to the same room, then opened (the room is tested first, Play).
    void JoinInvite(const Invite& invite) {
        const int index = InviteGame(invite);
        if (index < 0) {
            Notify(L"Você não tem " + Wide(invite.title) + L" (" + Wide(SystemName(Wide(invite.system))) + L") na sua biblioteca. Adicione o jogo para jogar junto.");
            return;
        }
        if (runner.Busy()) {
            Notify(L"Feche o jogo que está aberto antes de entrar em outro.");
            return;
        }
        Game& game = library.Games()[static_cast<size_t>(index)];
        if (invite.mode == "switch") {
            std::string host = invite.address, port;
            if (const size_t colon = host.rfind(':'); colon != std::string::npos) port = host.substr(colon + 1), host.resize(colon);
            WriteCoreOption(game, "eden_sala", "entrar");
            WriteCoreOption(game, "eden_sala_endereco", host);
            if (!port.empty()) WriteCoreOption(game, "eden_sala_porta", port);
        } else if (invite.mode == "3ds") {
            WriteCoreOption(game, "citra_sala", "entrar");
            WriteCoreOption(game, "citra_sala_endereco", invite.address);
        } else if (invite.mode == "classicos") {
            WriteCoreOption(game, "ranga_online", "ligado");
            WriteCoreOption(game, "ranga_online_endereco", invite.address);
        }
        Play(game);
    }

    // The profile's Chat tab with that friend, from anywhere.
    void OpenChatWith(const std::string& id) {
        OpenProfile();
        chatPeer = id;
        openChatTab = true;
    }

    // Messages written wait here for the chat's turn (a fetch may be running); one goes each frame the chat is free.
    std::deque<std::pair<std::string, std::string>> chatOutbox;
    void QueueChat(const std::string& to, const std::string& text) { chatOutbox.emplace_back(to, text); }
    void SendQueuedChat() {
        if (chatOutbox.empty() || chatBusy || !profileLoaded || profile.session.empty()) {
            PollChat();
            return;
        }
        const auto [to, text] = chatOutbox.front();
        chatOutbox.pop_front();
        PollChat(to, text);
    }

    // A small call to the central that nobody waits for ("digitando", "lido").
    void Signal(const std::string& path, const std::string& body) {
        if (profile.session.empty()) return;
        std::thread([root = library.Root(), session = profile.session, path, body] { CentralCall(root, "POST", path, body, session, 6000); }).detach();
    }

    // Who is typing to this account now and, per friend, up to which of this account's messages they read (PollChat).
    std::set<std::string> chatTyping;
    std::map<std::string, long long> chatReadBy, chatReadSent;
    ULONGLONG typingSentAt = 0;
    std::atomic<bool> imageBusy{false};
    std::string imageShown; // the picture open big ("" none)

    // A picture to a friend: shrunk to 960 at most, ~340 KB, sent as an image message.
    void SendChatImage(const std::string& to, const std::wstring& path) {
        if (imageBusy || profile.session.empty()) return;
        imageBusy = true;
        std::thread([this, to, path, root = library.Root(), session = profile.session] {
            Media media;
            std::string error;
            std::string bytes;
            if (ImportPicture(path, 960, media, error)) bytes = EncodeMedia(media, 340 * 1024);
            std::string problem;
            if (bytes.empty()) {
                problem = error.empty() ? "Não deu para converter essa imagem." : error;
            } else {
                const std::string where = IsGroup(to) ? "grupo\t" + to.substr(2) : "para\t" + to;
                const HttpResult sent = CentralCall(root, "POST", "/imagem", where + "\ndados\t" + Base64(bytes) + "\n", session, 60000);
                if (sent.status == 0) problem = "A imagem não foi: o servidor do Ranga não respondeu.";
                else if (sent.status == 404) problem = "O servidor do Ranga ainda não recebe imagens (falta atualizar a central).";
                else if (sent.status != 200) problem = ReplyField(sent.body, "erro");
            }
            {
                std::lock_guard guard(chatLock);
                chatProblem = problem;
            }
            chatAt = 0;
            imageBusy = false;
        }).detach();
    }

    // A chat picture as a texture (downloaded and decoded by the media thread); null while it comes.
    const AnimTexture* ChatImage(const std::string& id) {
        CachedMedia& cached = mediaCache[id + "|imagem"];
        cached.usedAt = ImGui::GetTime();
        if (cached.version != id && mediaAsked.insert(id + "|imagem|" + id).second) {
            {
                std::lock_guard guard(mediaLock);
                mediaQueue.push_back({id, "imagem", id});
            }
            StartMediaDownloads();
        }
        if (cached.anim.frames.size() > 1) animating = true;
        return cached.anim.frames.empty() ? nullptr : &cached.anim;
    }

    // ---- Friends coming online and starting games: a notice each ----

    std::map<std::string, std::pair<bool, std::string>> friendState; // online, playing
    long long friendStateAt = 0;
    bool friendStatePrimed = false;

    static bool FriendOnline(const PlayerCard& card) { return !card.closed && static_cast<long long>(std::time(nullptr)) - card.seen < 300; }

    void NoticeFriends() {
        std::vector<PlayerCard> list;
        {
            std::lock_guard guard(friendsLock);
            if (friendsAt == friendStateAt) return;
            friendStateAt = friendsAt;
            list = friends;
        }
        for (const auto& card : list) {
            const bool online = FriendOnline(card);
            auto& known = friendState[card.id];
            if (friendStatePrimed) {
                Invite invite;
                const bool joinable = ReadInvite(card.invite, invite) && !invite.mode.empty();
                const std::function<void()> open = [this, id = card.id] { OpenChatWith(id); };
                const std::function<void()> join = [this, invite] { pending = [this, invite] { JoinInvite(invite); }; };
                if (online && !card.playing.empty() && card.playing != known.second) {
                    AddToast(card.name + (joinable ? " está jogando online" : " começou a jogar"), card.playing, "", card.id, joinable ? join : open, false);
                } else if (online && !known.first) {
                    AddToast(card.name + " está online", "Clique para conversar", "", card.id, open, false);
                }
            }
            known = {online, online ? card.playing : std::string()};
        }
        friendStatePrimed = true;
    }

    // ---- Amigos: the list at the right, over any page (the top bar's Amigos) ----

    bool friendsPanel = false;

    int FriendsOnline() {
        std::lock_guard guard(friendsLock);
        return static_cast<int>(std::count_if(friends.begin(), friends.end(), [](const PlayerCard& card) { return FriendOnline(card); }));
    }

    void FriendsPanel(float scale) {
        if (!friendsPanel) return;
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const float width = 330 * scale, top = 70 * scale;
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - width - 12 * scale, viewport->WorkPos.y + top));
        ImGui::SetNextWindowSize(ImVec2(width, viewport->WorkSize.y - top - 12 * scale));
        ImGui::PushStyleColor(ImGuiCol_WindowBg, Color(kSidebar, 0.98f));
        ImGui::PushStyleColor(ImGuiCol_Border, Color(kSelected));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 14 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(14 * scale, 14 * scale));
        ImGui::Begin("##friendsPanel", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);

        ImGui::PushFont(semibold, 20);
        ImGui::TextUnformatted("Amigos");
        ImGui::PopFont();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 30 * scale);
        if (ImGui::Button("X##fecharAmigos", ImVec2(30 * scale, 0))) friendsPanel = false;
        ImGui::Dummy(ImVec2(0, 4 * scale));
        if (profile.session.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Entre na sua conta do Ranga para ver seus amigos aqui.");
            ImGui::PopTextWrapPos();
            if (ImGui::Button("Entrar ou criar conta", ImVec2(-1, 36 * scale))) {
                friendsPanel = false;
                OpenProfile();
            }
        } else {
            std::vector<PlayerCard> list;
            {
                std::lock_guard guard(friendsLock);
                list = friends;
            }
            std::map<std::string, int> unread;
            {
                std::lock_guard guard(chatLock);
                for (const auto& message : chat) if (message.from != profile.accountId && !IsGroup(message.to) && message.number > chatSeen[message.from]) ++unread[message.from];
            }
            std::vector<const PlayerCard*> playing, online, offline;
            for (const auto& card : list) (FriendOnline(card) ? (card.playing.empty() ? online : playing) : offline).push_back(&card);
            std::sort(offline.begin(), offline.end(), [](const PlayerCard* a, const PlayerCard* b) { return a->seen > b->seen; });
            ImGui::BeginChild("##friendsPanelList", ImVec2(0, -46 * scale), ImGuiChildFlags_None);
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float listWidth = ImGui::GetContentRegionAvail().x;
            // The gamepad (Big Picture): up and down pick a friend, A opens the chat, Y joins their game, B closes.
            const int rows = static_cast<int>(playing.size() + online.size() + offline.size());
            if (panelNav.down) ++panelFocus;
            if (panelNav.up) --panelFocus;
            panelFocus = std::clamp(panelFocus, 0, std::max(0, rows - 1));
            const bool padHere = gamepads.Connected() || _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr;
            int rowIndex = 0;
            const auto group = [&](const char* name, const std::vector<const PlayerCard*>& cards) {
                if (cards.empty()) return;
                ImGui::Dummy(ImVec2(0, 4 * scale));
                ImGui::TextDisabled("%s  (%zu)", name, cards.size());
                ImGui::Dummy(ImVec2(0, 2 * scale));
                for (const PlayerCard* card : cards) {
                    const bool on = FriendOnline(*card);
                    Invite invite;
                    const bool joinable = on && ReadInvite(card->invite, invite) && !invite.mode.empty();
                    const float row = 54 * scale, side = 36 * scale;
                    const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + listWidth, min.y + row);
                    ImGui::PushID(card->id.c_str());
                    const float buttonWidth = joinable ? 64 * scale : 0.0f;
                    const bool padOn = padHere && rowIndex++ == panelFocus;
                    if (padOn) ImGui::SetScrollHereY(0.5f);
                    if (ImGui::InvisibleButton("##amigo", ImVec2(listWidth - buttonWidth, row)) || (padOn && panelNav.accept)) {
                        friendsPanel = false;
                        pending = [this, id = card->id] { OpenChatWith(id); };
                    }
                    if (padOn && panelNav.search && joinable) {
                        friendsPanel = false;
                        pending = [this, invite] { JoinInvite(invite); };
                    }
                    if (ImGui::IsItemHovered() || padOn) draw->AddRectFilled(min, max, Packed(kHover), 10 * scale);
                    if (padOn) Focus(draw, min, max, scale, 10 * scale);
                    DrawAvatar(draw, ImVec2(min.x + 8 * scale, min.y + (row - side) / 2), side, *card, scale);
                    const ImVec2 dot(min.x + 8 * scale + side - 1 * scale, min.y + (row + side) / 2 - 1 * scale);
                    draw->AddCircleFilled(dot, 6 * scale, Packed(kSidebar), 16);
                    draw->AddCircleFilled(dot, 4.5f * scale, Packed(on ? kGreen : kDim), 16);
                    const float textX = min.x + side + 22 * scale;
                    const ImVec4 clip(textX, min.y, max.x - buttonWidth - 8 * scale, max.y);
                    draw->AddText(semibold, 15 * scale, ImVec2(textX, min.y + 9 * scale), Packed(on ? kText : kDim), card->name.c_str(), nullptr, 0, &clip);
                    const std::string status = !on ? "Visto " + FormatAgo(card->seen) : card->playing.empty() ? "Online" : card->playing;
                    draw->AddText(nullptr, 13 * scale, ImVec2(textX, min.y + 29 * scale), Packed(on && !card->playing.empty() ? kGreen : kDim), status.c_str(), nullptr, 0, &clip);
                    if (const int count = unread[card->id]; count > 0) {
                        const std::string number = std::to_string(count);
                        const ImVec2 center(max.x - buttonWidth - 16 * scale, min.y + row / 2);
                        draw->AddCircleFilled(center, 10 * scale, Packed(kAccent), 20);
                        const ImVec2 size = ImGui::CalcTextSize(number.c_str());
                        draw->AddText(ImVec2(center.x - size.x / 2, center.y - size.y / 2), Packed(0xffffff), number.c_str());
                    }
                    if (joinable) {
                        ImGui::SameLine(0, 0);
                        ImGui::SetCursorScreenPos(ImVec2(max.x - buttonWidth, min.y + (row - 28 * scale) / 2));
                        ImGui::PushStyleColor(ImGuiCol_Button, Color(kGreen));
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kGreen, 0.85f));
                        if (ImGui::Button("Entrar", ImVec2(buttonWidth, 28 * scale))) {
                            friendsPanel = false;
                            pending = [this, invite] { JoinInvite(invite); };
                        }
                        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abrir %s na mesma sala", invite.title.c_str());
                        ImGui::PopStyleColor(2);
                        ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
                    }
                    ImGui::PopID();
                    ImGui::Dummy(ImVec2(0, 2 * scale));
                }
            };
            group("Jogando", playing);
            group("Online", online);
            group("Offline", offline);
            if (list.empty()) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Nenhum amigo ainda. Adicione pelo Nome#1234 na aba Amigos do Perfil.");
                ImGui::PopTextWrapPos();
            } else if (padHere) {
                ImGui::Dummy(ImVec2(0, 6 * scale));
                ImGui::TextDisabled("A: conversar    Y: entrar no jogo    B: fechar");
            }
            ImGui::EndChild();
            if (ImGui::Button("Adicionar amigo", ImVec2(-1, 36 * scale))) {
                friendsPanel = false;
                pending = [this] { OpenProfile(); };
            }
        }
        ImGui::End();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
    }

    // ---- Atividade dos amigos (the home page): what they played and unlocked lately ----

    struct Activity {
        long long when = 0;
        std::string id, what, detail;
    };

    std::vector<Activity> FriendActivity(size_t count) {
        std::vector<Activity> out;
        const long long since = static_cast<long long>(std::time(nullptr)) - 14 * 86400;
        std::lock_guard guard(friendsLock);
        for (const auto& card : friends) {
            for (const auto& game : card.recent) {
                if (game.last < since) continue;
                out.push_back({game.last, card.id, card.name + " jogou " + Utf8(game.title), SystemName(game.system)});
            }
            for (const auto& item : card.showcase) {
                if (item.when < since) continue;
                out.push_back({item.when, card.id, card.name + " desbloqueou " + item.title, item.ra ? "RetroAchievements" : "Conquista do Ranga"});
            }
        }
        std::sort(out.begin(), out.end(), [](const Activity& a, const Activity& b) { return a.when > b.when; });
        if (out.size() > count) out.resize(count);
        return out;
    }

    // Under Continuar jogando, as far as the page goes.
    void ActivityPanel(ImDrawList* draw, ImVec2 at, float width, float bottom, float scale) {
        if (profile.session.empty() || bottom - at.y < 110 * scale) return;
        const float row = 50 * scale;
        const size_t fits = static_cast<size_t>(std::max(0.0f, (bottom - at.y - 34 * scale) / row));
        const auto items = FriendActivity(std::min<size_t>(fits, 6));
        if (items.empty()) return;
        draw->AddText(semibold, 18 * scale, at, Packed(kText), "Atividade dos amigos");
        float y = at.y + 32 * scale;
        std::map<std::string, PlayerCard> cards;
        {
            std::lock_guard guard(friendsLock);
            for (const auto& card : friends) cards[card.id] = card;
        }
        for (size_t k = 0; k < items.size(); ++k) {
            const Activity& item = items[k];
            const ImVec2 min(at.x, y), max(at.x + width, y + row - 6 * scale);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(static_cast<int>(k) + 200000);
            if (ImGui::InvisibleButton("##atividade", ImVec2(width, row - 6 * scale))) pending = [this, id = item.id] { OpenProfile(); profileViewing = id; };
            const bool lit = ImGui::IsItemHovered();
            ImGui::PopID();
            draw->AddRectFilled(min, max, Packed(lit ? kSelected : kSurface, 0.85f), 10 * scale);
            const float side = 30 * scale;
            if (const auto found = cards.find(item.id); found != cards.end()) DrawAvatar(draw, ImVec2(min.x + 8 * scale, min.y + (max.y - min.y - side) / 2), side, found->second, scale);
            const float textX = min.x + side + 18 * scale;
            const ImVec4 clip(textX, min.y, max.x - 8 * scale, max.y);
            draw->AddText(nullptr, 14 * scale, ImVec2(textX, min.y + 5 * scale), Packed(kText), item.what.c_str(), nullptr, 0, &clip);
            const std::string line = item.detail + "  ·  " + FormatAgo(item.when);
            draw->AddText(nullptr, 12 * scale, ImVec2(textX, min.y + 24 * scale), Packed(kDim), line.c_str(), nullptr, 0, &clip);
            y += row;
        }
    }

    // ---- Capturas (F12 in the game, RangaRetro): <Ranga>\capturas\<game>\*.png ----

    std::wstring CapturesFolder(const Game& game) const {
        std::wstring name = game.title;
        for (wchar_t& c : name) if (wcschr(L"\\/:*?\"<>|", c) != nullptr || c < 32) c = L'_';
        while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
        if (name.empty()) name = game.titleId;
        return library.Root() + L"\\capturas\\" + name;
    }

    struct Capture {
        std::wstring path;
        long long when = 0;
    };
    std::vector<Capture> Captures(const Game& game) {
        std::vector<Capture> out;
        WIN32_FIND_DATAW found;
        HANDLE search = FindFirstFileW((CapturesFolder(game) + L"\\*.png").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE) return out;
        do {
            ULARGE_INTEGER time;
            time.LowPart = found.ftLastWriteTime.dwLowDateTime;
            time.HighPart = found.ftLastWriteTime.dwHighDateTime;
            out.push_back({CapturesFolder(game) + L"\\" + found.cFileName, static_cast<long long>(time.QuadPart / 10000000ULL) - 11644473600LL});
        } while (FindNextFileW(search, &found));
        FindClose(search);
        std::sort(out.begin(), out.end(), [](const Capture& a, const Capture& b) { return a.when > b.when; });
        return out;
    }

    // Thumbnails of the captures, read once (a new file gets its own).
    std::map<std::wstring, Texture> captureThumbs;
    std::wstring captureShown; // the capture open big
    std::map<std::wstring, std::vector<Capture>> capturesListed;
    double capturesListedAt = -10;

    const Texture& CaptureThumb(const std::wstring& path) {
        auto found = captureThumbs.find(path);
        if (found == captureThumbs.end()) found = captureThumbs.emplace(path, LoadTexture(path, 480)).first;
        return found->second;
    }

    // The game page's Capturas: the newest, a click opens one big, to send to a friend.
    void CapturesSection(const Game& game, float scale) {
        if (ImGui::GetTime() - capturesListedAt > 3.0) {
            capturesListed.clear();
            capturesListedAt = ImGui::GetTime();
        }
        auto listed = capturesListed.find(game.titleId);
        if (listed == capturesListed.end()) listed = capturesListed.emplace(game.titleId, Captures(game)).first;
        const auto& shots = listed->second;
        SectionTitle("Capturas", scale);
        if (shots.empty()) {
            ImGui::TextDisabled("Aperte F12 durante o jogo para tirar uma captura de tela. Elas aparecem aqui.");
            return;
        }
        ImGui::TextDisabled("%zu captura%s  ·  F12 no jogo tira mais", shots.size(), shots.size() == 1 ? "" : "s");
        ImGui::SameLine();
        if (ImGui::SmallButton("Abrir pasta")) ShellExecuteW(nullptr, L"open", CapturesFolder(game).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::Dummy(ImVec2(0, 4 * scale));
        const float thumbW = 192 * scale, thumbH = 108 * scale, gap = 12 * scale;
        const float width = ImGui::GetContentRegionAvail().x;
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 start = ImGui::GetCursorScreenPos();
        size_t shown = 0;
        for (const auto& shot : shots) {
            const float x = start.x + shown * (thumbW + gap);
            if (x + thumbW > start.x + width || shown >= 8) break;
            const ImVec2 min(x, start.y), max(x + thumbW, start.y + thumbH);
            const Texture& thumb = CaptureThumb(shot.path);
            if (thumb.view != nullptr) draw->AddImageRounded(thumb.Ref(), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 8 * scale);
            else draw->AddRectFilled(min, max, Packed(kSurface), 8 * scale);
            ImGui::SetCursorScreenPos(min);
            ImGui::PushID(static_cast<int>(shown) + 300000);
            if (ImGui::InvisibleButton("##captura", ImVec2(thumbW, thumbH))) {
                captureShown = shot.path;
                ImGui::OpenPopup("##capturaGrande");
            }
            if (ImGui::IsItemHovered()) draw->AddRect(min, max, Packed(kAccent), 8 * scale, 0, 2 * scale);
            ImGui::PopID();
            ++shown;
        }
        ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + thumbH));
        ImGui::Dummy(ImVec2(0, 4 * scale));
        CaptureViewer(scale);
    }

    void CaptureViewer(float scale) {
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x / 2, viewport->WorkPos.y + viewport->WorkSize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopup("##capturaGrande")) return;
        const Texture& thumb = CaptureThumb(captureShown);
        const float maxW = viewport->WorkSize.x * 0.75f, maxH = viewport->WorkSize.y * 0.7f;
        if (thumb.view != nullptr) {
            float w = maxW, h = w * thumb.height / std::max(1, thumb.width);
            if (h > maxH) h = maxH, w = h * thumb.width / std::max(1, thumb.height);
            ImGui::Image(thumb.Ref(), ImVec2(w, h));
        }
        if (ImGui::Button("Abrir o arquivo")) ShellExecuteW(nullptr, L"open", captureShown.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        std::vector<PlayerCard> list;
        {
            std::lock_guard guard(friendsLock);
            list = friends;
        }
        ImGui::BeginDisabled(list.empty() || imageBusy);
        if (ImGui::Button(imageBusy ? "Mandando..." : "Mandar para um amigo")) ImGui::OpenPopup("##capturaPara");
        ImGui::EndDisabled();
        if (ImGui::BeginPopup("##capturaPara")) {
            for (const auto& card : list) {
                if (ImGui::Selectable(card.name.c_str())) {
                    SendChatImage(card.id, captureShown);
                    AddToast("Captura", "Mandando para " + card.name + "...", "");
                }
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Fechar")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // The emoji the chat's button offers (Segoe UI Emoji, merged into the fonts).
    static constexpr const char* kEmoji[] = {
        "😀", "😂", "🤣", "😅", "😊", "😍", "😎", "🤔", "😴", "😭", "😡", "🤯", "🥳", "😱", "🙄", "😏",
        "👍", "👎", "👏", "🙏", "💪", "🤝", "👀", "🔥", "💯", "❤", "💀", "🎮", "🕹", "🏆", "⭐", "🎉"};

    void ProfileChat(float scale) {
        chatTabOpen = true;
        if (profile.session.empty()) {
            AccountPrompt(scale);
            return;
        }
        std::vector<PlayerCard> list;
        std::vector<ChatGroup> myGroups;
        {
            std::lock_guard guard(friendsLock);
            list = friends;
            myGroups = groups;
        }
        const std::vector<PlayerCard> friendCards = list;
        // The groups come first, each as a card of its own ("g:<id>" and its name, no photo).
        {
            std::vector<PlayerCard> withGroups;
            for (const auto& group : myGroups) {
                PlayerCard card;
                card.id = "g:" + group.id;
                card.name = group.name;
                card.closed = true;
                withGroups.push_back(card);
            }
            withGroups.insert(withGroups.end(), list.begin(), list.end());
            list = std::move(withGroups);
        }
        if (list.empty()) {
            ImGui::Dummy(ImVec2(0, 10 * scale));
            ImGui::TextDisabled("Adicione amigos na aba Amigos para conversar.");
            return;
        }
        const auto groupOf = [&](const std::string& key) -> const ChatGroup* {
            for (const auto& group : myGroups) if ("g:" + group.id == key) return &group;
            return nullptr;
        };
        std::vector<DirectMessage> messages;
        std::map<std::string, int> unread;
        std::string problem;
        long long seenNow = 0, readByPeer = 0;
        bool peerTyping = false;
        {
            std::lock_guard guard(chatLock);
            if (std::none_of(list.begin(), list.end(), [&](const PlayerCard& c) { return c.id == chatPeer; })) chatPeer = list.front().id;
            for (const auto& message : chat) {
                const std::string conv = ConvOf(message);
                if (message.from != profile.accountId) unread[conv] += message.number > chatSeen[conv];
                if (conv == chatPeer) messages.push_back(message);
            }
            // Open is read.
            for (const auto& message : messages) if (message.from != profile.accountId) chatSeen[chatPeer] = std::max(chatSeen[chatPeer], message.number);
            unread[chatPeer] = 0;
            problem = chatProblem;
            seenNow = chatSeen[chatPeer];
            peerTyping = chatTyping.count(chatPeer) > 0;
            readByPeer = chatReadBy[chatPeer];
        }
        const bool inGroup = IsGroup(chatPeer);
        // The friend learns how far this side read ("Visto"), once per new message (not in groups).
        if (!inGroup && seenNow > chatReadSent[chatPeer]) {
            chatReadSent[chatPeer] = seenNow;
            Signal("/lido", "de\t" + chatPeer + "\nnumero\t" + std::to_string(seenNow) + "\n");
        }
        if (messages.size() != chatShown) {
            chatShown = messages.size();
            chatScroll = true;
        }
        const long long now = static_cast<long long>(std::time(nullptr));
        const auto isOnline = [&](const PlayerCard& card) { return !card.closed && now - card.seen < 300; };
        const auto statusOf = [&](const PlayerCard& card) {
            if (const ChatGroup* group = groupOf(card.id)) {
                std::string names;
                for (const auto& [account, name] : group->members) {
                    if (account == profile.accountId) continue;
                    names += (names.empty() ? "" : ", ") + name.substr(0, name.rfind('#'));
                }
                return std::to_string(group->members.size()) + " pessoas" + (names.empty() ? "" : ": você, " + names);
            }
            if (!isOnline(card)) return "Visto " + FormatAgo(card.seen);
            return card.playing.empty() ? std::string("Online agora") : "Jogando " + card.playing;
        };
        // The last message with each friend, for the preview under the name.
        std::map<std::string, DirectMessage> lastWith;
        {
            std::lock_guard guard(chatLock);
            for (const auto& message : chat) {
                const std::string other = ConvOf(message);
                if (lastWith[other].number < message.number || lastWith[other].when < message.when) lastWith[other] = message;
            }
        }
        const PlayerCard* peer = nullptr;
        for (const auto& card : list) if (card.id == chatPeer) peer = &card;
        // A friend on a Ranga from before groups, invites and pictures sees those as odd text: a line says so.
        const bool oldPeer = peer != nullptr && !inGroup && (peer->version.empty() || OlderVersion(peer->version, kSocialVersion));
        ImGui::Dummy(ImVec2(0, 6 * scale));

        // ---- The friends, on the left: photo with the online dot, name, last message or status, unread count.
        const float listWidth = 290 * scale, rowHeight = 64 * scale, avatar = 42 * scale;
        // The whole chat fits what is left of the page, the box to write in always in view.
        const float total = std::max(ImGui::GetContentRegionAvail().y - (problem.empty() ? 30 : 56) * scale, 300 * scale);
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(kSidebar));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * scale);
        ImGui::BeginChild("##chatFriends", ImVec2(listWidth, total), ImGuiChildFlags_None);
        ImDrawList* listDraw = ImGui::GetWindowDrawList();
        ImGui::Dummy(ImVec2(0, 6 * scale));
        // A new group: a name and which friends go in.
        ImGui::SetCursorPosX(10 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8 * scale);
        if (ImGui::Button("+  Novo grupo", ImVec2(listWidth - 20 * scale, 34 * scale))) {
            newGroupName.fill(0);
            newGroupPick.clear();
            ImGui::OpenPopup("##novoGrupo");
        }
        ImGui::PopStyleVar();
        if (ImGui::BeginPopup("##novoGrupo")) {
            ImGui::PushFont(semibold, 18);
            ImGui::TextUnformatted("Novo grupo");
            ImGui::PopFont();
            ImGui::SetNextItemWidth(300 * scale);
            ImGui::InputTextWithHint("##nomeGrupo", "Nome do grupo", newGroupName.data(), newGroupName.size());
            ImGui::TextDisabled("Quem entra (até 15 amigos):");
            int picked = 0;
            for (const auto& card : friendCards) {
                bool& on = newGroupPick[card.id];
                if (ImGui::Checkbox((card.name + "##g" + card.id).c_str(), &on)) {}
                picked += on;
            }
            if (friendCards.empty()) ImGui::TextDisabled("Você ainda não tem amigos para pôr no grupo.");
            ImGui::BeginDisabled(newGroupName[0] == 0 || picked == 0 || picked > 15 || actionBusy);
            if (ImGui::Button("Criar grupo", ImVec2(160 * scale, 34 * scale))) {
                std::string members;
                for (const auto& [id, on] : newGroupPick) if (on) members += (members.empty() ? "" : ",") + id;
                FriendAction("/grupo/criar", "nome\t" + NoTabs(newGroupName.data()) + "\nmembros\t" + members + "\n");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancelar", ImVec2(110 * scale, 34 * scale))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        ImGui::Dummy(ImVec2(0, 4 * scale));
        for (const auto& card : list) {
            const bool chosen = card.id == chatPeer;
            const ImVec2 min = ImGui::GetCursorScreenPos();
            const ImVec2 rowMin(min.x + 6 * scale, min.y), rowMax(min.x + listWidth - 6 * scale, min.y + rowHeight);
            ImGui::PushID(card.id.c_str());
            ImGui::SetCursorScreenPos(rowMin);
            if (ImGui::InvisibleButton("##friend", ImVec2(rowMax.x - rowMin.x, rowHeight))) chatPeer = card.id;
            const bool hovered = ImGui::IsItemHovered();
            ImGui::PopID();
            if (chosen || hovered) listDraw->AddRectFilled(rowMin, rowMax, Packed(chosen ? kSelected : kHover), 10 * scale);
            const ImVec2 photo(rowMin.x + 10 * scale, rowMin.y + (rowHeight - avatar) / 2);
            DrawAvatar(listDraw, photo, avatar, card, scale);
            // The online dot on the photo's corner, ringed in the row's color (a group has none).
            if (!IsGroup(card.id)) {
                const ImVec2 dot(photo.x + avatar - 2 * scale, photo.y + avatar - 2 * scale);
                listDraw->AddCircleFilled(dot, 7 * scale, Packed(chosen ? kSelected : kSidebar), 16);
                listDraw->AddCircleFilled(dot, 5 * scale, Packed(isOnline(card) ? kGreen : kDim), 16);
            }
            const float textX = photo.x + avatar + 14 * scale, textRight = rowMax.x - 12 * scale;
            const int count = unread[card.id];
            const float badge = count > 0 ? 26 * scale : 0.0f;
            const ImVec4 clip(textX, rowMin.y, textRight - badge, rowMax.y);
            listDraw->AddText(semibold, 16 * scale, ImVec2(textX, rowMin.y + 13 * scale), Packed(kText), card.name.c_str(), nullptr, 0, &clip);
            std::string preview = statusOf(card);
            if (const auto found = lastWith.find(card.id); found != lastWith.end() && found->second.number > 0) {
                const std::string who = found->second.from == profile.accountId ? "Você: " : IsGroup(card.id) ? FriendName(found->second.from) + ": " : "";
                preview = who + ShownText(found->second.text);
            }
            {
                std::lock_guard guard(chatLock);
                if (chatTyping.count(card.id) > 0) preview = "digitando...";
            }
            listDraw->AddText(nullptr, 13 * scale, ImVec2(textX, rowMin.y + 35 * scale), Packed(count > 0 ? kText : kDim), preview.c_str(), nullptr, 0, &clip);
            if (count > 0) {
                const std::string number = count > 99 ? "99+" : std::to_string(count);
                const ImVec2 size = ImGui::CalcTextSize(number.c_str());
                const ImVec2 center(textRight - 10 * scale, rowMin.y + rowHeight / 2);
                const float radius = std::max(10 * scale, size.x / 2 + 6 * scale);
                listDraw->AddCircleFilled(center, radius, Packed(kAccent), 24);
                listDraw->AddText(ImVec2(center.x - size.x / 2, center.y - size.y / 2), Packed(0xffffff), number.c_str());
            }
            ImGui::Dummy(ImVec2(0, 4 * scale));
        }
        ImGui::EndChild();
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();

        // ---- The conversation, on the right: who it is with, the messages in bubbles, the box to write in.
        ImGui::SameLine(0, 14 * scale);
        ImGui::BeginGroup();
        const float right = ImGui::GetContentRegionAvail().x;
        const float headerHeight = 62 * scale, inputHeight = 52 * scale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(kSidebar));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 12 * scale);
        ImGui::BeginChild("##chatHeader", ImVec2(right, headerHeight), ImGuiChildFlags_None, ImGuiWindowFlags_NoScrollbar);
        if (peer != nullptr) {
            ImDrawList* head = ImGui::GetWindowDrawList();
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const float side = 40 * scale;
            DrawAvatar(head, ImVec2(at.x + 12 * scale, at.y + (headerHeight - side) / 2), side, *peer, scale);
            head->AddText(semibold, 18 * scale, ImVec2(at.x + side + 28 * scale, at.y + 11 * scale), Packed(kText), peer->name.c_str());
            if (!inGroup) head->AddCircleFilled(ImVec2(at.x + side + 33 * scale, at.y + 42 * scale), 4 * scale, Packed(isOnline(*peer) ? kGreen : kDim), 12);
            const float statusX = at.x + side + (inGroup ? 28 : 42) * scale;
            const ImVec4 statusClip(statusX, at.y, at.x + right - 420 * scale, at.y + headerHeight);
            head->AddText(nullptr, 13 * scale, ImVec2(statusX, at.y + 34 * scale), Packed(kDim), statusOf(*peer).c_str(), nullptr, 0, &statusClip);
            // Joining what the friend plays online, and inviting them to a game of this library; the group's or the
            // friend's own menu.
            float buttonsX = right - 16 * scale;
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 8 * scale);
            buttonsX -= 44 * scale;
            ImGui::SetCursorScreenPos(ImVec2(at.x + buttonsX, at.y + (headerHeight - 34 * scale) / 2));
            if (ImGui::Button(inGroup ? "···##grupo" : "···##conversa", ImVec2(44 * scale, 34 * scale))) ImGui::OpenPopup(inGroup ? "##grupo" : "##conversa");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip(inGroup ? "Membros, nome e sair do grupo" : "Perfil, bloquear e denunciar");
            buttonsX -= 198 * scale;
            ImGui::SetCursorScreenPos(ImVec2(at.x + buttonsX, at.y + (headerHeight - 34 * scale) / 2));
            if (ImGui::Button("Convidar para jogar", ImVec2(190 * scale, 34 * scale))) ImGui::OpenPopup("##convidar");
            Invite theirs;
            if (!inGroup && isOnline(*peer) && ReadInvite(peer->invite, theirs) && !theirs.mode.empty()) {
                buttonsX -= 150 * scale;
                ImGui::SetCursorScreenPos(ImVec2(at.x + buttonsX, at.y + (headerHeight - 34 * scale) / 2));
                ImGui::PushStyleColor(ImGuiCol_Button, Color(kGreen));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kGreen, 0.85f));
                if (ImGui::Button("Entrar no jogo", ImVec2(140 * scale, 34 * scale))) pending = [this, theirs] { JoinInvite(theirs); };
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s, na mesma sala", theirs.title.c_str());
                ImGui::PopStyleColor(2);
            }
            ImGui::PopStyleVar();
            ChatMenus(peer, groupOf(chatPeer), friendCards, scale);
            if (ImGui::BeginPopup("##convidar")) {
                ImGui::TextDisabled("Convidar %s para:", peer->name.c_str());
                ImGui::Separator();
                const auto recent = RecentGames(12);
                if (recent.empty()) ImGui::TextDisabled("Jogue algum jogo primeiro: os últimos aparecem aqui.");
                for (const int index : recent) {
                    const Game& game = library.Games()[static_cast<size_t>(index)];
                    const Invite invite = InviteFor(game);
                    const std::string label = Utf8(game.title) + "  (" + SystemName(game.system) + (invite.mode.empty() ? ")" : ", online)") + "##" + Utf8(game.titleId);
                    if (ImGui::Selectable(label.c_str())) {
                        QueueChat(chatPeer, InviteText(invite));
                        chatScroll = true;
                    }
                }
                ImGui::EndPopup();
            }
        }
        ImGui::EndChild();
        ImGui::Dummy(ImVec2(0, 4 * scale));
        const float bannerHeight = oldPeer ? 30 * scale : 0.0f;
        if (oldPeer) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 8 * scale);
            ImGui::TextUnformatted(("⚠ " + peer->name + " usa um Ranga mais antigo: convites, imagens e grupos podem chegar como texto estranho. Peça para atualizar.").c_str());
            ImGui::PopStyleColor();
            ImGui::Dummy(ImVec2(0, bannerHeight - ImGui::GetTextLineHeightWithSpacing()));
        }

        ImGui::BeginChild("##chatLog", ImVec2(right, total - headerHeight - inputHeight - 10 * scale - bannerHeight), ImGuiChildFlags_None);
        ImDrawList* log = ImGui::GetWindowDrawList();
        const float logWidth = ImGui::GetContentRegionAvail().x;
        const float bubbleMax = std::min(logWidth * 0.62f, 620 * scale), pad = 12 * scale, textSize = 15 * scale;
        ImGui::Dummy(ImVec2(0, 10 * scale));
        if (messages.empty()) {
            const std::string empty = (inGroup ? "Nenhuma mensagem ainda em " : "Nenhuma mensagem ainda com ") + FriendName(chatPeer) + ". Mande a primeira!";
            const ImVec2 size = ImGui::CalcTextSize(empty.c_str());
            ImGui::SetCursorPos(ImVec2((logWidth - size.x) / 2, ImGui::GetCursorPosY() + 60 * scale));
            ImGui::TextDisabled("%s", empty.c_str());
        }
        const auto dayOf = [](long long when) {
            const time_t at = static_cast<time_t>(when);
            tm local{};
            localtime_s(&local, &at);
            return local.tm_year * 1000 + local.tm_yday;
        };
        int lastDay = -1;
        long long lastMine = 0;
        for (const auto& message : messages) if (message.from == profile.accountId) lastMine = message.number;
        for (size_t i = 0; i < messages.size(); ++i) {
            const auto& message = messages[i];
            const bool mine = message.from == profile.accountId;
            // A line with the day between days: Hoje, Ontem or the date.
            if (const int day = dayOf(message.when); day != lastDay) {
                lastDay = day;
                const time_t at = static_cast<time_t>(message.when);
                tm local{};
                localtime_s(&local, &at);
                char date[16];
                std::snprintf(date, sizeof(date), "%02d/%02d", local.tm_mday, local.tm_mon + 1);
                const std::string label = day == dayOf(now) ? "Hoje" : day == dayOf(now - 86400) ? "Ontem" : date;
                const ImVec2 size = ImGui::CalcTextSize(label.c_str());
                const ImVec2 at2 = ImGui::GetCursorScreenPos();
                const float middle = at2.x + logWidth / 2;
                log->AddRectFilled(ImVec2(middle - size.x / 2 - 12 * scale, at2.y + 4 * scale), ImVec2(middle + size.x / 2 + 12 * scale, at2.y + size.y + 12 * scale),
                                   Packed(kSelected), 10 * scale);
                log->AddText(ImVec2(middle - size.x / 2, at2.y + 8 * scale), Packed(kDim), label.c_str());
                ImGui::Dummy(ImVec2(0, size.y + 22 * scale));
            }
            // Messages from the same person a few minutes apart sit together; the time goes under the last one.
            const bool groupEnds = i + 1 == messages.size() || messages[i + 1].from != message.from || messages[i + 1].when - message.when > 300 ||
                                   dayOf(messages[i + 1].when) != dayOf(message.when);
            // In a group, the name of who wrote, over the first of their messages in a row.
            const bool runStarts = i == 0 || messages[i - 1].from != message.from || message.when - messages[i - 1].when > 300 || dayOf(messages[i - 1].when) != dayOf(message.when);
            if (inGroup && !mine && runStarts) {
                const ImVec2 nameAt = ImGui::GetCursorScreenPos();
                log->AddText(semibold, 13 * scale, ImVec2(nameAt.x + 20 * scale, nameAt.y), Packed(kGreen), FriendName(message.from).c_str());
                ImGui::Dummy(ImVec2(0, 19 * scale));
            }
            const ImVec2 at = ImGui::GetCursorScreenPos();
            Invite invite;
            const std::string imageId = ImageId(message.text);
            ImVec2 min, max;
            if (ReadInvite(message.text, invite)) {
                // An invite: a card with the game's art, its name and, for whoever got it, Entrar.
                const float width = std::min(360 * scale, bubbleMax), height = 96 * scale, art = 72 * scale;
                const float x = mine ? at.x + logWidth - width - 16 * scale : at.x + 16 * scale;
                min = ImVec2(x, at.y), max = ImVec2(x + width, at.y + height);
                log->AddRectFilled(min, max, Packed(kSurface), 14 * scale);
                log->AddRect(min, max, Packed(mine ? kAccent : kGreen, 0.8f), 14 * scale, 0, 1.5f * scale);
                const int index = InviteGame(invite);
                const ImVec2 artMin(min.x + 12 * scale, min.y + (height - art) / 2);
                if (index >= 0) GameArt(log, library.Games()[static_cast<size_t>(index)], artMin, ImVec2(artMin.x + art, artMin.y + art), scale * 0.7f);
                else log->AddRectFilled(artMin, ImVec2(artMin.x + art, artMin.y + art), Packed(kSelected), 8 * scale);
                const float textX = artMin.x + art + 12 * scale;
                const ImVec4 clip(textX, min.y, max.x - 10 * scale, max.y);
                log->AddText(nullptr, 12 * scale, ImVec2(textX, min.y + 12 * scale), Packed(kDim), mine ? "Você convidou para jogar" : "Convite para jogar", nullptr, 0, &clip);
                log->AddText(semibold, 16 * scale, ImVec2(textX, min.y + 28 * scale), Packed(kText), invite.title.c_str(), nullptr, 0, &clip);
                const std::string where = SystemName(Wide(invite.system)) + (invite.mode.empty() ? "" : "  ·  online");
                log->AddText(nullptr, 12 * scale, ImVec2(textX, min.y + 48 * scale), Packed(LookFor(Wide(invite.system)).name), where.c_str(), nullptr, 0, &clip);
                if (!mine) {
                    ImGui::SetCursorScreenPos(ImVec2(textX, max.y - 30 * scale));
                    ImGui::PushID(static_cast<int>(message.number));
                    ImGui::PushStyleColor(ImGuiCol_Button, Color(index >= 0 ? kGreen : kSelected));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(index >= 0 ? kGreen : kHover, 0.85f));
                    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 6 * scale);
                    if (ImGui::Button(index >= 0 ? "Entrar" : "Não tenho esse jogo", ImVec2(0, 24 * scale)) && index >= 0) pending = [this, invite] { JoinInvite(invite); };
                    ImGui::PopStyleVar();
                    ImGui::PopStyleColor(2);
                    ImGui::PopID();
                }
                ImGui::SetCursorScreenPos(at);
                ImGui::Dummy(ImVec2(0, height + (groupEnds ? 2 : 4) * scale));
            } else if (!imageId.empty()) {
                // A picture: its own shape, 300 x 240 at most; a click shows it big.
                const AnimTexture* anim = ChatImage(imageId);
                const Texture* frame = anim != nullptr ? anim->Now() : nullptr;
                float w = 240 * scale, h = 135 * scale;
                if (frame != nullptr) {
                    w = std::min(300 * scale, bubbleMax);
                    h = w * frame->height / std::max(1, frame->width);
                    if (h > 240 * scale) h = 240 * scale, w = h * frame->width / std::max(1, frame->height);
                }
                const float x = mine ? at.x + logWidth - w - 16 * scale : at.x + 16 * scale;
                min = ImVec2(x, at.y), max = ImVec2(x + w, at.y + h);
                if (frame != nullptr) {
                    log->AddImageRounded(frame->Ref(), min, max, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 12 * scale);
                    ImGui::SetCursorScreenPos(min);
                    ImGui::PushID(static_cast<int>(message.number));
                    if (ImGui::InvisibleButton("##imagem", ImVec2(w, h))) {
                        imageShown = imageId;
                        ImGui::OpenPopup("##imagemGrande");
                    }
                    ImGui::PopID();
                } else {
                    log->AddRectFilled(min, max, Packed(kSelected), 12 * scale);
                    const char* waiting = "Carregando imagem...";
                    const ImVec2 size = ImGui::CalcTextSize(waiting);
                    log->AddText(ImVec2(min.x + (w - size.x) / 2, min.y + (h - size.y) / 2), Packed(kDim), waiting);
                }
                ImGui::SetCursorScreenPos(at);
                ImGui::Dummy(ImVec2(0, h + (groupEnds ? 2 : 4) * scale));
            } else {
                const ImVec2 text = ImGui::GetFont()->CalcTextSizeA(textSize, FLT_MAX, bubbleMax - 2 * pad, message.text.c_str());
                const float width = std::max(text.x + 2 * pad, 48 * scale), height = text.y + 2 * pad * 0.8f;
                const float x = mine ? at.x + logWidth - width - 16 * scale : at.x + 16 * scale;
                min = ImVec2(x, at.y), max = ImVec2(x + width, at.y + height);
                log->AddRectFilled(min, max, Packed(mine ? kAccent : kSelected, mine ? 0.9f : 1.0f), 14 * scale);
                log->AddText(ImGui::GetFont(), textSize, ImVec2(min.x + pad, min.y + pad * 0.8f), Packed(mine ? 0xffffff : kText), message.text.c_str(), nullptr, bubbleMax - 2 * pad);
                ImGui::Dummy(ImVec2(0, height + (groupEnds ? 2 : 4) * scale));
            }
            if (groupEnds) {
                const time_t when = static_cast<time_t>(message.when);
                tm local{};
                localtime_s(&local, &when);
                char hour[24];
                std::snprintf(hour, sizeof(hour), "%02d:%02d", local.tm_hour, local.tm_min);
                // Under this side's last message: whether the friend read it.
                std::string stamp = hour;
                if (mine && message.number == lastMine && readByPeer >= message.number) stamp += "  ·  Visto";
                const ImVec2 size = ImGui::CalcTextSize(stamp.c_str());
                const ImVec2 under = ImGui::GetCursorScreenPos();
                log->AddText(nullptr, 12 * scale, ImVec2(mine ? max.x - size.x * 0.85f - 4 * scale : min.x + 4 * scale, under.y), Packed(kDim), stamp.c_str());
                ImGui::Dummy(ImVec2(0, 18 * scale));
            }
        }
        // "digitando...", with dots that come and go.
        if (peerTyping) {
            const int dots = static_cast<int>(ImGui::GetTime() * 3) % 3 + 1;
            const std::string typingText = FriendName(chatPeer) + " está digitando" + std::string(static_cast<size_t>(dots), '.');
            const ImVec2 at = ImGui::GetCursorScreenPos();
            log->AddText(nullptr, 13 * scale, ImVec2(at.x + 20 * scale, at.y), Packed(kDim), typingText.c_str());
            ImGui::Dummy(ImVec2(0, 22 * scale));
            animating = true;
        }
        // A picture open big.
        {
            const ImGuiViewport* viewport = ImGui::GetMainViewport();
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x / 2, viewport->WorkPos.y + viewport->WorkSize.y / 2), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopup("##imagemGrande")) {
                const AnimTexture* anim = ChatImage(imageShown);
                if (const Texture* frame = anim != nullptr ? anim->Now() : nullptr) {
                    const float maxW = viewport->WorkSize.x * 0.75f, maxH = viewport->WorkSize.y * 0.75f;
                    float w = static_cast<float>(frame->width) * scale, h = static_cast<float>(frame->height) * scale;
                    const float fit = std::min({1.0f, maxW / w, maxH / h});
                    ImGui::Image(frame->Ref(), ImVec2(w * fit, h * fit));
                }
                if (ImGui::Button("Fechar")) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
        }
        if (chatScroll) {
            ImGui::SetScrollHereY(1.0f);
            chatScroll = false;
        }
        ImGui::EndChild();

        // The box to write in, round, with Enviar beside it.
        ImGui::Dummy(ImVec2(0, 2 * scale));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 22 * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(18 * scale, 14 * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
        // Emoji and a picture, beside the box.
        const float small = 50 * scale;
        if (ImGui::Button("😀##emoji", ImVec2(small, 0))) ImGui::OpenPopup("##emojis");
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Emoji");
        if (ImGui::BeginPopup("##emojis")) {
            for (size_t e = 0; e < std::size(kEmoji); ++e) {
                if (e % 8 != 0) ImGui::SameLine(0, 4 * scale);
                ImGui::PushID(static_cast<int>(e));
                if (ImGui::Button(kEmoji[e], ImVec2(40 * scale, 40 * scale))) {
                    const std::string now2 = std::string(chatInput.data()) + kEmoji[e];
                    if (now2.size() + 1 < chatInput.size()) std::snprintf(chatInput.data(), chatInput.size(), "%s", now2.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndPopup();
        }
        ImGui::SameLine(0, 6 * scale);
        ImGui::BeginDisabled(imageBusy);
        if (ImGui::Button(imageBusy ? "...##imagem" : "🖼##imagem", ImVec2(small, 0))) {
            pending = [this, to = chatPeer] {
                Browse(L"Mandar uma imagem", L"", false, {{L"Imagens", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif"}}, [this, to](const std::wstring& path) {
                    SendChatImage(to, path);
                    OpenChatWith(to);
                });
            };
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip(imageBusy ? "Mandando a imagem..." : "Mandar uma imagem (PNG, JPG ou GIF)");
        ImGui::SameLine(0, 6 * scale);
        ImGui::SetNextItemWidth(right - 140 * scale - 2 * (small + 6 * scale));
        const std::string hint = "Mensagem para " + FriendName(chatPeer) + (gamepads.Connected() ? "...   (Y: escrever com o controle)" : "...");
        const bool enter = ImGui::InputTextWithHint("##chatInput", hint.c_str(), chatInput.data(), chatInput.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        // The friend sees "digitando..." (told every 3 s while the text changes; not in groups).
        if (!inGroup && ImGui::IsItemEdited() && chatInput[0] != '\0' && GetTickCount64() - typingSentAt > 3000) {
            typingSentAt = GetTickCount64();
            Signal("/digitando", "para\t" + chatPeer + "\n");
        }
        ImGui::PopStyleColor();
        ImGui::SameLine(0, 10 * scale);
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, Color(kAccent));
        ImGui::PushStyleColor(ImGuiCol_Text, Color(0xffffff));
        const bool send = ImGui::Button(chatOutbox.empty() ? "Enviar" : "Enviando...", ImVec2(130 * scale, 0)) || enter;
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar(2);
        if (send) {
            const std::string text = chatInput.data();
            if (text.find_first_not_of(' ') != std::string::npos) {
                QueueChat(chatPeer, text);
                chatInput.fill('\0');
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
        if (!problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextUnformatted(problem.c_str());
            ImGui::PopStyleColor();
        }
        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        ImGui::EndGroup();
    }

    // New group: what is typed and who is ticked; the group's rename field; the report's reason.
    std::array<char, 61> newGroupName{}, groupRename{};
    std::map<std::string, bool> newGroupPick;
    std::array<char, 301> reportReason{};

    // The "···" menus of the chat's header: a friend's (profile, block, report) or a group's (members, name, leave).
    void ChatMenus(const PlayerCard* peer, const ChatGroup* group, const std::vector<PlayerCard>& friendCards, float scale) {
        if (ImGui::BeginPopup("##conversa")) {
            if (ImGui::Selectable("Ver perfil")) {
                profileViewing = peer->id;
            }
            if (ImGui::Selectable("Bloquear")) {
                const std::string id = peer->id, who = peer->name;
                Confirm(L"Bloquear " + Wide(who) + L"? A amizade acaba e essa pessoa não consegue mais te pedir amizade nem falar com você.", [this, id] {
                    FriendAction("/bloquear", "conta\t" + id + "\n");
                    chatPeer.clear();
                });
            }
            if (ImGui::Selectable("Denunciar...")) {
                reportReason.fill(0);
                reportFor = peer->id;
                openReport = true;
            }
            ImGui::EndPopup();
        }
        if (openReport) {
            ImGui::OpenPopup("##denunciar");
            openReport = false;
        }
        if (ImGui::BeginPopup("##denunciar")) {
            ImGui::PushFont(semibold, 18);
            ImGui::Text("Denunciar %s", FriendName(reportFor).c_str());
            ImGui::PopFont();
            ImGui::PushTextWrapPos(460 * scale);
            ImGui::TextDisabled("A denúncia vai para o dono do Ranga junto com as últimas 30 mensagens de vocês, para ele ver o que aconteceu. A pessoa não fica sabendo.");
            ImGui::PopTextWrapPos();
            ImGui::InputTextMultiline("##motivo", reportReason.data(), reportReason.size(), ImVec2(460 * scale, 90 * scale));
            ImGui::BeginDisabled(reportReason[0] == 0 || actionBusy);
            if (ImGui::Button("Enviar denúncia", ImVec2(170 * scale, 34 * scale))) {
                FriendAction("/denunciar", "conta\t" + reportFor + "\nmotivo\t" + NoTabs(reportReason.data()) + "\n");
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancelar", ImVec2(110 * scale, 34 * scale))) ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }
        if (group == nullptr) return;
        if (ImGui::BeginPopup("##grupo")) {
            const bool owner = group->owner == profile.accountId;
            ImGui::PushFont(semibold, 18);
            ImGui::TextUnformatted(group->name.c_str());
            ImGui::PopFont();
            if (owner) {
                if (ImGui::IsWindowAppearing()) std::snprintf(groupRename.data(), groupRename.size(), "%s", group->name.c_str());
                ImGui::SetNextItemWidth(220 * scale);
                ImGui::InputText("##renomear", groupRename.data(), groupRename.size());
                ImGui::SameLine();
                ImGui::BeginDisabled(groupRename[0] == 0 || group->name == groupRename.data() || actionBusy);
                if (ImGui::Button("Renomear")) FriendAction("/grupo/renomear", "grupo\t" + group->id + "\nnome\t" + NoTabs(groupRename.data()) + "\n");
                ImGui::EndDisabled();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Membros (%zu de 16)", group->members.size());
            for (const auto& [account, name] : group->members) {
                ImGui::TextUnformatted((name + (account == group->owner ? "  (criou)" : "") + (account == profile.accountId ? "  (você)" : "")).c_str());
                if (owner && account != profile.accountId) {
                    ImGui::SameLine();
                    ImGui::PushID(account.c_str());
                    if (ImGui::SmallButton("Tirar")) {
                        const std::string id = group->id, who = account;
                        Confirm(L"Tirar " + Wide(name) + L" do grupo?", [this, id, who] { FriendAction("/grupo/remover", "grupo\t" + id + "\nconta\t" + who + "\n"); });
                    }
                    ImGui::PopID();
                }
            }
            std::vector<const PlayerCard*> outside;
            for (const auto& card : friendCards) {
                if (std::none_of(group->members.begin(), group->members.end(), [&](const auto& m) { return m.first == card.id; })) outside.push_back(&card);
            }
            if (!outside.empty() && group->members.size() < 16) {
                ImGui::Separator();
                ImGui::TextDisabled("Pôr um amigo no grupo:");
                for (const PlayerCard* card : outside) {
                    if (ImGui::Selectable(("+  " + card->name + "##add" + card->id).c_str()) && !actionBusy)
                        FriendAction("/grupo/adicionar", "grupo\t" + group->id + "\nconta\t" + card->id + "\n");
                }
            }
            ImGui::Separator();
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kRed));
            if (ImGui::Selectable("Sair do grupo")) {
                const std::string id = group->id, name = group->name;
                Confirm(L"Sair de " + Wide(name) + L"? Você para de ver as mensagens dele." + (owner ? L" O grupo passa para o membro mais antigo." : L""), [this, id] {
                    FriendAction("/grupo/sair", "grupo\t" + id + "\n");
                    chatPeer.clear();
                });
            }
            ImGui::PopStyleColor();
            ImGui::EndPopup();
        }
    }
    std::string reportFor;
    bool openReport = false;

    void ProfileFriends(float scale) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        if (profile.session.empty()) {
            AccountPrompt(scale);
            return;
        }
        std::vector<PlayerCard> list;
        std::vector<std::pair<std::string, std::string>> incoming, outgoing;
        std::string problem;
        long long at = 0;
        {
            std::lock_guard guard(friendsLock);
            list = friends;
            incoming = friendsIncoming;
            outgoing = friendsOutgoing;
            problem = friendsProblem;
            at = friendsAt;
        }
        const long long now = static_cast<long long>(std::time(nullptr));
        std::sort(list.begin(), list.end(), [&](const PlayerCard& a, const PlayerCard& b) {
            const bool aOn = !a.closed && now - a.seen < 300, bOn = !b.closed && now - b.seen < 300;
            if (aOn != bOn) return aOn;
            return a.seen > b.seen;
        });
        ImGui::Dummy(ImVec2(0, 8 * scale));
        // This player's Nome#1234, for friends to type.
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("Sua conta:");
        ImGui::SameLine();
        ImGui::PushFont(semibold, 18);
        ImGui::TextUnformatted(profile.accountName.c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        if (ImGui::SmallButton("Copiar")) {
            ImGui::SetClipboardText(profile.accountName.c_str());
            AddToast("Conta do Ranga", "Copiado: " + profile.accountName, "");
        }
        ImGui::SameLine(0, 20 * scale);
        ImGui::BeginDisabled(profileBusy);
        if (ImGui::SmallButton(profileBusy ? "Atualizando..." : "Atualizar")) SyncProfiles(true);
        ImGui::EndDisabled();
        if (!problem.empty()) {
            ImGui::SameLine();
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextUnformatted(problem.c_str());
            ImGui::PopStyleColor();
        } else if (at != 0) {
            ImGui::SameLine();
            ImGui::TextDisabled("atualizado %s", FormatAgo(at).c_str());
        }

        // Adding someone by Nome#1234.
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
        ImGui::SetNextItemWidth(260 * scale);
        const bool enter = ImGui::InputTextWithHint("##friendAdd", "Nome#1234 do amigo", friendAddField.data(), friendAddField.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled(actionBusy || friendAddField[0] == '\0');
        if (ImGui::Button("Pedir amizade") || (enter && friendAddField[0] != '\0')) {
            FriendAction("/amigos/pedir", "nome\t" + NoTabs(friendAddField.data()) + "\n");
            friendAddField.fill('\0');
        }
        ImGui::EndDisabled();
        if (!friendAction.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("%s", friendAction.c_str());
        }

        // Requests.
        if (!incoming.empty()) {
            SectionTitle("Pedidos de amizade", scale);
            for (const auto& [id, name] : incoming) {
                ImGui::PushID(id.c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(name.c_str());
                ImGui::SameLine(300 * scale);
                ImGui::BeginDisabled(actionBusy);
                ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
                if (ImGui::Button("Aceitar")) FriendAction("/amigos/responder", "conta\t" + id + "\naceitar\t1\n");
                ImGui::PopStyleColor();
                ImGui::SameLine();
                if (ImGui::Button("Recusar")) FriendAction("/amigos/responder", "conta\t" + id + "\naceitar\t0\n");
                ImGui::SameLine();
                if (ImGui::Button("Bloquear")) FriendAction("/bloquear", "conta\t" + id + "\n");
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        if (!outgoing.empty()) {
            SectionTitle("Esperando resposta", scale);
            for (const auto& [id, name] : outgoing) {
                ImGui::PushID(id.c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", name.c_str());
                ImGui::SameLine(300 * scale);
                ImGui::BeginDisabled(actionBusy);
                if (ImGui::Button("Cancelar pedido")) FriendAction("/amigos/remover", "conta\t" + id + "\n");
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }

        char heading[64];
        std::snprintf(heading, sizeof(heading), "Amigos   (%d)", static_cast<int>(list.size()));
        SectionTitle(heading, scale);
        if (list.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Nenhum amigo ainda. Passe o seu Nome#1234 para eles, ou peça a amizade pelo deles aqui em cima.");
            ImGui::PopTextWrapPos();
        }
        const float cardWidth = 300 * scale, cardHeight = 96 * scale, gap = 14 * scale;
        const int columns = std::max(1, static_cast<int>((ImGui::GetContentRegionAvail().x + gap) / (cardWidth + gap)));
        for (size_t i = 0; i < list.size(); ++i) {
            const PlayerCard& card = list[i];
            if (i % columns != 0) ImGui::SameLine(0, gap);
            const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + cardWidth, min.y + cardHeight);
            ImGui::PushID(card.id.c_str());
            const bool clicked = ImGui::InvisibleButton("##friend", ImVec2(cardWidth, cardHeight));
            const bool hovered = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            ImGui::PopID();
            draw->AddRectFilled(min, max, Packed(hovered ? kHover : kSurface), 10 * scale);
            DrawCover(draw, min, max, card, 10 * scale, hovered ? 0.7f : 0.88f);
            const float side = 64 * scale;
            DrawAvatar(draw, ImVec2(min.x + 16 * scale, min.y + 16 * scale), side, card, scale * 0.6f);
            const float x = min.x + 16 * scale + side + 16 * scale;
            draw->AddText(semibold, 18 * scale, ImVec2(x, min.y + 14 * scale), Packed(kText), card.name.c_str());
            const std::string level = "Nível " + std::to_string(LevelFor(card.xp).level) + "   ·   " + FormatPlayed(card.seconds);
            draw->AddText(nullptr, ImGui::GetFontSize() * 0.92f, ImVec2(x, min.y + 40 * scale), Packed(kDim), level.c_str());
            const bool online = !card.closed && now - card.seen < 300;
            const std::string status = online ? (card.playing.empty() ? "Online" : "Jogando " + card.playing) : "Visto " + FormatAgo(card.seen);
            const ImVec4 clip(x, min.y, max.x - 8 * scale, max.y);
            draw->AddText(nullptr, ImGui::GetFontSize() * 0.92f, ImVec2(x, min.y + 62 * scale), Packed(online ? kGreen : kDim), status.c_str(), nullptr, 0, &clip);
            if (clicked) pending = [this, id = card.id] { profileViewing = id; };
        }

        std::vector<std::pair<std::string, std::string>> blockedList;
        {
            std::lock_guard guard(friendsLock);
            blockedList = friendsBlocked;
        }
        if (!blockedList.empty()) {
            SectionTitle("Bloqueados", scale);
            ImGui::TextDisabled("Não te pedem amizade nem conversam com você. Para eles, você não existe.");
            for (const auto& [id, name] : blockedList) {
                ImGui::PushID(id.c_str());
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", name.c_str());
                ImGui::SameLine(300 * scale);
                ImGui::BeginDisabled(actionBusy);
                if (ImGui::Button("Desbloquear")) FriendAction("/desbloquear", "conta\t" + id + "\n");
                ImGui::EndDisabled();
                ImGui::PopID();
            }
        }
        ImGui::Dummy(ImVec2(0, 12 * scale));
        OwnerPanel(scale);
        if (ImGui::CollapsingHeader("Servidor (avançado)")) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Onde ficam as contas, os amigos e as conversas. Vazio: o do Ranga, na Cloudflare.");
            ImGui::PopTextWrapPos();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
            ImGui::SetNextItemWidth(380 * scale);
            ImGui::InputTextWithHint("##central", "https://ranga-central.nadroi-dev.workers.dev", centralField.data(), centralField.size());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Salvar servidor")) {
                SetCentralServer(library.Root(), centralField.data());
                profileSyncedAt = 0;
            }
        }
    }

    // Fetches the accounts (search: part of a name), after banning or unbanning one when id is given.
    void OwnerFetch(const std::string& path = "", const std::string& id = "", const std::string& reason = "") {
        if (ownerBusy) return;
        if (ownerThread.joinable()) ownerThread.join();
        ownerBusy = true;
        ownerThread = std::thread([this, path, id, reason, search = std::string(ownerSearch.data()), root = library.Root()] {
            std::string problem;
            if (!path.empty()) {
                const std::string body = path == "/dono/denuncia" ? "id\t" + id + "\n" : "conta\t" + id + "\nmotivo\t" + NoTabs(reason) + "\n";
                const HttpResult done = OwnerCall(root, "POST", path, body);
                if (done.status != 200) problem = done.status == 0 ? "O servidor do Ranga não respondeu." : ReplyField(done.body, "erro");
            }
            const HttpResult reported = OwnerCall(root, "GET", "/dono/denuncias", "");
            std::vector<OwnerReport> reports;
            for (size_t at = 0; reported.status == 200 && at < reported.body.size();) {
                size_t end = reported.body.find('\n', at);
                if (end == std::string::npos) end = reported.body.size();
                const std::string line = reported.body.substr(at, end - at);
                at = end + 1;
                std::vector<std::string> parts;
                for (size_t from = 0;;) {
                    const size_t tab = line.find('\t', from);
                    parts.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                    if (tab == std::string::npos) break;
                    from = tab + 1;
                }
                if (parts.size() >= 8 && parts[0] == "d") reports.push_back({parts[1], parts[3], parts[4], parts[5], parts[6], parts[7], std::atoll(parts[2].c_str())});
            }
            {
                std::lock_guard guard(ownerLock);
                if (reported.status == 200) ownerReports = std::move(reports);
            }
            const HttpResult got = OwnerCall(root, "GET", "/dono/contas?busca=" + UrlEncode(search), "");
            std::vector<OwnerRow> rows;
            for (size_t at = 0; got.status == 200 && at < got.body.size();) {
                size_t end = got.body.find('\n', at);
                if (end == std::string::npos) end = got.body.size();
                const std::string line = got.body.substr(at, end - at);
                at = end + 1;
                std::vector<std::string> parts;
                for (size_t from = 0;;) {
                    const size_t tab = line.find('\t', from);
                    parts.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                    if (tab == std::string::npos) break;
                    from = tab + 1;
                }
                if (parts.size() >= 7 && parts[0] == "c") rows.push_back({parts[1], parts[2], parts[6], std::atoll(parts[3].c_str()), std::atoll(parts[4].c_str()), parts[5] == "1"});
            }
            if (got.status != 200 && problem.empty()) problem = got.status == 0 ? "O servidor do Ranga não respondeu." : got.status == 403 ? "A chave de dono não confere." : "Erro " + std::to_string(got.status) + ".";
            std::lock_guard guard(ownerLock);
            if (got.status == 200) ownerRows = std::move(rows);
            ownerProblem = problem;
            ownerBusy = false;
        });
    }

    // Only on the owner's PC: every account, and banning one.
    void OwnerPanel(float scale) {
        if (!ownerHere || !ImGui::CollapsingHeader("Dono do Ranga")) return;
        ImGui::PushTextWrapPos(0);
        ImGui::TextDisabled("Só aparece neste PC (salas\\chave-dono.txt; nunca vai na atualização). Banir fecha as sessões da conta, desfaz as "
                            "amizades dela e impede o PC dela de criar outra conta. Desbanir desfaz.");
        ImGui::PopTextWrapPos();
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
        ImGui::SetNextItemWidth(240 * scale);
        const bool enter = ImGui::InputTextWithHint("##ownerSearch", "buscar pelo nome (vazio: todas)", ownerSearch.data(), ownerSearch.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(260 * scale);
        ImGui::InputTextWithHint("##ownerReason", "motivo do banimento", ownerReason.data(), ownerReason.size());
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled(ownerBusy);
        if (ImGui::Button(ownerBusy ? "Buscando..." : "Buscar") || enter) OwnerFetch();
        ImGui::EndDisabled();
        std::vector<OwnerRow> rows;
        std::vector<OwnerReport> reports;
        std::string problem;
        {
            std::lock_guard guard(ownerLock);
            rows = ownerRows;
            reports = ownerReports;
            problem = ownerProblem;
        }
        if (!problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextUnformatted(problem.c_str());
            ImGui::PopStyleColor();
        }
        // The reports first: each with the messages before it, Banir (the reported account) and Arquivar.
        if (!reports.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::Text("Denúncias abertas: %zu", reports.size());
            ImGui::PopStyleColor();
            for (const auto& report : reports) {
                ImGui::PushID(("d" + report.id).c_str());
                ImGui::Bullet();
                ImGui::TextWrapped("%s denunciou %s (%s): %s", report.from.c_str(), report.againstName.c_str(), FormatAgo(report.when).c_str(), report.reason.c_str());
                ImGui::Indent(20 * scale);
                if (ImGui::TreeNode("Mensagens antes da denúncia")) {
                    ImGui::PushTextWrapPos(0);
                    for (size_t from = 0; from < report.excerpt.size();) {
                        size_t mark = report.excerpt.find(" \xc2\xb6 ", from);
                        const std::string line = report.excerpt.substr(from, mark == std::string::npos ? std::string::npos : mark - from);
                        // "<when> denunciante: text": the time as a date.
                        const size_t space = line.find(' ');
                        const long long when = std::atoll(line.c_str());
                        ImGui::TextDisabled("%s", (when > 0 && space != std::string::npos ? FormatDate(when) + " " + line.substr(space + 1) : line).c_str());
                        if (mark == std::string::npos) break;
                        from = mark + 4;
                    }
                    if (report.excerpt.empty()) ImGui::TextDisabled("(nenhuma mensagem entre os dois)");
                    ImGui::PopTextWrapPos();
                    ImGui::TreePop();
                }
                ImGui::BeginDisabled(ownerBusy);
                if (ImGui::SmallButton(("Banir " + report.againstName).c_str())) {
                    const std::string id = report.against, why = ownerReason[0] != 0 ? std::string(ownerReason.data()) : "Denúncia: " + report.reason;
                    Confirm(L"Banir " + Wide(report.againstName) + L"? A conta sai do Ranga e o PC dela não cria outra.", [this, id, why] { OwnerFetch("/dono/banir", id, why); });
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("Arquivar")) OwnerFetch("/dono/denuncia", report.id);
                ImGui::EndDisabled();
                ImGui::Unindent(20 * scale);
                ImGui::PopID();
            }
            ImGui::Separator();
        }
        if (rows.empty()) {
            ImGui::TextDisabled("Clique em Buscar para ver as contas.");
            return;
        }
        if (ImGui::BeginTable("##ownerTable", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_ScrollY, ImVec2(0, 260 * scale))) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Conta", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("Criada", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Visto", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("Estado", ImGuiTableColumnFlags_WidthStretch, 2.0f);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableHeadersRow();
            for (const auto& row : rows) {
                ImGui::PushID(row.id.c_str());
                ImGui::TableNextRow(ImGuiTableRowFlags_None, 28 * scale);
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(row.name.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", FormatDate(row.created).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", row.seen > 0 ? FormatAgo(row.seen).c_str() : "nunca");
                ImGui::TableNextColumn();
                if (row.banned) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(kRed));
                    ImGui::Text("banida%s%s", row.reason.empty() ? "" : ": ", row.reason.c_str());
                    ImGui::PopStyleColor();
                } else {
                    ImGui::TextDisabled("ativa");
                }
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(ownerBusy);
                if (row.banned) {
                    if (ImGui::SmallButton("Desbanir")) OwnerFetch("/dono/desbanir", row.id);
                } else if (ImGui::SmallButton("Banir")) {
                    const std::string id = row.id, reason = ownerReason.data();
                    Confirm(L"Banir " + Wide(row.name) + L"? A conta sai do Ranga e o PC dela não cria outra.", [this, id, reason] { OwnerFetch("/dono/banir", id, reason); });
                }
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void AccountWindow(float scale) {
        if (!accountOpen) return;
        if (!ImGui::IsPopupOpen("##account")) ImGui::OpenPopup("##account");
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(560 * scale, viewport->WorkSize.x - 40 * scale), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * scale, 22 * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Color(kSelected));
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kSelected));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(0x34353c));
        if (ImGui::BeginPopupModal("##account", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            if (!accountRecovery.empty()) {
                // Right after creating the account (or recovering it): the code, once.
                ImGui::PushFont(semibold, 22);
                ImGui::TextUnformatted("Conta criada!");
                ImGui::PopFont();
                ImGui::Text("Sua conta: %s", profile.accountName.c_str());
                ImGui::Dummy(ImVec2(0, 6 * scale));
                ImGui::PushTextWrapPos(0);
                ImGui::TextUnformatted("Guarde este código de recuperação. Ele é o único jeito de voltar para a conta se você esquecer a senha, e ele não aparece de novo:");
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0, 4 * scale));
                ImGui::PushFont(semibold, 26);
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                ImGui::TextUnformatted(accountRecovery.c_str());
                ImGui::PopStyleColor();
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0, 6 * scale));
                if (ImGui::Button("Copiar código", ImVec2(160 * scale, 36 * scale))) ImGui::SetClipboardText(accountRecovery.c_str());
                ImGui::SameLine();
                ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
                if (ImGui::Button("Já guardei", ImVec2(140 * scale, 36 * scale))) {
                    accountRecovery.clear();
                    accountOpen = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::PopStyleColor(2);
                ImGui::EndPopup();
                ImGui::PopStyleColor(4);
                ImGui::PopStyleVar();
                return;
            }
            ImGui::PushFont(semibold, 22);
            ImGui::TextUnformatted("Conta do Ranga");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 4 * scale));
            if (ImGui::RadioButton("Entrar##modo", accountMode == 0)) accountMode = 0, accountProblem.clear();
            ImGui::SameLine();
            if (ImGui::RadioButton("Criar conta##modo", accountMode == 1)) accountMode = 1, accountProblem.clear();
            ImGui::SameLine();
            if (ImGui::RadioButton("Esqueci a senha##modo", accountMode == 2)) accountMode = 2, accountProblem.clear();
            ImGui::Dummy(ImVec2(0, 6 * scale));
            ImGui::TextDisabled(accountMode == 1 ? "Nome (3 a 20 letras ou números)" : "Sua conta (Nome#1234)");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##accName", accName.data(), accName.size());
            if (accountMode == 2) {
                ImGui::TextDisabled("Código de recuperação");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##accCode", accCode.data(), accCode.size(), ImGuiInputTextFlags_CharsUppercase);
            }
            ImGui::TextDisabled(accountMode == 2 ? "Senha nova" : "Senha");
            ImGui::SetNextItemWidth(-1);
            bool enter = ImGui::InputText("##accPass", accPass.data(), accPass.size(), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
            if (accountMode == 1) {
                ImGui::TextDisabled("Repita a senha");
                ImGui::SetNextItemWidth(-1);
                enter = ImGui::InputText("##accPass2", accPass2.data(), accPass2.size(), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue) || enter;
            }
            ImGui::PushTextWrapPos(0);
            if (accountMode == 1) ImGui::TextDisabled("Sem e-mail. A tag (#1234) o Ranga sorteia. Ao criar, aparece um código de recuperação: guarde.");
            else if (accountMode == 2) ImGui::TextDisabled("O código que apareceu quando você criou a conta. Depois de usar, ele muda: o novo aparece na hora.");
            ImGui::PopTextWrapPos();
            if (!accountProblem.empty()) {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                ImGui::PushTextWrapPos(0);
                ImGui::TextWrapped("%s", accountProblem.c_str());
                ImGui::PopTextWrapPos();
                ImGui::PopStyleColor();
            }
            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::BeginDisabled(accountBusy);
            ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
            const char* action = accountBusy ? (accountStage == 1 ? "Verificando o PC..." : "Esperando o servidor...") : accountMode == 0 ? "Entrar" : accountMode == 1 ? "Criar conta" : "Trocar a senha";
            if (ImGui::Button(action, ImVec2(200 * scale, 36 * scale)) || (enter && !accountBusy)) StartAccount();
            ImGui::PopStyleColor(2);
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Fechar", ImVec2(120 * scale, 36 * scale)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
                accountOpen = false;
                accPass.fill('\0');
                accPass2.fill('\0');
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
    }
    void ProfileEditor(float scale) {
        if (!profileEdit) return;
        if (!ImGui::IsPopupOpen("##profileEdit")) ImGui::OpenPopup("##profileEdit");
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(std::min(640 * scale, viewport->WorkSize.x - 40 * scale), 0));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(26 * scale, 22 * scale));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, Color(kSelected));
        ImGui::PushStyleColor(ImGuiCol_Button, Color(kSelected));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(0x34353c));
        if (ImGui::BeginPopupModal("##profileEdit", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize)) {
            ImGui::PushFont(semibold, 22);
            ImGui::TextUnformatted("Editar perfil");
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0, 6 * scale));
            const PlayerCard card = profile.Card("", "");
            const ImVec2 at = ImGui::GetCursorScreenPos();
            DrawAvatar(ImGui::GetWindowDrawList(), ImVec2(at.x + 3 * scale, at.y + 3 * scale), 96 * scale, card, scale);
            ImGui::Dummy(ImVec2(102 * scale, 102 * scale));
            ImGui::SameLine(0, 20 * scale);
            ImGui::BeginGroup();
            ImGui::TextDisabled("Foto e capa: imagem, GIF ou vídeo (até 5 s, sem som)");
            const std::vector<std::pair<std::wstring, std::wstring>> kinds = {
                {L"Imagens, GIFs e vídeos", L"*.png;*.jpg;*.jpeg;*.bmp;*.gif;*.mp4;*.mov;*.wmv;*.avi;*.m4v;*.mkv;*.webm"}};
            const auto choose = [this, kinds](bool cover) {
                profileEdit = false;
                ImGui::CloseCurrentPopup();
                pending = [this, kinds, cover] {
                    Browse(cover ? L"Escolher a capa do perfil" : L"Escolher a foto do perfil", L"", false, kinds, [this, cover](const std::wstring& path) {
                        ImportProfileMedia(path, cover);
                        OpenProfileEditor();
                    });
                };
            };
            ImGui::BeginDisabled(importBusy);
            if (ImGui::Button("Escolher foto...")) choose(false);
            if (!profile.photo.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Tirar foto")) profile.SetPhoto("");
            }
            if (ImGui::Button("Escolher capa...")) choose(true);
            if (!profile.cover.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Tirar capa")) profile.SetCover("");
            }
            ImGui::EndDisabled();
            if (importBusy) ImGui::TextDisabled("Convertendo...");
            ImGui::EndGroup();
            // The cover as it will look, under the photo.
            const ImVec2 coverAt = ImGui::GetCursorScreenPos();
            const float coverWidth = ImGui::GetContentRegionAvail().x;
            if (!profile.cover.empty()) {
                DrawCover(ImGui::GetWindowDrawList(), coverAt, ImVec2(coverAt.x + coverWidth, coverAt.y + coverWidth / 5), card, 8 * scale, 0.0f);
                ImGui::Dummy(ImVec2(coverWidth, coverWidth / 5));
            }
            ImGui::Dummy(ImVec2(0, 6 * scale));
            if (profile.accountName.empty()) {
                ImGui::TextDisabled("Nome");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##editName", editName.data(), 41);
            } else {
                ImGui::TextDisabled("Conta");
                ImGui::AlignTextToFramePadding();
                ImGui::Text("%s", profile.accountName.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Sair da conta")) {
                    SignOut();
                    profileEdit = false;
                    ImGui::CloseCurrentPopup();
                }
            }
            ImGui::TextDisabled("Frase");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##editPhrase", editPhrase.data(), 121);

            ImGui::Dummy(ImVec2(0, 8 * scale));
            ImGui::Separator();
            ImGui::Dummy(ImVec2(0, 4 * scale));
            ImGui::PushFont(semibold, 18);
            ImGui::TextUnformatted("RetroAchievements");
            ImGui::PopFont();
            if (profile.raToken.empty()) {
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("Conta grátis em retroachievements.org. A senha vai só para o RetroAchievements e não fica guardada: "
                                    "o Ranga guarda só a chave de acesso que ele devolve.");
                ImGui::PopTextWrapPos();
                ImGui::TextDisabled("Usuário");
                ImGui::SetNextItemWidth(-1);
                ImGui::InputText("##raUser", raUserField.data(), raUserField.size());
                ImGui::TextDisabled("Senha");
                ImGui::SetNextItemWidth(-1);
                const bool enter = ImGui::InputText("##raPassword", raPasswordField.data(), raPasswordField.size(), ImGuiInputTextFlags_Password | ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::BeginDisabled(raBusy);
                if (ImGui::Button(raBusy ? "Entrando..." : "Entrar") || (enter && !raBusy)) StartRaLogin();
                ImGui::EndDisabled();
                std::string problem;
                {
                    std::lock_guard guard(raLock);
                    problem = raProblem;
                }
                if (!problem.empty()) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                    ImGui::PushTextWrapPos(0);
                    ImGui::TextWrapped("%s", problem.c_str());
                    ImGui::PopTextWrapPos();
                    ImGui::PopStyleColor();
                }
            } else {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kGreen));
                ImGui::Text("Conectado como %s", profile.raUser.c_str());
                ImGui::PopStyleColor();
                ImGui::PushTextWrapPos(0);
                ImGui::TextDisabled("As conquistas valem no modo normal (softcore) do RetroAchievements e aparecem no jogo e aqui no perfil.");
                ImGui::PopTextWrapPos();
                if (ImGui::Button("Sair da conta")) profile.SetRetroAchievements("", "");
            }

            ImGui::Dummy(ImVec2(0, 10 * scale));
            ImGui::PushStyleColor(ImGuiCol_Button, Color(kAccent));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, Color(kAccentHover));
            if (ImGui::Button("Salvar", ImVec2(130 * scale, 36 * scale))) {
                if (profile.accountName.empty() && editName[0] != '\0' && profile.name != editName.data()) profile.SetName(editName.data());
                if (profile.phrase != editPhrase.data()) profile.SetPhrase(editPhrase.data());
                profileEdit = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Fechar", ImVec2(130 * scale, 36 * scale)) || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight)) {
                profileEdit = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
    }

    // Achievements just unlocked, at the bottom left for a few seconds.
    void Toasts(float scale) {
        const double now = ImGui::GetTime();
        for (Toast& toast : toasts) {
            if (toast.until != 0.0) continue;
            toast.start = now;
            toast.until = now + 7.0;
        }
        toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& toast) { return toast.until < now; }), toasts.end());
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float bottom = viewport->WorkPos.y + viewport->WorkSize.y - 16 * scale;
        int clicked = -1;
        for (size_t i = 0; i < toasts.size() && i < 4; ++i) {
            const Toast& toast = toasts[i];
            const float height = 74 * scale, width = 360 * scale;
            const float in = EaseOut(static_cast<float>((now - toast.start) / 0.28)), out = std::clamp(static_cast<float>((toast.until - now) / 0.4), 0.0f, 1.0f);
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16 * scale - (1.0f - in) * (width + 24 * scale), bottom), ImGuiCond_Always, ImVec2(0, 1));
            ImGui::SetNextWindowSize(ImVec2(width, height));
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::min(in, out));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Color(kSurface));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale, 12 * scale));
            char name[32];
            std::snprintf(name, sizeof(name), "##toast%zu", i);
            ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | (toast.action ? 0 : ImGuiWindowFlags_NoInputs) | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            if (toast.action && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) clicked = static_cast<int>(i);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            const PlayerCard* card = nullptr;
            PlayerCard friendCard;
            if (!toast.friendId.empty()) {
                std::lock_guard guard(friendsLock);
                for (const auto& known : friends) if (known.id == toast.friendId) friendCard = known, card = &friendCard;
            }
            if (card != nullptr) DrawAvatar(ImGui::GetWindowDrawList(), ImVec2(at.x + 2 * scale, at.y + 2 * scale), 46 * scale, *card, scale);
            else DrawMedal(ImGui::GetWindowDrawList(), at, 50 * scale, toast.text, true);
            ImGui::SetCursorScreenPos(ImVec2(at.x + 62 * scale, at.y + 2 * scale));
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", toast.title.c_str());
            ImGui::PushFont(semibold, 18);
            ImGui::PushClipRect(ImGui::GetCursorScreenPos(), ImVec2(at.x + width - 24 * scale, at.y + height), true);
            ImGui::TextUnformatted(toast.text.c_str());
            ImGui::PopClipRect();
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
            bottom -= height + 10 * scale;
        }
        if (clicked >= 0) {
            auto action = std::move(toasts[static_cast<size_t>(clicked)].action);
            toasts.erase(toasts.begin() + clicked);
            action();
        }
    }

    // How long the window may sleep before the next frame: 16 ms while something moves, 40 ms while only an animated
    // photo or cover plays (their frames last 100 ms), 250 ms when nothing changes on its own.
    DWORD FrameWait() const {
        if (Moving() || PadInFront()) return 16;
        return animatedLastFrame ? 40 : 250;
    }

    // Something on screen is moving by itself (a slide, a notice, a page coming in): frames at 60 a second.
    bool Moving() const {
        return std::fabs(homeTarget - homeScroll) > 0.0f || !toasts.empty() || movedLastFrame || ImGui::GetTime() - pageChangedAt < 0.4;
    }

    bool PadInFront() const { return gamepads.Connected() && (GetForegroundWindow() == window || _wgetenv(L"RANGA_CLIENT_FRENTE") != nullptr); }

    // With a controller, the loop wakes 60 times a second to read it; a frame is drawn only when a button is down,
    // something moves, or the usual idle frame is due (4 a second; 25 with an animated photo). Before, every one of the
    // 60 was a whole frame: a DualSense left on, with Ranga in front, kept CPU and GPU busy for a still menu.
    ULONGLONG lastFrameAt = 0;
    bool SkipFrame() const {
        // RANGA_CLIENT_TODO_QUADRO=1 (medição): o jeito antigo, um quadro a cada leitura do controle.
        static const bool everyFrame = _wgetenv(L"RANGA_CLIENT_TODO_QUADRO") != nullptr;
        if (everyFrame || !PadInFront() || Moving()) return false;
        if (gamepads.AnyDown()) return false;
        return GetTickCount64() - lastFrameAt < (animatedLastFrame ? 40u : 250u);
    }

    // The client gets out of the way while a game runs and comes back when it ends.
    // Minimized or covered (a game is running, say): no frames are drawn, but friends and messages keep coming, so a
    // request or a message is there at once on coming back.
    void Background() {
        if (!profileLoaded) return;
        background = true;
        ProcessAccount();
        SyncProfiles(false);
        NoticeChat();
        PollChat();
    }

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

    std::wstring startGame; // RANGA_CLIENT_PAGE=jogo:<titleId> (tests), until the library has it

    bool startChat = false; // RANGA_CLIENT_PAGE=chat (tests)

    void Frame(float scale) {
        if (startChat) {
            startChat = false;
            OpenProfile();
            openChatTab = true;
        }
        if (!startGame.empty()) {
            for (size_t i = 0; i < library.Games().size(); ++i) {
                if (library.Games()[i].titleId != startGame) continue;
                OpenGame(static_cast<int>(i));
                // RANGA_CLIENT_RELATORIO=1 (tests): makes that game's error report at once.
                if (_wgetenv(L"RANGA_CLIENT_RELATORIO") != nullptr) MakeErrorReport(library.Games()[i]);
                startGame.clear();
                break;
            }
        }
        TakePlayCheck();
        // The hosting PC shares its Switch shader caches once Ranga is open (and after each Switch game, ProcessEnded).
        if (static bool shared = false; !shared) {
            shared = true;
            if (RoomServerEnabled(library.Root())) std::thread([root = library.Root()] { ShareShaderCaches(root); }).detach();
        }
        // The library goes in the profile card (who has what), looked at again every 10 s.
        if (static ULONGLONG ownedAt = 0; profileLoaded && GetTickCount64() - ownedAt > 10000) {
            ownedAt = GetTickCount64();
            profile.owned.clear();
            for (const auto& game : library.Games()) {
                if (game.titleId.size() > 8 && game.titleId.compare(game.titleId.size() - 8, 8, L"-SISTEMA") == 0) continue;
                profile.owned.emplace_back(Utf8(game.system), Utf8(game.title));
            }
        }
        // Library changes asked for while drawing run here, before anything holds on to a game.
        if (pending) {
            auto action = std::move(pending);
            pending = nullptr;
            action();
        }
        movedLastFrame = moving;
        moving = false;
        animatedLastFrame = animating;
        animating = false;
        if (!profileLoaded) InitProfile();
        TendMedia();
        ProcessEnded();
        TakeRaLogin();
        ProcessAccount();
        background = false;
        SyncProfiles(false);
        NoticeChat();
        NoticeFriends();
        TendCloud();
        SendQueuedChat();
        const bool chatWasOpen = chatTabOpen;
        chatTabOpen = false;
        for (const auto& titleId : covers.TakeDone()) {
            for (const auto& game : library.Games()) if (game.titleId == titleId) ForgetArt(game);
        }
        // The controllers belong to the game while it runs, and to an app (Apps) while it is open.
        ReapStoreApps();
        if (runner.Busy() || StoreAppInFront()) gamepads.Stop();
        else gamepads.Start();
        Nav nav = ReadNav();
        // Big Picture: Start / Options or F11 turn the TV mode on and off; Select / View opens the friends' panel,
        // which B closes; Y on the Chat tab writes with the keyboard on screen.
        if (nav.menu) SetTvMode(!tvMode);
        if (nav.view && !profile.session.empty()) {
            friendsPanel = !friendsPanel;
            panelFocus = 0;
        }
        if (friendsPanel && nav.back) {
            friendsPanel = false;
            nav = Nav{};
        }
        if (chatWasOpen && nav.search && !keyboardOpen && !chatPeer.empty()) {
            OpenTextKeyboard(chatInput.data(), chatInput.size(), "Para " + FriendName(chatPeer) + ":", [this] {
                const std::string text = chatInput.data();
                if (text.find_first_not_of(' ') != std::string::npos) QueueChat(chatPeer, text);
                chatInput.fill('\0');
                chatScroll = true;
            });
            nav.search = false;
        }
        // The chat's keyboard and the friends' panel take the d-pad; the page under them gets nothing.
        const bool chatKeyboard = keyboardOpen && keyTarget != nullptr;
        const Nav keysNav = nav;
        if (chatKeyboard || friendsPanel) nav = Nav{};
        // Arrows move the keyboard focus only where there are fields and tabs; on the console row and the grid they
        // move the selection.
        ImGuiIO& io = ImGui::GetIO();
        // A page left behind takes the gamepad's place on its top row with it.
        static Page lastPage = page;
        static bool lastEngines = enginesPage, lastProfile = profileOpen;
        if (page != lastPage || enginesPage != lastEngines || profileOpen != lastProfile) barFocus = -1;
        lastProfile = profileOpen;
        if (enginesPage && !lastEngines) focusEngines = true;
        lastPage = page;
        lastEngines = enginesPage;
        // ImGui's own focus also runs an open list, such as Adicionar jogo's.
        panelNav = friendsPanel ? keysNav : Nav{};
        const bool fieldNav = !chatKeyboard && !friendsPanel && (enginesPage || profileOpen || page == Page::Game || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId));
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
            // Nor is the press the Controle tab takes as a new button.
            else if (!fieldNav || captureFor >= 0 || captureCooling) padStale[button] = true;
            const bool down = fieldNav && padFront && held && !padStale[button];
            if (down != padSent[button]) io.AddKeyEvent(key, padSent[button] = down);
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        const Game* shownGame = page == Page::Game ? Current() : nullptr;
        const std::string pageKey = std::to_string(storeOpen) + std::to_string(newsOpen) + std::to_string(profileOpen) + std::to_string(enginesPage) + std::to_string(static_cast<int>(page)) + profileViewing +
                                    (shownGame != nullptr ? Utf8(shownGame->titleId) : "") + (page == Page::Console ? Utf8(console) : "");
        if (pageKey != shownPage) {
            shownPage = pageKey;
            pageChangedAt = ImGui::GetTime();
        }
        const float appear = EaseOut(static_cast<float>((ImGui::GetTime() - pageChangedAt) / 0.22));
        ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x, viewport->WorkPos.y + (1.0f - appear) * 18 * scale));
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.2f + 0.8f * appear);
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (storeOpen) {
            StorePage(scale, nav);
        } else if (newsOpen) {
            NewsPage(scale, nav);
        } else if (profileOpen) {
            ProfilePage(scale, nav);
        } else if (enginesPage) {
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
        BrowserWindow(scale);
        NoticeWindow(scale);
        ProfileEditor(scale);
        AccountWindow(scale);
        ImGui::End();
        ImGui::PopStyleVar();
        FriendsPanel(scale);
        if (chatKeyboard) SearchKeyboard(keysNav, scale);
        UpdateBar(scale);
        Toasts(scale);
    }

    // ---- Big Picture: the TV mode (full screen, everything bigger, all by the gamepad) ----

    bool tvMode = false;
    // The friends' panel by the gamepad: the friend picked, and the d-pad and buttons it gets this frame.
    int panelFocus = 0;
    Nav panelNav;
    WINDOWPLACEMENT tvRestore{sizeof(WINDOWPLACEMENT)};

    // engines.ini [Ranga] modo_tv: the TV mode is how Ranga opens next time too.
    bool TvModeSaved() const { return ReadIni(library.Root() + L"\\engines.ini", L"Ranga", L"modo_tv", L"0") == L"1"; }

    void SetTvMode(bool on, bool quiet = false) {
        if (on == tvMode || window == nullptr) return;
        tvMode = on;
        if (on) {
            tvRestore.length = sizeof(tvRestore);
            GetWindowPlacement(window, &tvRestore);
            MONITORINFO monitor{};
            monitor.cbSize = sizeof(monitor);
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor);
            const RECT& r = monitor.rcMonitor;
            SetWindowLongPtrW(window, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            SetWindowPos(window, HWND_TOP, r.left, r.top, r.right - r.left, r.bottom - r.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            SetWindowPlacement(window, &tvRestore);
            SetWindowPos(window, nullptr, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
        }
        const std::wstring ini = library.Root() + L"\\engines.ini";
        EnsureUnicodeIni(ini);
        WritePrivateProfileStringW(L"Ranga", L"modo_tv", on ? L"1" : L"0", ini.c_str());
        ForgetIni(ini);
        if (!quiet) AddToast(on ? "Modo TV ligado" : "Modo TV desligado", on ? "Start (ou F11) volta para a janela  ·  Select abre os amigos" : "Start (ou F11) liga de novo", "");
    }
};

Client* g_client = nullptr;
bool g_closeConfirmed = false;

LRESULT WINAPI WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam) {
    // The interface tests post clicks to a window the real pointer is not over; Windows says the pointer left at once,
    // which would undo them.
    static const bool testInput = _wgetenv(L"RANGA_CLIENT_TEST_INPUT") != nullptr;
    if (testInput && message == WM_MOUSELEAVE) return 0;
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

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR commandLine, int) {
    // RangaEmu.exe --salas (the shortcut Windows runs when it starts): only opens the rooms marked on, no window.
    if (commandLine != nullptr && wcsstr(commandLine, L"--salas") != nullptr) {
        wchar_t self[MAX_PATH * 2] = {};
        GetModuleFileNameW(nullptr, self, MAX_PATH * 2);
        std::wstring folder = self;
        folder.resize(folder.find_last_of(L"\\/"));
        StartRoomsOn(folder);
        return 0;
    }
    // One Ranga at a time; the interface tests (RANGA_CLIENT_TEST_INPUT) run their copy beside the user's.
    HANDLE single = CreateMutexW(nullptr, TRUE, kMutex);
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS && commandLine != nullptr && wcsstr(commandLine, L"--atualizado") != nullptr) {
        // Reopened by an update: the old Ranga is closing; its lock is taken over once it lets go.
        const DWORD waited = WaitForSingleObject(single, 10000);
        if (waited == WAIT_OBJECT_0 || waited == WAIT_ABANDONED) SetLastError(0);
    }
    // Publishing opens no window, so it runs beside an open Ranga instead of only bringing that one to the front (it
    // used to end there, saying nothing, and nothing was published). It reads the installed files, which an open
    // Ranga does not lock.
    const bool publishing = commandLine != nullptr && (wcsstr(commandLine, L"--publicar") != nullptr || wcsstr(commandLine, L"--assinar-catalogo") != nullptr);
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS && _wgetenv(L"RANGA_CLIENT_TEST_INPUT") == nullptr && !publishing) {
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
    // What an update left behind (the old RangaEmu.exe and cores, the download folder) goes once the new one runs.
    CleanUpdateLeftovers(root);
    // RangaEmu.exe --assinar-catalogo "C:\...\catalogo.txt": signs the Apps catalog (Store.hpp) with this PC's key,
    // writing catalogo.sig beside it; the result goes to salas\publicar.log. Only the PC that publishes has the key.
    if (commandLine != nullptr && wcsstr(commandLine, L"--assinar-catalogo") != nullptr) {
        std::wstring path = wcsstr(commandLine, L"--assinar-catalogo") + 18;
        while (!path.empty() && (path.front() == L' ' || path.front() == L'"')) path.erase(0, 1);
        while (!path.empty() && (path.back() == L' ' || path.back() == L'"')) path.pop_back();
        const std::wstring error = SignStoreCatalog(root, path);
        if (FILE* log = _wfopen((root + L"\\salas\\publicar.log").c_str(), L"ab")) {
            const std::string line = error.empty() ? "catalogo assinado: " + Utf8(path) + "\n" : "catalogo NAO assinado: " + Utf8(error) + "\n";
            std::fwrite(line.data(), 1, line.size(), log);
            std::fclose(log);
        }
        return error.empty() ? 0 : 1;
    }
    // RangaEmu.exe --publicar "o que mudou": publishes this PC's Ranga for the friends without opening the window;
    // the result goes to salas\publicar.log.
    if (commandLine != nullptr && wcsstr(commandLine, L"--publicar") != nullptr) {
        // --versao 1.1.0 (before --publicar) gives the number; without it the last part goes one up.
        std::string wanted;
        if (const wchar_t* at = wcsstr(commandLine, L"--versao "); at != nullptr && at < wcsstr(commandLine, L"--publicar")) {
            for (at += 9; *at == L' '; ++at) {}
            for (; *at != 0 && *at != L' '; ++at) wanted += static_cast<char>(*at);
        }
        std::wstring notes = wcsstr(commandLine, L"--publicar") + 10;
        while (!notes.empty() && (notes.front() == L' ' || notes.front() == L'"')) notes.erase(0, 1);
        while (!notes.empty() && (notes.back() == L' ' || notes.back() == L'"')) notes.pop_back();
        std::string version;
        const std::wstring error = PublishUpdate(root, Utf8(notes), version, wanted);
        if (!error.empty()) {
            WriteTextLine(root + L"\\salas\\publicar.log", error);
            return 1;
        }
        std::string sent;
        const std::wstring github = wcsstr(commandLine, L"--sem-github") != nullptr ? L"GitHub: pulado" : PublishToGitHub(root, sent);
        WriteTextLine(root + L"\\salas\\publicar.log", L"publicada " + Wide(InstalledVersionName(root)) + L" (" + Wide(version) + L") | " + (github.empty() ? L"GitHub: ok" : github));
        return 0;
    }
    // RangaEmu.exe --servidor: this PC hosts rooms; the rooms section shows from now on.
    if (commandLine != nullptr && wcsstr(commandLine, L"--servidor") != nullptr) SetRoomServerEnabled(root, true);
    if (commandLine != nullptr && wcsstr(commandLine, L"--mostrar-ps4-ps5") != nullptr) SetShowPs4Ps5(root, true);
    if (commandLine != nullptr && wcsstr(commandLine, L"--esconder-ps4-ps5") != nullptr) SetShowPs4Ps5(root, false);

    ImGui_ImplWin32_EnableDpiAwareness();
    const float scale = ImGui_ImplWin32_GetDpiScaleForMonitor(MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY));
    WNDCLASSEXW windowClass{sizeof(windowClass), CS_CLASSDC, WindowProc, 0, 0, instance, LoadIconW(instance, MAKEINTRESOURCEW(1)), LoadCursorW(nullptr, IDC_ARROW), nullptr, nullptr, kWindowClass,
                             LoadIconW(instance, MAKEINTRESOURCEW(1))};
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
    // RANGA_CLIENT_TV=1 (tests) opens in the TV mode, as engines.ini modo_tv=1 does.
    const bool startTv = _wgetenv(L"RANGA_CLIENT_TV") != nullptr;
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
    // The chat's emoji: Windows' Segoe UI Emoji merged into each font, in their own colors (FreeType draws the
    // font's color layers).
    const bool emoji = Exists(Wide(fonts + "seguiemj.ttf"));
    const auto mergeEmoji = [&] {
        if (!emoji) return;
        ImFontConfig merge;
        merge.MergeMode = true;
        merge.FontLoaderFlags |= ImGuiFreeTypeLoaderFlags_LoadColor;
        io.Fonts->AddFontFromFileTTF((fonts + "seguiemj.ttf").c_str(), 0.0f, &merge);
    };
    mergeEmoji();
    ImFont* semibold = Exists(Wide(fonts + "seguisb.ttf")) ? io.Fonts->AddFontFromFileTTF((fonts + "seguisb.ttf").c_str()) : nullptr;
    if (semibold != nullptr) mergeEmoji();
    ApplyTheme(scale);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // RANGA_CLIENT_TIMING=<file>: how long each step of the start takes, in milliseconds, written to that file.
    static const wchar_t* timingPath = _wgetenv(L"RANGA_CLIENT_TIMING");
    LARGE_INTEGER frequency, mark;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&mark);
    std::string timing;
    const auto step = [&](const char* what) {
        if (timingPath == nullptr) return;
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        timing += std::string(what) + ": " + std::to_string((now.QuadPart - mark.QuadPart) * 1000 / frequency.QuadPart) + " ms\r\n";
        mark = now;
    };
    Client client(root);
    client.window = hwnd;
    client.semibold = semibold != nullptr ? semibold : regular;
    g_runner = &client.runner;
    g_client = &client;
    step("Client");
    // A Ranga folder copied or extracted somewhere else first gets its saved paths pointed at where it is now.
    if (const int moved = RelocateRoot(client.library.Root()); moved > 0) {
        client.Notify(L"O Ranga está numa pasta nova (" + client.library.Root() + L"). " + std::to_wstring(moved) +
                      L" arquivo(s) de configuração foram ajustados para ela; as versões antigas ficaram em backups\\mudou-de-pasta.");
    }
    step("RelocateRoot");
    // Every console's folder of games exists from the first start, and what is in them is in the library.
    client.library.ScanGamesFolders();
    step("ScanGamesFolders");
    RepairLinks(client.library.Root());
    step("RepairLinks");
    // Rooms kept on that are not running (the PC restarted without the Windows option) open again.
    StartRoomsOn(client.library.Root());
    step("StartRoomsOn");
    client.Reload(client.library.Selected());
    step("Reload");
    if (timingPath != nullptr) {
        if (FILE* file = _wfopen(timingPath, L"wb")) {
            std::fwrite(timing.data(), 1, timing.size(), file);
            std::fclose(file);
        }
    }
    if (const Game* game = client.Current()) client.console = game->system;
    if (startTv || client.TvModeSaved()) client.SetTvMode(true, true);
    float appliedScale = scale;

    bool done = false;
    while (!done) {
        MSG message;
        // Idle frames wait for input, so the client costs nothing while a game runs; a busy runner keeps it ticking for status.
        // A controller is read, not told by a message, and the console row slides: those need frames meanwhile.
        if (!client.runner.Busy()) MsgWaitForMultipleObjects(0, nullptr, FALSE, client.FrameWait(), QS_ALLINPUT);
        bool gotMessage = false;
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            gotMessage = true;
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT) done = true;
        }
        if (done) break;
        // Woke only to look at the controller, and nothing is down: no frame (see SkipFrame).
        if (!gotMessage && !client.runner.Busy() && client.SkipFrame()) continue;
        client.lastFrameAt = GetTickCount64();
        static const wchar_t* shotPath = _wgetenv(L"RANGA_CLIENT_SHOT");
        if (IsIconic(hwnd) || (shotPath == nullptr && g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
            Sleep(50);
            if (!IsIconic(hwnd)) g_occluded = false;
            client.Watch();
            client.Background();
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
        client.RefreshIniCache();
        ImGui::NewFrame();
        // The TV mode is everything a third bigger, for a TV seen from the couch.
        const float shownScale = scale * (client.tvMode ? 1.35f : 1.0f);
        if (shownScale != appliedScale) {
            ApplyTheme(shownScale);
            appliedScale = shownScale;
        }
        client.Frame(shownScale);
        ImGui::Render();
        const ImVec4 clear = Color(kBackground);
        const float clearColor[4] = {clear.x, clear.y, clear.z, 1.0f};
        g_context->OMSetRenderTargets(1, &g_target, nullptr);
        g_context->ClearRenderTargetView(g_target, clearColor);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        if (shotPath != nullptr) {
            static ULONGLONG shotAt = 0;
            if (GetTickCount64() - shotAt > 1000) {
                WriteShot(shotPath);
                shotAt = GetTickCount64();
            }
        }
        g_occluded = g_swapChain->Present(1, 0) == DXGI_STATUS_OCCLUDED;
        // A screen asleep presents nothing and returns at once: the loop is held to about 60 turns a second.
        if (shotPath != nullptr && g_occluded) Sleep(16);
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
