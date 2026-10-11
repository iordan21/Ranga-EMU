// Ranga Emu client: a library of PS5 games on the left, the chosen game on the right, with Play and its options, cheats and logs.
#include "Common.hpp"
#include "Dlss5.hpp"
#include "Library.hpp"
#include "Runner.hpp"

#include <d3d11.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"

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

Texture LoadTexture(const std::wstring& path) {
    Texture texture;
    const std::string bytes = ReadBytes(path);
    if (bytes.empty()) return texture;
    int channels = 0;
    unsigned char* pixels = stbi_load_from_memory(reinterpret_cast<const stbi_uc*>(bytes.data()), static_cast<int>(bytes.size()), &texture.width, &texture.height, &channels, 4);
    if (pixels == nullptr) return texture;
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

    explicit Client(const std::wstring& root) : library(root) {}

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
            // The Bloodborne port writes everything to game.log; PS5 games put their errors in game.err.
            logFile = ProfileFor(game->titleId).bloodborne ? 1 : 0;
        }
    }

    void Reload(const std::wstring& keep) {
        library.Load();
        int index = library.Games().empty() ? -1 : 0;
        for (size_t i = 0; i < library.Games().size(); ++i) {
            if (library.Games()[i].titleId == keep) index = static_cast<int>(i);
        }
        Select(index);
    }

    const Texture& Icon(const Game& game) {
        auto found = icons.find(game.folder);
        if (found == icons.end()) found = icons.emplace(game.folder, LoadTexture(game.folder + L"\\sce_sys\\icon0.png")).first;
        return found->second;
    }

    const Texture& Banner(const Game& game) {
        auto found = banners.find(game.folder);
        // PS4 games have their background in pic1.png.
        if (found == banners.end()) found = banners.emplace(game.folder, LoadTexture(game.folder + (Exists(game.folder + L"\\sce_sys\\pic0.png") ? L"\\sce_sys\\pic0.png" : L"\\sce_sys\\pic1.png"))).first;
        return found->second;
    }

    void AddGame() {
        const std::wstring folder = PickFolder(window, library.Root(), L"Escolha a pasta do jogo (com eboot.bin e sce_sys)");
        if (folder.empty()) return;
        std::wstring error;
        const int index = library.Add(folder, error);
        if (index < 0) {
            MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
            return;
        }
        Select(index);
    }

    void Save() {
        if (Game* game = Current()) SaveOptions(*game, options);
    }

    void Sidebar(float scale) {
        const float width = 290 * scale;
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Color(kSidebar));
        ImGui::BeginChild("##sidebar", ImVec2(width, 0), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_NoScrollbar);
        ImGui::SetCursorPos(ImVec2(20 * scale, 20 * scale));
        ImGui::PushFont(semibold, 22);
        ImGui::TextUnformatted("Ranga Emu");
        ImGui::PopFont();
        ImGui::SetCursorPosX(20 * scale);
        ImGui::TextDisabled("Biblioteca");
        ImGui::Dummy(ImVec2(0, 4 * scale));

        const float listHeight = ImGui::GetContentRegionAvail().y - 64 * scale;
        ImGui::BeginChild("##games", ImVec2(0, listHeight), ImGuiChildFlags_None);
        const float row = 64 * scale;
        auto& games = library.Games();
        for (size_t i = 0; i < games.size(); ++i) {
            const Game& game = games[i];
            ImGui::PushID(static_cast<int>(i));
            ImGui::SetCursorPosX(10 * scale);
            const bool isSelected = static_cast<int>(i) == selected;
            if (ImGui::Selectable("##game", isSelected, ImGuiSelectableFlags_None, ImVec2(width - 20 * scale, row)) && !isSelected) Select(static_cast<int>(i));
            const ImVec2 min = ImGui::GetItemRectMin();
            ImDrawList* draw = ImGui::GetWindowDrawList();
            const float iconSize = row - 16 * scale;
            const ImVec2 iconMin(min.x + 8 * scale, min.y + 8 * scale), iconMax(iconMin.x + iconSize, iconMin.y + iconSize);
            const Texture& icon = Icon(game);
            if (icon.view != nullptr) draw->AddImageRounded(icon.Ref(), iconMin, iconMax, ImVec2(0, 0), ImVec2(1, 1), IM_COL32_WHITE, 6 * scale);
            else draw->AddRectFilled(iconMin, iconMax, Packed(kSurface), 6 * scale);
            const float textX = iconMax.x + 12 * scale;
            const float textWidth = min.x + width - 20 * scale - textX - 8 * scale;
            const std::string title = Utf8(game.title);
            ImGui::PushClipRect(ImVec2(textX, min.y), ImVec2(textX + textWidth, min.y + row), true);
            draw->AddText(semibold, ImGui::GetFontSize(), ImVec2(textX, min.y + 12 * scale), Packed(kText), title.c_str());
            ImGui::PopClipRect();
            const Profile gameProfile = ProfileFor(game.titleId);
            const bool tested = gameProfile.tested;
            const bool missing = !IsGameFolder(game.folder);
            const unsigned dot = missing ? kRed : tested ? kGreen : kAmber;
            const char* label = missing ? "pasta não encontrada" : gameProfile.bloodborne ? "port nativo (PS4)" : tested ? "testado" : "experimental";
            const float small = ImGui::GetFontSize() * 0.85f;
            draw->AddCircleFilled(ImVec2(textX + 4 * scale, min.y + row - 20 * scale), 3.5f * scale, Packed(dot));
            draw->AddText(nullptr, small, ImVec2(textX + 14 * scale, min.y + row - 20 * scale - small / 2), Packed(kDim), label);
            ImGui::PopID();
        }
        if (games.empty()) {
            ImGui::SetCursorPosX(20 * scale);
            ImGui::PushTextWrapPos(width - 20 * scale);
            ImGui::TextDisabled("Nenhum jogo ainda.");
            ImGui::PopTextWrapPos();
        }
        ImGui::EndChild();

        ImGui::SetCursorPos(ImVec2(16 * scale, ImGui::GetWindowHeight() - 52 * scale));
        if (ImGui::Button("+  Adicionar jogo", ImVec2(width - 32 * scale, 36 * scale))) pending = [this] { AddGame(); };
        ImGui::EndChild();
        ImGui::PopStyleColor();
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
            ImGui::TextDisabled("Roda pelo port nativo do Bloodborne (deadinside28, Mrsuss60 e shadPS4, licença GPL), não pelo emulador de PS5. Precisa da versão 1.09 com a atualização mesclada na pasta do jogo e do Python 3.10 ou mais novo.");
            ImGui::TextDisabled("No jogo, Insert ou L3 + R3 abre o menu de gráficos e FSR.");
            ImGui::PopTextWrapPos();
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
        ImGui::TextDisabled("Pasta do jogo");
        ImGui::TextWrapped("%s", Utf8(game.folder).c_str());
        if (ImGui::Button("Abrir pasta")) ShellExecuteW(window, L"open", game.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ImGui::SameLine();
        if (ImGui::Button("Trocar pasta...")) {
            const std::wstring folder = PickFolder(window, game.folder, L"Escolha a nova pasta do jogo");
            if (!folder.empty()) {
                pending = [this, folder] {
                    std::wstring error;
                    const int index = library.Add(folder, error);
                    if (index < 0) MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
                    else Select(index);
                };
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Remover da lista")) {
            const std::wstring question = L"Remover " + game.title + L" da lista?\n\nOs arquivos do jogo e os saves não são apagados.";
            if (MessageBoxW(window, question.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                pending = [this, index = static_cast<size_t>(selected)] {
                    library.Remove(index);
                    Reload(L"");
                };
            }
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
                    if (!error.empty()) MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
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
                if (MessageBoxW(window, question.c_str(), kAppName, MB_YESNO | MB_ICONQUESTION) == IDYES) {
                    pending = [this, label = addon.label] {
                        Game* current = Current();
                        if (current == nullptr) return;
                        RemoveAddon(*current, label);
                        addons = LoadAddons(*current);
                    };
                }
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

    void Detail(float scale) {
        ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_None);
        Game* game = Current();
        if (game == nullptr) {
            const ImVec2 size = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPos(ImVec2(size.x / 2 - 170 * scale, size.y / 2 - 50 * scale));
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 24);
            ImGui::TextUnformatted("Nenhum jogo na biblioteca");
            ImGui::PopFont();
            ImGui::TextDisabled("Adicione a pasta de um jogo do seu console.");
            ImGui::Dummy(ImVec2(0, 6 * scale));
            if (ImGui::Button("+  Adicionar jogo", ImVec2(200 * scale, 38 * scale))) pending = [this] { AddGame(); };
            ImGui::EndGroup();
            ImGui::EndChild();
            return;
        }
        const Profile profile = ProfileFor(game->titleId);
        const float width = ImGui::GetContentRegionAvail().x;
        const float bannerHeight = std::min(260 * scale, ImGui::GetContentRegionAvail().y * 0.36f);
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        const Texture& banner = Banner(*game);
        if (banner.view != nullptr) {
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
        } else {
            draw->AddRectFilled(origin, ImVec2(origin.x + width, origin.y + bannerHeight), Packed(kSurface));
        }
        draw->AddRectFilledMultiColor(ImVec2(origin.x, origin.y + bannerHeight * 0.35f), ImVec2(origin.x + width, origin.y + bannerHeight),
                                      Packed(kBackground, 0), Packed(kBackground, 0), Packed(kBackground), Packed(kBackground));
        ImGui::Dummy(ImVec2(width, bannerHeight - 40 * scale));

        const float pad = 32 * scale;
        ImGui::SetCursorPosX(pad);
        ImGui::BeginGroup();
        ImGui::PushFont(semibold, 30);
        ImGui::TextUnformatted(Utf8(game->title).c_str());
        ImGui::PopFont();
        std::string meta = Utf8(game->titleId);
        if (!game->version.empty()) meta += "   ·   v" + Utf8(game->version);
        ImGui::TextDisabled("%s", meta.c_str());
        ImGui::SameLine();
        const char* tag = profile.bloodborne ? "port nativo (PS4)" : profile.tested ? "testado" : "experimental";
        ImGui::PushStyleColor(ImGuiCol_Text, Color(profile.tested ? kGreen : kAmber));
        ImGui::TextUnformatted(tag);
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
        if (ImGui::Button(playLabel, ImVec2(170 * scale, 44 * scale))) {
            Save();
            runner.Start(library.Root(), *game, options);
            logDirty = true;
        }
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
        ImGui::BeginChild("##tabs", ImVec2(width - 2 * pad, 0), ImGuiChildFlags_None);
        if (ImGui::BeginTabBar("##tabbar")) {
            if (ImGui::BeginTabItem("Opções")) {
                ImGui::BeginChild("##options");
                OptionsTab(*game, profile, scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            if (profile.sonsOfSparta && ImGui::BeginTabItem("Cheats")) {
                ImGui::BeginChild("##cheats");
                CheatsTab(scale);
                ImGui::EndChild();
                ImGui::EndTabItem();
            }
            // Add-ons are mounted by the PS5 runtime; the Bloodborne port has no such mount.
            if (!profile.bloodborne && ImGui::BeginTabItem("DLC")) {
                ImGui::BeginChild("##dlc");
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
        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->WorkPos);
        ImGui::SetNextWindowSize(viewport->WorkSize);
        ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus);
        Sidebar(scale);
        ImGui::SameLine(0, 0);
        Detail(scale);
        ImGui::End();
    }
};

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
            if (g_runner != nullptr && g_runner->Current() == Runner::State::Preparing &&
                MessageBoxW(hwnd, L"O jogo ainda está sendo preparado. Fechar agora interrompe o preparo, que recomeça na próxima vez. Fechar mesmo assim?", kAppName, MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, message, wParam, lParam);
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE single = CreateMutexW(nullptr, TRUE, kMutex);
    if (single != nullptr && GetLastError() == ERROR_ALREADY_EXISTS) {
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
    const int width = std::min(static_cast<int>(1040 * scale), static_cast<int>(work.right - work.left));
    const int height = std::min(static_cast<int>(660 * scale), static_cast<int>(work.bottom - work.top));
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
    client.Reload(client.library.Selected());

    bool done = false;
    while (!done) {
        MSG message;
        // Idle frames wait for input, so the client costs nothing while a game runs; a busy runner keeps it ticking for status.
        if (!client.runner.Busy()) MsgWaitForMultipleObjects(0, nullptr, FALSE, 250, QS_ALLINPUT);
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
