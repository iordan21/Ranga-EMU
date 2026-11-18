// Ranga Emu client: a library of games of every console on the left, the chosen game on the right, with Play and its
// options, cheats and logs, and an Emulators page where each console other than PS5 gets the emulator that runs it.
#include "Common.hpp"
#include "Covers.hpp"
#include "Dlss5.hpp"
#include "Library.hpp"
#include "Runner.hpp"
#include "Systems.hpp"
#include "Rooms.hpp"
#include "ConsoleGraphics.hpp"
#include "Update.hpp"
#include "Http.hpp"
#include "PlayerProfile.hpp"

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
#include <cfloat>

#include "imgui.h"
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

void ApplyTheme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
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

// A photo for the profile: the middle square of the picture, shrunk to kAvatarSide by averaging; empty when the
// file is no PNG or JPEG.
std::string MakeAvatar(const std::wstring& path) {
    const std::string bytes = ReadBytes(path);
    if (bytes.empty()) return {};
    int width = 0, height = 0, channels = 0;
    unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &width, &height, &channels, 4);
    if (pixels == nullptr) return {};
    const int side = std::min(width, height), left = (width - side) / 2, top = (height - side) / 2, out = kAvatarSide;
    std::string avatar(static_cast<size_t>(out) * out * 4, '\0');
    for (int y = 0; y < out; ++y) {
        const int y0 = top + y * side / out, y1 = std::max(y0 + 1, top + (y + 1) * side / out);
        for (int x = 0; x < out; ++x) {
            const int x0 = left + x * side / out, x1 = std::max(x0 + 1, left + (x + 1) * side / out);
            unsigned long long sum[4] = {};
            for (int sy = y0; sy < y1; ++sy) {
                const unsigned char* row = pixels + (static_cast<size_t>(sy) * width + x0) * 4;
                for (int sx = 0; sx < x1 - x0; ++sx) for (int c = 0; c < 4; ++c) sum[c] += row[sx * 4 + c];
            }
            const unsigned long long count = static_cast<unsigned long long>(y1 - y0) * (x1 - x0);
            for (int c = 0; c < 4; ++c) avatar[(static_cast<size_t>(y) * out + x) * 4 + c] = static_cast<char>(sum[c] / count);
        }
    }
    stbi_image_free(pixels);
    return avatar;
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
    std::array<char, 128> roomAddress{}, roomPort{}, roomNickname{}, roomPassword{};
    // Online play of the classics (Ranga Retro's own, Netplay.hpp): this game's choice and the table server's state.
    std::array<char, 128> onlineAddress{}, onlineName{};
    bool onlineOn = false;
    int onlineDelay = 4;
    // The servers beside the Switch's rooms (Rooms.hpp Extra), by kind: the port typed, whether it runs, why not.
    std::array<char, 8> serverPort[2]{};
    bool serverRunning[2] = {};
    ULONGLONG serverCheckedAt[2] = {};
    std::wstring serverProblem[2];
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

    explicit Client(const std::wstring& root) : library(root), covers(root) {
        // Every PC but the one that hosts looks for a newer Ranga there when it opens (Update.hpp).
        // RANGA_CLIENT_AUTO_UPDATE=1 (tests): checks even in test mode and takes the update without asking.
        if (!RoomServerEnabled(root) && (_wgetenv(L"RANGA_CLIENT_TEST_INPUT") == nullptr || autoUpdate)) StartUpdateCheck();
    }
    ~Client() {
        if (updateThread.joinable()) updateThread.join();
        StopProfileThreads();
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
        const int delay = std::atoi(ReadCoreOption(game, "ranga_online_atraso").c_str());
        onlineDelay = delay >= 2 && delay <= 8 ? delay : 4;
        LoadServerPorts();
    }

    void LoadServerPorts() {
        for (const Extra extra : {Extra::Classics, Extra::Rooms3ds}) {
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
        if (Game* game = Current()) {
            options = LoadOptions(*game);
            addons = LoadAddons(*game);
            dlss5Kit = InspectDlss5(library.Root());
            if (game->system == L"SWITCH") {
                const std::string mode = ReadCoreOption(*game, "eden_sala");
                roomMode = mode == "entrar" ? 1 : mode == "criar" ? 2 : 0;
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
                fill(roomAddress, "eden_sala_endereco", L"sala_endereco", "");
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
        if (game.system == L"PSP") files.push_back(art + (icon ? L"\\ICON0.PNG" : L"\\PIC1.PNG"));
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
        bool left = false, right = false, up = false, down = false, accept = false, back = false, padAccept = false;
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
        ImGui::SetCursorPos(ImVec2(right - wide - 2 * narrow - 20 * scale, 22 * scale));
        if (ImGui::Button("+  Adicionar jogo", ImVec2(wide, height)) || barPress == first) ImGui::OpenPopup("##add");
        FocusLast(barFocus == first, scale);
        AddMenu(scale);
        ImGui::SameLine(0, 10 * scale);
        if (ImGui::Button("Emuladores", ImVec2(narrow, height)) || barPress == first + 1) {
            enginesPage = true;
            profileOpen = false;
            RefreshEngines();
        }
        FocusLast(barFocus == first + 1, scale);
        ImGui::SameLine(0, 10 * scale);
        const std::string profileLabel = chatUnread > 0 ? "Perfil (" + std::to_string(chatUnread) + ")###perfil" : "Perfil###perfil";
        if (ImGui::Button(profileLabel.c_str(), ImVec2(narrow, height)) || barPress == first + 2) OpenProfile();
        FocusLast(barFocus == first + 2, scale);
    }

    // The art of a game in a box: its own square art fills it; box art of another shape sits whole on the console's
    // colors; no art at all is the console's colors with the title.
    void GameArt(ImDrawList* draw, const Game& game, ImVec2 min, ImVec2 max, float scale) {
        const Look& look = LookFor(game.system);
        const float rounding = 10 * scale, width = max.x - min.x, height = max.y - min.y;
        const Texture& icon = Icon(game);
        if (icon.view == nullptr) {
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

    void Home(float scale, const Nav& input) {
        // On the top row (Adicionar jogo, Emuladores) the gamepad moves nothing else; up from the consoles reaches it.
        const bool inBar = TopBar(input, 3);
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
        // The top row here is Consoles, Adicionar jogo and Emuladores.
        const bool inBar = TopBar(input, 4);
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
        if (!games.empty()) {
            ImGui::SameLine(0, 18 * scale);
            ImGui::SetCursorPosY(22 * scale);
            if (ImGui::Button("Pasta de jogos", ImVec2(0, 36 * scale))) pending = [this] { OpenGamesFolder(); };
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Abre a pasta deste console. O que você colocar nela entra na lista sozinho.");
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
            if (roomMode != 0) {
                const auto field = [&](const char* name, const char* id, std::array<char, 128>& text, const char* key, const wchar_t* remembered, ImGuiInputTextFlags flags = 0) {
                    label(name);
                    ImGui::InputText(id, text.data(), text.size(), flags);
                    if (ImGui::IsItemDeactivatedAfterEdit()) {
                        WriteCoreOption(game, key, text.data());
                        RememberSwitch(remembered, text.data());
                    }
                };
                if (roomMode == 1) field("Endereço", "##roomAddress", roomAddress, "eden_sala_endereco", L"sala_endereco");
                // Joining with "address:port" (as playit shows it), that port is the one used: the field is not shown,
                // so a leftover 24872 there can no longer look like it matters.
                const std::string address = roomAddress.data();
                const size_t colon = address.rfind(':');
                const bool portInAddress = colon != std::string::npos && colon + 1 < address.size() &&
                                           std::all_of(address.begin() + static_cast<std::ptrdiff_t>(colon + 1), address.end(), [](unsigned char c) { return std::isdigit(c); });
                if (roomMode == 1 && portInAddress) {
                    label("Porta");
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextDisabled("%s (a do endereço)", address.substr(colon + 1).c_str());
                } else {
                    field("Porta", "##roomPort", roomPort, "eden_sala_porta", L"sala_porta", ImGuiInputTextFlags_CharsDecimal);
                }
                field("Senha", "##roomPassword", roomPassword, "eden_sala_senha", L"sala_senha");
                const std::string nickname = roomNickname.data();
                const bool nicknameFits = nickname.size() >= 4 && nickname.size() <= 20 &&
                    std::all_of(nickname.begin(), nickname.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '_' || c == '-' || c == ' '; });
                const char* problem = !nicknameFits ? "Para a sala, o nome do jogador precisa de 4 a 20 letras ou números, sem acento." :
                                      roomMode == 1 && roomAddress[0] == 0 ? "Falta o endereço que o dono da sala passou." : nullptr;
                if (problem != nullptr) {
                    ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                    ImGui::TextWrapped("%s", problem);
                    ImGui::PopStyleColor();
                }
                ImGui::PushTextWrapPos(0);
                if (roomMode == 1) ImGui::TextDisabled("Cole o endereço como o dono da sala passou; \"endereço:porta\" junto também serve, e aí a porta pode ficar vazia. Senha só se a sala tiver.");
                else ImGui::TextDisabled("A sala abre junto com o jogo, na porta escolhida (vazia = 24872), em UDP. Para amigos de fora entrarem, aponte um túnel UDP (playit, por exemplo) para 127.0.0.1 nessa porta e passe a eles o endereço do túnel.");
                ImGui::TextDisabled("No jogo, todos escolhem o modo sem fio local / LAN, não o online da Nintendo. Todos precisam da mesma versão do emulador e do jogo.");
                ImGui::PopTextWrapPos();
            }
            ImGui::EndDisabled();
            if (running) ImGui::TextDisabled("Feche o jogo para mudar a sala.");
            RoomsSection(game, label, labelWidth, scale);
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
            field("Seu nome", "##onlineName", onlineName, "ranga_online_nome", L"online_nome");
            label("Atraso");
            if (ImGui::SliderInt("##onlineDelay", &onlineDelay, 2, 8, "%d quadros")) WriteCoreOption(game, "ranga_online_atraso", std::to_string(onlineDelay));
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

    // The PSP's own online: its local wireless (ad hoc), through a public PRO ad hoc server kept by fans. PPSSPP's
    // server takes a fixed port (27312) and a plain IP, and the players' consoles then talk to each other directly, so
    // a playit tunnel cannot carry it; the public servers can, when the players' routers let the games through (UPnP).
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
        }
        if (wanted) {
            label("Servidor");
            if (ImGui::Combo("##pspServer", &picked, servers, 3)) WriteCoreOption(game, "ppsspp_change_pro_ad_hoc_server_address", servers[picked]);
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Para jogos com multijogador local do PSP (Monster Hunter, Tekken, Ridge Racer...). Todos escolhem o mesmo servidor "
                                "e, no jogo, o modo ad hoc / multijogador local. É um servidor público de fãs, não o playit: os PSPs conversam "
                                "direto entre si, então depende do roteador de cada um deixar passar (UPnP ligado). Se não conectar, é o roteador.");
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
        ImGui::TextUnformatted(classics ? "Sala dos clássicos deste PC (servidor)" : "Sala do 3DS deste PC (servidor)");
        ImGui::PopStyleColor();
        ImGui::SetCursorPosX(labelWidth);
        ImGui::PushTextWrapPos(0);
        if (classics)
            ImGui::TextDisabled("Uma sala só serve para todos os jogos clássicos. Precisa de um túnel TCP do playit para 127.0.0.1 nesta porta; "
                                "o endereço do túnel é o que todos (você também, ou 127.0.0.1:porta) põem em \"Endereço da sala\".");
        else
            ImGui::TextDisabled("Uma sala serve para todos os jogos de 3DS. Precisa de um túnel UDP do playit para 127.0.0.1 nesta porta; "
                                "o endereço do túnel é o que todos (você também, ou 127.0.0.1:porta) põem em \"Endereço da sala\".");
        ImGui::PopTextWrapPos();
        label("Porta");
        ImGui::BeginDisabled(serverRunning[k]);
        ImGui::InputText("##serverPort", serverPort[k].data(), serverPort[k].size(), ImGuiInputTextFlags_CharsDecimal);
        if (ImGui::IsItemDeactivatedAfterEdit()) SetExtraPort(library.Root(), extra, std::atoi(serverPort[k].data()));
        ImGui::EndDisabled();
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
                };
            }
            if (!publishResult.empty()) {
                ImGui::SameLine();
                ImGui::TextDisabled("%s", Utf8(publishResult).c_str());
            }
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

    void Play(Game& game) {
        Save();
        RefreshEngines();
        // For the profile: online or not, and what the friends see as being played.
        playOnline = OnlineChosen(game);
        playingTitle = Utf8(game.title);
        playingSystem = SystemName(game.system);
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
    };
    std::mutex endedLock;
    std::vector<Ended> endedQueue;
    std::atomic<bool> playOnline{false};
    std::string playingTitle, playingSystem;
    struct Toast {
        std::string title, text, badge;
        double until = 0;
    };
    std::vector<Toast> toasts;
    // The friends' cards, as the room handed them over the last time (SyncProfiles, in the background).
    std::mutex friendsLock;
    std::vector<PlayerCard> friends;
    std::string friendsProblem, friendsServer;
    long long friendsAt = 0;
    // Servidor dos amigos, as typed on the Amigos tab.
    std::array<char, 128> centralField{}, groupField{};
    // The friends' chat: what the server has (the last 500), fetched in the background; unread counts what came from
    // the others while the Chat tab was not open.
    struct ChatMessage {
        long long number = 0, when = 0;
        std::string id, name, text;
    };
    std::mutex chatLock;
    std::vector<ChatMessage> chat;
    long long chatLast = 0, chatSeen = -1;
    std::string chatProblem, chatServer;
    std::thread chatThread;
    std::atomic<bool> chatBusy{false};
    ULONGLONG chatAt = 0;
    int chatUnread = 0;
    size_t chatShown = 0;
    bool chatTabOpen = false, chatScroll = false;
    std::array<char, 501> chatInput{};
    std::thread profileThread;
    std::atomic<bool> profileBusy{false};
    ULONGLONG profileSyncedAt = 0;
    uint64_t sentRevision = 0;
    std::string sentPlaying;
    // Pictures: this player's photo, the friends' (by id), and RetroAchievements' badges, downloaded once to
    // perfil\insignias.
    Texture avatarTexture;
    std::string avatarShown;
    std::map<std::string, std::pair<std::string, Texture>> friendAvatars;
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
        if (profileThread.joinable()) profileThread.join();
        if (chatThread.joinable()) chatThread.join();
        // The friends see this Ranga closed at once, not five minutes later.
        if (profileLoaded && profileSyncedAt != 0) {
            PlayerCard card = profile.Card("", "");
            card.closed = true;
            SocialRequest(library.Root(), "POST", "/ranga/perfil", "segredo\t" + profile.secret + "\n" + WriteCard(card), 2000);
        }
        if (badgeThread.joinable()) badgeThread.join();
        if (raThread.joinable()) raThread.join();
    }

    int LibraryCount() { return static_cast<int>(library.Games().size()); }

    void InitProfile() {
        profileLoaded = true;
        profile.Load(library.Root());
        runner.ended = [this](const Game& game, long long started, long long seconds) {
            std::lock_guard guard(endedLock);
            endedQueue.push_back({game, started, seconds, playOnline.load()});
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

    void AddToast(const std::string& title, const std::string& text, const std::string& badge) {
        toasts.push_back({title, text, badge, ImGui::GetTime() + 7.0});
    }

    // Whether the game was set to play online when it started (any of the online choices on its page).
    static bool OnlineChosen(const Game& game) {
        const std::string room = ReadCoreOption(game, "eden_sala");
        return ReadCoreOption(game, "ranga_online") == "ligado" || ReadCoreOption(game, "citra_sala") == "entrar" ||
               ReadCoreOption(game, "ppsspp_enable_wlan") == "enabled" || (!room.empty() && room != "desligada");
    }

    // The games that closed: their time goes to the profile, and what that unlocks shows at the bottom.
    void ProcessEnded() {
        if (!runner.Busy() && !playingTitle.empty()) {
            playingTitle.clear();
            playingSystem.clear();
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
        }
        ScanProfileRa();
        for (const auto& title : profile.Evaluate(LibraryCount())) AddToast("Conquista desbloqueada", title, "");
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

    // Sends this player's card to the room and takes everyone's back: at once when something changed (a game opened
    // or closed, the profile was edited), otherwise every two minutes, so "jogando agora" and "visto" stay true.
    void SyncProfiles(bool force) {
        if (!profileLoaded || profileBusy) return;
        const ULONGLONG now = GetTickCount64();
        const std::string playing = playingTitle.empty() ? "" : playingTitle + "\t" + playingSystem;
        const bool changed = profile.Revision() != sentRevision || playing != sentPlaying;
        if (!force && profileSyncedAt != 0) {
            if (!changed && now - profileSyncedAt < 120000) return;
            if (changed && now - profileSyncedAt < 5000) return;
        }
        if (profileThread.joinable()) profileThread.join();
        profileSyncedAt = now;
        sentRevision = profile.Revision();
        sentPlaying = playing;
        const std::string body = "segredo\t" + profile.secret + "\n" + WriteCard(profile.Card(playingTitle, playingSystem));
        const std::wstring root = library.Root();
        profileBusy = true;
        profileThread = std::thread([this, body, root] {
            const SocialAnswer posted = SocialRequest(root, "POST", "/ranga/perfil", body, 8000);
            const HttpResult& sent = posted.result;
            const SocialAnswer fetched = sent.status == 0 ? SocialAnswer{} : SocialRequest(root, "GET", "/ranga/perfis", "", 8000);
            const HttpResult& got = fetched.result;
            const std::string server = posted.server;
            {
                std::lock_guard guard(friendsLock);
                friendsServer = fetched.server.empty() ? server : fetched.server;
                if (got.status == 200) {
                    friends = ReadCards(got.body);
                    friendsAt = static_cast<long long>(std::time(nullptr));
                    friendsProblem.clear();
                } else if (sent.status == 401 || got.status == 401) {
                    friendsProblem = "O servidor dos amigos pede o código do grupo. Digite em Servidor dos amigos, aqui embaixo.";
                } else if (sent.status == 0) {
                    friendsProblem = "A sala dos amigos não respondeu (" + server + "). O PC que hospeda precisa estar ligado, com o playit e a sala dos clássicos.";
                } else {
                    friendsProblem = "A sala respondeu com erro " + std::to_string(got.status) + ". O servidor da sala pode ser de uma versão antiga do Ranga.";
                }
                if (sent.status == 403) friendsProblem = "A sala recusou este perfil: outro Ranga já usa o mesmo código.";
            }
            profileBusy = false;
        });
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

    const Texture* AvatarOf(const PlayerCard& card) {
        if (card.avatar.empty()) return nullptr;
        const auto make = [](Texture& texture, const std::string& pixels) {
            if (texture.view != nullptr) texture.view->Release();
            texture = UploadTexture(reinterpret_cast<const unsigned char*>(pixels.data()), kAvatarSide, kAvatarSide);
        };
        if (card.id == profile.id) {
            if (avatarShown != card.avatar) {
                make(avatarTexture, card.avatar);
                avatarShown = card.avatar;
            }
            return avatarTexture.view != nullptr ? &avatarTexture : nullptr;
        }
        auto& [pixels, texture] = friendAvatars[card.id];
        if (pixels != card.avatar) {
            make(texture, card.avatar);
            pixels = card.avatar;
        }
        return texture.view != nullptr ? &texture : nullptr;
    }

    // The photo in a square with a ring of the level's color; no photo is the name's first letter.
    void DrawAvatar(ImDrawList* draw, ImVec2 min, float side, const PlayerCard& card, float scale) {
        const ImVec2 max(min.x + side, min.y + side);
        const float ring = std::max(2.0f, 3 * scale * side / (128 * scale));
        draw->AddRectFilled(ImVec2(min.x - ring, min.y - ring), ImVec2(max.x + ring, max.y + ring), Packed(LevelColor(LevelFor(card.xp).level)), 8 * scale);
        if (const Texture* photo = AvatarOf(card)) {
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
        std::snprintf(groupField.data(), groupField.size(), "%s", GroupCode(library.Root()).c_str());
        profileOpen = true;
        enginesPage = false;
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
        ImGui::ProgressBar(static_cast<float>(level.into) / static_cast<float>(level.size), ImVec2(110 * scale, 6 * scale), "");
        ImGui::PopStyleColor();
        ImGui::TextDisabled("%d / %d XP", level.into, level.size);
        ImGui::EndGroup();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("XP: 10 por hora jogada, 50 por conquista do Ranga e os pontos de cada conquista do RetroAchievements.");
        if (own) {
            ImGui::SetCursorScreenPos(ImVec2(origin.x + size.x - left - 190 * scale, origin.y + 150 * scale));
            if (ImGui::Button("Editar perfil", ImVec2(190 * scale, 36 * scale))) OpenProfileEditor();
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
            if (own && ImGui::BeginTabItem(chatLabel.c_str())) {
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

    // Fetches the chat (every three seconds while its tab is open, every thirty otherwise), sending say first.
    void PollChat(const std::string& say = "") {
        if (!profileLoaded || chatBusy) return;
        const ULONGLONG now = GetTickCount64();
        if (say.empty() && chatAt != 0 && now - chatAt < (chatTabOpen ? 3000u : 30000u)) return;
        if (chatThread.joinable()) chatThread.join();
        chatAt = now;
        chatBusy = true;
        long long after;
        std::string before;
        {
            std::lock_guard guard(chatLock);
            after = chatLast;
            before = chatServer;
        }
        chatThread = std::thread([this, say, after, before, root = library.Root(), secret = profile.secret, id = profile.id] {
            std::string problem;
            if (!say.empty()) {
                const int sent = SocialRequest(root, "POST", "/ranga/chat", "segredo\t" + secret + "\nid\t" + id + "\ntexto\t" + say + "\n", 8000).result.status;
                if (sent == 429) problem = "Calma: uma mensagem por segundo.";
                else if (sent == 403) problem = "O servidor ainda não conhece o seu perfil. Espere alguns segundos e mande de novo.";
                else if (sent == 401) problem = "Falta o código do grupo (aba Amigos, Servidor dos amigos).";
                else if (sent != 200) problem = "A mensagem não foi enviada: o servidor dos amigos não respondeu.";
            }
            SocialAnswer got = SocialRequest(root, "GET", "/ranga/chat?desde=" + std::to_string(after), "", 8000);
            // Another server (the central one came back, or went down) numbers its messages its own way: all again.
            const bool restart = got.result.status == 200 && after != 0 && got.server != before;
            if (restart) got = SocialRequest(root, "GET", "/ranga/chat?desde=0", "", 8000);
            std::vector<ChatMessage> fresh;
            for (size_t at = 0; got.result.status == 200 && at < got.result.body.size();) {
                size_t end = got.result.body.find('\n', at);
                if (end == std::string::npos) end = got.result.body.size();
                const std::string line = got.result.body.substr(at, end - at);
                at = end + 1;
                std::vector<std::string> parts;
                for (size_t from = 0;;) {
                    const size_t tab = line.find('\t', from);
                    parts.push_back(line.substr(from, tab == std::string::npos ? std::string::npos : tab - from));
                    if (tab == std::string::npos) break;
                    from = tab + 1;
                }
                if (parts.size() == 6 && parts[0] == "m") fresh.push_back({std::atoll(parts[1].c_str()), std::atoll(parts[2].c_str()), parts[3], parts[4], parts[5]});
            }
            std::lock_guard guard(chatLock);
            if (got.result.status == 200) {
                if (restart) {
                    chat.clear();
                    chatLast = 0;
                }
                chatServer = got.server;
                for (auto& message : fresh) {
                    if (message.number <= chatLast) continue;
                    chatLast = message.number;
                    chat.push_back(std::move(message));
                }
                if (chat.size() > 500) chat.erase(chat.begin(), chat.begin() + static_cast<std::ptrdiff_t>(chat.size() - 500));
            } else if (problem.empty()) {
                problem = got.result.status == 401 ? "Falta o código do grupo (aba Amigos, Servidor dos amigos)." : got.result.status == 0 ? "O servidor dos amigos não respondeu." : "";
            }
            chatProblem = problem;
            chatBusy = false;
        });
    }

    // New messages from the others: a notice and the count on Perfil and Chat while the chat is not being read.
    void NoticeChat() {
        std::vector<ChatMessage> fresh;
        {
            std::lock_guard guard(chatLock);
            if (chatSeen < 0) {
                // The history already there when Ranga opened is not news.
                if (chatServer.empty() || chatBusy) return;
                chatSeen = chatLast;
                return;
            }
            for (const auto& message : chat) if (message.number > chatSeen && message.id != profile.id) fresh.push_back(message);
            if (chatLast > chatSeen && chatTabOpen) {
                chatSeen = chatLast;
                chatUnread = 0;
                return;
            }
            chatSeen = std::max(chatSeen, chatLast);
        }
        for (const auto& message : fresh) {
            ++chatUnread;
            AddToast("Chat  ·  " + message.name, message.text, "");
        }
    }

    void ProfileChat(float scale) {
        chatTabOpen = true;
        std::vector<ChatMessage> list;
        std::string problem;
        {
            std::lock_guard guard(chatLock);
            list = chat;
            problem = chatProblem;
        }
        if (list.size() != chatShown) {
            chatShown = list.size();
            chatScroll = true;
        }
        ImGui::Dummy(ImVec2(0, 6 * scale));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(kSidebar));
        ImGui::BeginChild("##chatLog", ImVec2(0, -54 * scale), ImGuiChildFlags_None, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        ImGui::Dummy(ImVec2(0, 4 * scale));
        if (list.empty()) ImGui::TextDisabled("  Nenhuma mensagem ainda. Mande a primeira!");
        for (const auto& message : list) {
            ImGui::SetCursorPosX(12 * scale);
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 15);
            ImGui::PushStyleColor(ImGuiCol_Text, Color(message.id == profile.id ? kGreen : kAccentHover));
            ImGui::TextUnformatted(message.name.c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
            ImGui::SameLine();
            const time_t at = static_cast<time_t>(message.when);
            tm local{};
            localtime_s(&local, &at);
            ImGui::TextDisabled("%02d/%02d %02d:%02d", local.tm_mday, local.tm_mon + 1, local.tm_hour, local.tm_min);
            ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - 12 * scale);
            ImGui::TextWrapped("%s", message.text.c_str());
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, 4 * scale));
        }
        if (chatScroll) {
            ImGui::SetScrollHereY(1.0f);
            chatScroll = false;
        }
        ImGui::EndChild();
        ImGui::PopStyleColor();
        ImGui::Dummy(ImVec2(0, 4 * scale));
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 130 * scale);
        ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
        const bool enter = ImGui::InputTextWithHint("##chatInput", "Escreva uma mensagem...", chatInput.data(), chatInput.size(), ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::BeginDisabled(chatBusy);
        const bool send = ImGui::Button(chatBusy ? "Enviando..." : "Enviar", ImVec2(120 * scale, 0)) || enter;
        ImGui::EndDisabled();
        if (send && !chatBusy) {
            const std::string text = chatInput.data();
            if (text.find_first_not_of(' ') != std::string::npos) {
                PollChat(text);
                chatInput.fill('\0');
            }
            ImGui::SetKeyboardFocusHere(-1);
        }
        if (!problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::TextUnformatted(problem.c_str());
            ImGui::PopStyleColor();
        }
    }

    void ProfileFriends(float scale) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        std::vector<PlayerCard> list;
        std::string problem;
        long long at = 0;
        {
            std::lock_guard guard(friendsLock);
            for (const auto& card : friends) if (card.id != profile.id) list.push_back(card);
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
        ImGui::BeginDisabled(profileBusy);
        if (ImGui::Button(profileBusy ? "Atualizando..." : "Atualizar agora")) SyncProfiles(true);
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        if (!problem.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
            ImGui::PushTextWrapPos(0);
            ImGui::TextWrapped("%s", problem.c_str());
            ImGui::PopTextWrapPos();
            ImGui::PopStyleColor();
        } else if (at != 0) {
            std::string server;
            {
                std::lock_guard guard(friendsLock);
                server = friendsServer;
            }
            const std::string by = server == CentralServer(library.Root()) ? "pela central " + server : "pela sala em " + server;
            ImGui::TextDisabled("Atualizado %s, %s", FormatAgo(at).c_str(), by.c_str());
        }
        ImGui::Dummy(ImVec2(0, 6 * scale));
        if (ImGui::CollapsingHeader("Servidor dos amigos")) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Central: o servidor sempre ligado dos amigos (vazio: a do Ranga, na Cloudflare). Quando ela não responde, o Ranga "
                                "usa a sala do PC que hospeda, pelo playit. \"nenhuma\": só a sala do PC. O código do grupo é o mesmo para todos.");
            ImGui::PopTextWrapPos();
            ImGui::PushStyleColor(ImGuiCol_FrameBg, Color(kSelected));
            ImGui::SetNextItemWidth(320 * scale);
            ImGui::InputTextWithHint("##central", "https://ranga-central.nadroi-dev.workers.dev", centralField.data(), centralField.size());
            ImGui::SameLine();
            ImGui::SetNextItemWidth(200 * scale);
            ImGui::InputTextWithHint("##group", "código do grupo", groupField.data(), groupField.size());
            ImGui::PopStyleColor();
            ImGui::SameLine();
            if (ImGui::Button("Salvar servidor")) {
                SetCentralServer(library.Root(), centralField.data());
                SetGroupCode(library.Root(), groupField.data());
                OpenProfile();
                {
                    std::lock_guard guard(chatLock);
                    chatAt = 0;
                }
                AddToast("Servidor dos amigos", "Salvo. Conferindo agora...", "");
            }
            ImGui::Dummy(ImVec2(0, 6 * scale));
        }
        if (list.empty()) {
            ImGui::PushTextWrapPos(0);
            ImGui::TextDisabled("Nenhum amigo apareceu ainda. O perfil de cada um aparece aqui quando o Ranga dele abre com a sala do PC que hospeda no ar.");
            ImGui::PopTextWrapPos();
            return;
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
            ImGui::TextDisabled("Foto (PNG ou JPG; fica o quadrado do meio)");
            if (ImGui::Button("Escolher foto...")) {
                profileEdit = false;
                ImGui::CloseCurrentPopup();
                pending = [this] {
                    Browse(L"Escolher foto do perfil", L"", false, {{L"Imagens (png, jpg)", L"*.png;*.jpg;*.jpeg"}}, [this](const std::wstring& path) {
                        const std::string pixels = MakeAvatar(path);
                        if (pixels.empty()) {
                            Notify(L"Não deu para ler essa imagem. Escolha um arquivo PNG ou JPG.");
                        } else {
                            profile.SetAvatar(pixels);
                            for (const auto& title : profile.Evaluate(LibraryCount())) AddToast("Conquista desbloqueada", title, "");
                        }
                        OpenProfileEditor();
                    });
                };
            }
            if (!profile.avatar.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Tirar foto")) profile.SetAvatar("");
            }
            ImGui::EndGroup();
            ImGui::Dummy(ImVec2(0, 6 * scale));
            ImGui::TextDisabled("Nome");
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##editName", editName.data(), 41);
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
                if (editName[0] != '\0' && profile.name != editName.data()) profile.SetName(editName.data());
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
        toasts.erase(std::remove_if(toasts.begin(), toasts.end(), [&](const Toast& toast) { return toast.until < now; }), toasts.end());
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        float bottom = viewport->WorkPos.y + viewport->WorkSize.y - 16 * scale;
        for (size_t i = 0; i < toasts.size() && i < 4; ++i) {
            const Toast& toast = toasts[i];
            const float height = 74 * scale, width = 360 * scale;
            ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + 16 * scale, bottom), ImGuiCond_Always, ImVec2(0, 1));
            ImGui::SetNextWindowSize(ImVec2(width, height));
            ImGui::PushStyleColor(ImGuiCol_WindowBg, Color(kSurface));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 10 * scale);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(12 * scale, 12 * scale));
            char name[32];
            std::snprintf(name, sizeof(name), "##toast%zu", i);
            ImGui::Begin(name, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoSavedSettings |
                                            ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav);
            const ImVec2 at = ImGui::GetCursorScreenPos();
            DrawMedal(ImGui::GetWindowDrawList(), at, 50 * scale, toast.text, true);
            ImGui::SetCursorScreenPos(ImVec2(at.x + 62 * scale, at.y + 2 * scale));
            ImGui::BeginGroup();
            ImGui::TextDisabled("%s", toast.title.c_str());
            ImGui::PushFont(semibold, 18);
            ImGui::TextUnformatted(toast.text.c_str());
            ImGui::PopFont();
            ImGui::EndGroup();
            ImGui::End();
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor();
            bottom -= height + 10 * scale;
        }
    }

    bool NeedsFrames() const {
        return (gamepads.Connected() && GetForegroundWindow() == window) || std::fabs(homeTarget - homeScroll) > 0.0f || !toasts.empty();
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
        if (!profileLoaded) InitProfile();
        ProcessEnded();
        TakeRaLogin();
        SyncProfiles(false);
        NoticeChat();
        PollChat();
        chatTabOpen = false;
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
        static bool lastEngines = enginesPage, lastProfile = profileOpen;
        if (page != lastPage || enginesPage != lastEngines || profileOpen != lastProfile) barFocus = -1;
        lastProfile = profileOpen;
        if (enginesPage && !lastEngines) focusEngines = true;
        lastPage = page;
        lastEngines = enginesPage;
        // ImGui's own focus also runs an open list, such as Adicionar jogo's.
        const bool fieldNav = enginesPage || profileOpen || page == Page::Game || ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
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
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        if (profileOpen) {
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
        ImGui::End();
        UpdateBar(scale);
        Toasts(scale);
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
    // What an update left behind (the old RangaEmu.exe and cores, the download folder) goes once the new one runs.
    CleanUpdateLeftovers(root);
    // RangaEmu.exe --publicar "o que mudou": publishes this PC's Ranga for the friends without opening the window;
    // the result goes to salas\publicar.log.
    if (commandLine != nullptr && wcsstr(commandLine, L"--publicar") != nullptr) {
        std::wstring notes = wcsstr(commandLine, L"--publicar") + 10;
        while (!notes.empty() && (notes.front() == L' ' || notes.front() == L'"')) notes.erase(0, 1);
        while (!notes.empty() && (notes.back() == L' ' || notes.back() == L'"')) notes.pop_back();
        std::string version;
        const std::wstring error = PublishUpdate(root, Utf8(notes), version);
        WriteTextLine(root + L"\\salas\\publicar.log", error.empty() ? L"publicada " + Wide(version) : error);
        return error.empty() ? 0 : 1;
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
        static const wchar_t* shotPath = _wgetenv(L"RANGA_CLIENT_SHOT");
        if (IsIconic(hwnd) || (shotPath == nullptr && g_occluded && g_swapChain->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED)) {
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
