#include "Dlss5.hpp"
#include "Library.hpp"

#include <cstdio>
#include <cwctype>

namespace Ranga {

namespace {

// addons\dlss5\camada holds the Vulkan layer manifests and their DLLs; addons\dlss5\jogo holds what goes beside the game's
// executable (add-ons, nvngx DLLs, shaders, settings).
std::wstring LayerFolder(const std::wstring& root) { return Dlss5Folder(root) + L"\\camada"; }
std::wstring GameFolder(const std::wstring& root) { return Dlss5Folder(root) + L"\\jogo"; }
std::wstring PlacedList(const std::wstring& workspace) { return workspace + L"\\dlss5.placed"; }

struct Entry {
    std::wstring name;
    bool directory = false;
};

std::vector<Entry> Entries(const std::wstring& folder, const wchar_t* pattern) {
    std::vector<Entry> entries;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((folder + L"\\" + pattern).c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return entries;
    do {
        const std::wstring name = found.cFileName;
        if (name != L"." && name != L"..") entries.push_back({name, (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileW(search, &found));
    FindClose(search);
    return entries;
}

bool EndsWith(const std::wstring& text, const std::wstring& end) {
    return text.size() >= end.size() && Lower(text.substr(text.size() - end.size())) == end;
}

// Settings the add-ons and ReShade write while the game runs. Each game gets a copy of its own, made once and kept when
// DLSS 5 is turned off, so what was tuned for one game is not lost and does not change another.
bool IsSettingsFile(const std::wstring& name) { return EndsWith(name, L".ini") || EndsWith(name, L".cfg"); }

void WriteLog(const std::wstring& path, const std::wstring& text) {
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"wb") != 0 || file == nullptr) return;
    const std::string bytes = Utf8(text);
    fwrite(bytes.data(), 1, bytes.size(), file);
    fclose(file);
}

constexpr const char* kReadMe =
    "\xEF\xBB\xBF"
    "DLSS 5 no Ranga Emu (experimental)\r\n"
    "==================================\r\n"
    "\r\n"
    "Funciona como o DLSS 5 Swapper faz com emuladores: o ReShade entra no Vulkan do jogo,\r\n"
    "o DLSS5-Feeder monta o pedido de DLSS que o jogo nunca faz, e um add-on neural roda o DLSS 5.\r\n"
    "Nada e instalado no Windows: o Ranga liga a camada so no processo do jogo e, ao desligar\r\n"
    "a opcao DLSS 5, tira de perto do jogo tudo o que colocou.\r\n"
    "\r\n"
    "O Ranga NAO baixa nenhum desses arquivos. Voce coloca cada um aqui.\r\n"
    "\r\n"
    "camada\\   A camada Vulkan do ReShade com suporte a add-ons: ReShade64.json e ReShade64.dll.\r\n"
    "          (Opcional) a camada do Feeder, VkLayer_feed_vk.json e VkLayer_feed_vk.dll, se o\r\n"
    "          dlss5-feed.log disser que faltam extensoes.\r\n"
    "\r\n"
    "jogo\\     Tudo o que iria ao lado do .exe de um jogo:\r\n"
    "          - dlss5-feed.addon64 (DLSS5-Feeder) e os arquivos dele;\r\n"
    "          - UM add-on neural: renodx-dlss5.addon64 OU deep-fried-chicken (nunca os dois);\r\n"
    "          - nvngx_dlss.dll e nvngx_dlssnr.dll (o DLSS 5);\r\n"
    "          - a pasta reshade-shaders com o DLSS5_Feed.fx;\r\n"
    "          - ReShade.ini e os .cfg/.ini dos add-ons, se quiser um ponto de partida.\r\n"
    "\r\n"
    "Arquivos .ini e .cfg viram uma copia por jogo (feita uma vez e mantida). O resto entra como\r\n"
    "link e sai quando a opcao e desligada. Se voce trocar um arquivo aqui, o jogo pega o novo na\r\n"
    "proxima vez que abrir.\r\n"
    "\r\n"
    "Avisos\r\n"
    "- O modelo neural so roda em placa RTX 50.\r\n"
    "- Pesa: nos testes publicos o FPS caiu perto da metade, e a VRAM sobe.\r\n"
    "- Desligue o Smooth Motion da NVIDIA para Vulkan, ou metade dos quadros sai sem DLSS 5.\r\n"
    "- Se algo der errado, olhe ReShade.log e dlss5-feed.log na pasta do jogo em saves\\<console>\\<jogo>,\r\n"
    "  e logs\\dlss5.log para ver o que o Ranga colocou.\r\n";

}

std::wstring Dlss5Folder(const std::wstring& root) { return root + L"\\addons\\dlss5"; }

void CreateDlss5Folder(const std::wstring& root) {
    CreateDirectoryW((root + L"\\addons").c_str(), nullptr);
    CreateDirectoryW(Dlss5Folder(root).c_str(), nullptr);
    CreateDirectoryW(LayerFolder(root).c_str(), nullptr);
    CreateDirectoryW(GameFolder(root).c_str(), nullptr);
    const std::wstring readMe = Dlss5Folder(root) + L"\\LEIA-ME.txt";
    if (Exists(readMe)) return;
    FILE* file = nullptr;
    if (_wfopen_s(&file, readMe.c_str(), L"wb") != 0 || file == nullptr) return;
    std::fputs(kReadMe, file);
    fclose(file);
}

Dlss5Kit InspectDlss5(const std::wstring& root) {
    Dlss5Kit kit;
    if (!IsDirectory(Dlss5Folder(root))) {
        kit.problem = L"a pasta addons\\dlss5 ainda não existe.";
        return kit;
    }
    std::wstring reshade;
    for (const auto& entry : Entries(LayerFolder(root), L"*.json")) {
        const std::string text = ReadBytes(LayerFolder(root) + L"\\" + entry.name);
        const size_t layer = text.find("\"layer\"");
        const size_t from = layer == std::string::npos ? 0 : layer;
        const std::wstring name = Wide(JsonString(text, "name", from));
        if (name.empty()) continue;
        // The loader resolves library_path from the manifest's folder; only its file name is checked here, since a
        // manifest may write ".\x.dll" with a stray escape that the loader accepts.
        std::wstring library = Wide(JsonString(text, "library_path", from));
        library = library.substr(library.find_last_of(L"\\/") == std::wstring::npos ? 0 : library.find_last_of(L"\\/") + 1);
        if (!library.empty() && library[0] == L'.') library.erase(0, 1);
        if (library.empty() || !Exists(LayerFolder(root) + L"\\" + library)) {
            kit.problem = L"a camada " + name + L" (" + entry.name + L") precisa de um DLL que não está na pasta camada.";
            return kit;
        }
        if (Lower(name).find(L"reshade") != std::wstring::npos) reshade = name;
        else kit.layers.push_back(name);
    }
    if (reshade.empty()) {
        kit.layers.clear();
        kit.problem = L"falta a camada Vulkan do ReShade (ReShade64.json e ReShade64.dll) na pasta camada.";
        return kit;
    }
    kit.layers.push_back(reshade);
    if (Entries(GameFolder(root), L"*.addon64").empty()) {
        kit.problem = L"faltam os add-ons do ReShade na pasta jogo (o dlss5-feed.addon64 e um add-on neural).";
        return kit;
    }
    if (!Exists(GameFolder(root) + L"\\nvngx_dlssnr.dll")) {
        kit.problem = L"falta o nvngx_dlssnr.dll (o DLSS 5) na pasta jogo.";
        return kit;
    }
    kit.ready = true;
    return kit;
}

std::wstring EnableDlss5(const std::wstring& root, const std::wstring& exeFolder, const std::wstring& workspace, const std::wstring& log) {
    const Dlss5Kit kit = InspectDlss5(root);
    if (!kit.ready) return L"O DLSS 5 está ligado, mas " + kit.problem + L" Complete a pasta " + Dlss5Folder(root) + L" ou desligue o DLSS 5 nas opções.";
    // Placed again on every start, so a file swapped in the kit reaches the game.
    DisableDlss5(exeFolder, workspace);
    const std::wstring leftover = L"\n" + ReadPathFile(PlacedList(workspace));
    const std::wstring linkLog = log.substr(0, log.find_last_of(L'\\')) + L"\\link.log";
    std::wstring report = L"DLSS 5 ligado para " + exeFolder + L"\r\n", placed;
    for (const auto& entry : Entries(GameFolder(root), L"*")) {
        const std::wstring source = GameFolder(root) + L"\\" + entry.name, target = exeFolder + L"\\" + entry.name;
        if (!entry.directory && IsSettingsFile(entry.name)) {
            if (Exists(target)) report += L"mantido (cópia deste jogo): " + entry.name + L"\r\n";
            else report += (CopyFileW(source.c_str(), target.c_str(), TRUE) ? L"copiado: " : L"FALHOU ao copiar: ") + entry.name + L"\r\n";
            continue;
        }
        // A file of the game's own or of the runtime with the same name is never touched.
        if (Exists(target)) {
            if (leftover.find(L"\n" + entry.name + L"\n") != std::wstring::npos) {
                report += L"ainda preso por um jogo aberto, fica o anterior: " + entry.name + L"\r\n";
                placed += entry.name + L"\n";
                continue;
            }
            report += L"já existe, não mexi: " + entry.name + L"\r\n";
            continue;
        }
        bool done = false;
        if (entry.directory) {
            LinkFolder(target, source, linkLog);
            done = Exists(target) && FinalPath(target) == FinalPath(source);
        } else {
            // A hard link costs no space; on another drive it has to be a copy.
            done = CreateHardLinkW(target.c_str(), source.c_str(), nullptr) || CopyFileW(source.c_str(), target.c_str(), TRUE);
        }
        report += (done ? L"colocado: " : L"FALHOU ao colocar: ") + entry.name + L"\r\n";
        if (done) placed += entry.name + L"\n";
    }
    WritePathFile(PlacedList(workspace), placed);
    std::wstring layers;
    for (const auto& name : kit.layers) layers += (layers.empty() ? L"" : L";") + name;
    SetEnvironmentVariableW(L"VK_LAYER_PATH", LayerFolder(root).c_str());
    SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", layers.c_str());
    report += L"VK_LAYER_PATH=" + LayerFolder(root) + L"\r\nVK_INSTANCE_LAYERS=" + layers + L"\r\n";
    WriteLog(log, report);
    return L"";
}

void DisableDlss5(const std::wstring& exeFolder, const std::wstring& workspace) {
    SetEnvironmentVariableW(L"VK_LAYER_PATH", nullptr);
    SetEnvironmentVariableW(L"VK_INSTANCE_LAYERS", nullptr);
    const std::wstring list = ReadPathFile(PlacedList(workspace));
    // What a game still open holds cannot go yet; it stays listed so a later start or turn-off takes it away.
    std::wstring kept;
    size_t start = 0;
    while (start < list.size()) {
        size_t end = list.find(L'\n', start);
        if (end == std::wstring::npos) end = list.size();
        const std::wstring name = list.substr(start, end - start);
        start = end + 1;
        // Only plain names were written; anything else did not come from here.
        if (name.empty() || name.find_first_of(L"\\/:") != std::wstring::npos || name == L"." || name == L"..") continue;
        const std::wstring path = exeFolder + L"\\" + name;
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) continue;
        // A junction goes without touching the kit folder it points at; a real folder with files in it stays.
        if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            RemoveDirectoryW(path.c_str());
        } else {
            SetFileAttributesW(path.c_str(), FILE_ATTRIBUTE_NORMAL);
            DeleteFileW(path.c_str());
        }
        if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) kept += name + L"\n";
    }
    if (kept.empty()) DeleteFileW(PlacedList(workspace).c_str());
    else WritePathFile(PlacedList(workspace), kept);
}

}
