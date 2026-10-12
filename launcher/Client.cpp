// Ranga Emu client: a library of games of every console on the left, the chosen game on the right, with Play and its
// options, cheats and logs, and an Emulators page where each console other than PS5 gets the emulator that runs it.
#include "Common.hpp"
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
    // The Emulators page takes the place of the game while it is open.
    bool enginesPage = false;
    // Each console's emulator, read again whenever the page changes one, so drawing never reads engines.ini.
    std::map<std::wstring, Engine> engines;
    std::map<std::wstring, std::array<char, 1024>> argsEdit, fullscreenEdit;
    // A game file whose extension fits several consoles waits here until the user says which.
    std::wstring askFile;
    Detected askDetected;

    explicit Client(const std::wstring& root) : library(root) {}

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
    }

    const Engine& EngineFor(const std::wstring& system) {
        static const Engine none;
        const auto found = engines.find(system);
        return found != engines.end() ? found->second : none;
    }

    static bool External(const Game& game) { return game.system != L"PS5" && !ProfileFor(game.titleId).bloodborne; }

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

    // A cover the user chose wins; otherwise the art the game itself carries, when its console has any.
    static std::wstring ArtFile(const Game& game, bool icon) {
        const std::wstring art = game.workspace + L"\\art";
        if (Exists(art + L"\\cover.png")) return art + L"\\cover.png";
        if (game.system == L"PS5" || game.system == L"PS4") {
            // PS4 games have their background in pic1.png.
            if (icon) return game.folder + L"\\sce_sys\\icon0.png";
            return game.folder + (Exists(game.folder + L"\\sce_sys\\pic0.png") ? L"\\sce_sys\\pic0.png" : L"\\sce_sys\\pic1.png");
        }
        if (game.system == L"PS3" && IsDirectory(game.folder)) {
            const std::wstring base = IsDirectory(game.folder + L"\\PS3_GAME") ? game.folder + L"\\PS3_GAME" : game.folder;
            return base + (icon ? L"\\ICON0.PNG" : L"\\PIC1.PNG");
        }
        if (game.system == L"PSP") return art + (icon ? L"\\ICON0.PNG" : L"\\PIC1.PNG");
        return L"";
    }

    const Texture& Icon(const Game& game) {
        auto found = icons.find(game.titleId);
        if (found == icons.end()) found = icons.emplace(game.titleId, LoadTexture(ArtFile(game, true))).first;
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
            MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
            return;
        }
        enginesPage = false;
        Select(index);
    }

    void AddGameFile() {
        const std::wstring path = PickFile(window, L"", L"Escolha o arquivo do jogo (ISO, ROM, NSP...)", GameFileFilters());
        if (path.empty()) return;
        const Detected found = DetectFile(path);
        if (found.systems.empty()) {
            const std::wstring text = L"O Ranga não sabe qual console abre arquivos " + (Extension(path).empty() ? std::wstring(L"sem extensão") : Extension(path)) + L":\n\n" + path;
            MessageBoxW(window, text.c_str(), kAppName, MB_OK | MB_ICONWARNING);
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
        MessageBoxW(window, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
        enginesPage = false;
        if (selected < 0 && !library.Games().empty()) Select(0);
    }

    void AddDetected(const std::wstring& path, const Detected& found, const std::wstring& system) {
        std::wstring error;
        const int index = library.AddFile(path, system, found.serial, found.title, error);
        if (index < 0) {
            MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
            return;
        }
        enginesPage = false;
        Select(index);
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

        const float listHeight = ImGui::GetContentRegionAvail().y - 106 * scale;
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
            else {
                // No art: the console's short name stands in for it.
                draw->AddRectFilled(iconMin, iconMax, Packed(kSurface), 6 * scale);
                const std::string tag = Utf8(game.system);
                const float size = ImGui::GetFontSize() * (tag.size() > 4 ? 0.62f : 0.8f);
                const ImVec2 extent = semibold->CalcTextSizeA(size, FLT_MAX, 0, tag.c_str());
                draw->AddText(semibold, size, ImVec2((iconMin.x + iconMax.x - extent.x) / 2, (iconMin.y + iconMax.y - extent.y) / 2), Packed(kDim), tag.c_str());
            }
            const float textX = iconMax.x + 12 * scale;
            const float textWidth = min.x + width - 20 * scale - textX - 8 * scale;
            const std::string title = Utf8(game.title);
            ImGui::PushClipRect(ImVec2(textX, min.y), ImVec2(textX + textWidth, min.y + row), true);
            draw->AddText(semibold, ImGui::GetFontSize(), ImVec2(textX, min.y + 12 * scale), Packed(kText), title.c_str());
            ImGui::PopClipRect();
            const Profile gameProfile = ProfileFor(game.titleId);
            const bool missing = !GameAvailable(game);
            bool ready = gameProfile.tested;
            std::string status = gameProfile.bloodborne ? "experimento (PS4)" : gameProfile.tested ? "testado" : "experimental";
            if (External(game)) {
                const Engine& engine = EngineFor(game.system);
                ready = engine.ready;
                status = Utf8(game.system) + "  ·  " + (!engine.ready ? std::string("falta emulador") : engine.presetName.empty() ? Utf8(FileName(engine.exe)) : Utf8(engine.presetName));
            }
            if (missing) status = "não encontrado";
            const unsigned dot = missing ? kRed : ready ? kGreen : kAmber;
            const char* label = status.c_str();
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

        ImGui::SetCursorPos(ImVec2(16 * scale, ImGui::GetWindowHeight() - 94 * scale));
        if (ImGui::Button("+  Adicionar jogo", ImVec2(width - 32 * scale, 36 * scale))) ImGui::OpenPopup("##add");
        AddMenu(scale);
        ImGui::SetCursorPosX(16 * scale);
        ImGui::PushStyleColor(ImGuiCol_Button, Color(enginesPage ? kSelected : kSurface));
        if (ImGui::Button("Emuladores", ImVec2(width - 32 * scale, 36 * scale))) {
            enginesPage = !enginesPage;
            RefreshEngines();
        }
        ImGui::PopStyleColor();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }

    void AddMenu(float scale) {
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(8 * scale, 8 * scale));
        if (ImGui::BeginPopup("##add")) {
            if (ImGui::Selectable("Pasta do jogo...")) pending = [this] { AddGame(); };
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
            if (!engine.ready) {
                ImGui::PushStyleColor(ImGuiCol_Text, Color(kAmber));
                ImGui::TextWrapped("%s", Utf8(engine.problem).c_str());
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
                        if (index < 0) MessageBoxW(window, error.c_str(), kAppName, MB_OK | MB_ICONWARNING);
                        else Select(index);
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
        if (ImGui::Button("Voltar para o jogo")) enginesPage = false;
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
                                MessageBoxW(window, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
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

    void Detail(float scale) {
        if (enginesPage) {
            EnginesPage(scale);
            return;
        }
        ImGui::BeginChild("##detail", ImVec2(0, 0), ImGuiChildFlags_None);
        Game* game = Current();
        if (game == nullptr) {
            const ImVec2 size = ImGui::GetContentRegionAvail();
            ImGui::SetCursorPos(ImVec2(size.x / 2 - 170 * scale, size.y / 2 - 50 * scale));
            ImGui::BeginGroup();
            ImGui::PushFont(semibold, 24);
            ImGui::TextUnformatted("Nenhum jogo na biblioteca");
            ImGui::PopFont();
            ImGui::TextDisabled("Adicione a pasta ou o arquivo de um jogo do seu console.");
            ImGui::Dummy(ImVec2(0, 6 * scale));
            if (ImGui::Button("Pasta do jogo...", ImVec2(200 * scale, 38 * scale))) pending = [this] { AddGame(); };
            ImGui::SameLine();
            if (ImGui::Button("Arquivo do jogo...", ImVec2(200 * scale, 38 * scale))) pending = [this] { AddGameFile(); };
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
            good = engine.ready;
            tag = !engine.ready ? "falta emulador" : "via " + (engine.presetName.empty() ? Utf8(FileName(engine.exe)) : Utf8(engine.presetName));
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
        if (ImGui::Button(playLabel, ImVec2(170 * scale, 44 * scale))) {
            Save();
            RefreshEngines();
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
            // Add-ons are mounted by the PS5 runtime; the Bloodborne port and other emulators have no such mount.
            if (game->system == L"PS5" && ImGui::BeginTabItem("DLC")) {
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
        AskSystem(scale);
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
