#include "Diagnosis.hpp"
#include "Common.hpp"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <initializer_list>

namespace Ranga {

namespace {

std::string Head(const std::wstring& path, size_t size) {
    std::string out(size, '\0');
    FILE* file = nullptr;
    if (_wfopen_s(&file, path.c_str(), L"rb") != 0 || file == nullptr) return "";
    out.resize(std::fread(out.data(), 1, size, file));
    std::fclose(file);
    return out;
}

std::string LowerAscii(std::string text) {
    for (char& c : text)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + 32);
    return text;
}

bool Has(const std::string& text, std::initializer_list<const char*> any) {
    return std::any_of(any.begin(), any.end(), [&](const char* part) { return text.find(part) != std::string::npos; });
}

Diagnosis Make(bool sure, const char* reason, const char* todo, Diagnosis::Fix fix = Diagnosis::Fix::None) {
    Diagnosis out;
    out.sure = sure;
    out.reason = reason;
    out.todo = todo;
    out.fix = fix;
    return out;
}

Diagnosis InFolder(Diagnosis out, const std::wstring& folder, const std::wstring& detail) {
    out.fix = Diagnosis::Fix::OpenFolder;
    out.folder = folder;
    out.detail = detail;
    return out;
}

// The first file a .cue, .gdi or .m3u asks for that is not beside it; empty when all are there.
std::wstring MissingTrack(const std::wstring& rom, const std::wstring& ext) {
    const std::string text = Head(rom, 256 * 1024);
    const std::wstring folder = ParentFolder(rom);
    size_t start = 0;
    bool first = true;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (first && line.rfind("\xEF\xBB\xBF", 0) == 0) line.erase(0, 3);
        const bool firstLine = first;
        first = false;
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(0, 1);
        std::string name;
        if (ext == L".m3u") {
            if (!line.empty() && line.front() != '#') name = line;
        } else if (ext == L".cue") {
            if (LowerAscii(line).rfind("file ", 0) != 0) continue;
            const size_t a = line.find('"'), b = line.rfind('"');
            if (a != std::string::npos && b > a) name = line.substr(a + 1, b - a - 1);
            else name = line.substr(5, line.rfind(' ') - 5);
        } else { // .gdi: "1 0 4 2352 track01.bin 0", the name quoted when it has spaces
            if (firstLine) continue;
            const size_t a = line.find('"'), b = line.rfind('"');
            if (a != std::string::npos && b > a) {
                name = line.substr(a + 1, b - a - 1);
            } else {
                size_t at = 0;
                for (int field = 0; field < 4 && at != std::string::npos; ++field) at = line.find_first_not_of(' ', line.find(' ', at));
                if (at != std::string::npos) name = line.substr(at, line.find(' ', at) - at);
            }
        }
        if (name.empty()) continue;
        std::wstring path = Wide(name);
        // Names written in the PC's own code page read wrong as UTF-8; those are left alone.
        if (path.find(L'\xFFFD') != std::wstring::npos) continue;
        std::replace(path.begin(), path.end(), L'/', L'\\');
        const bool absolute = path.size() > 2 && path[1] == L':';
        if (!Exists(absolute ? path : folder + L"\\" + path)) return path;
    }
    return L"";
}

// "Eden: ... (código N)": N is the core's status; from 7 on, the loader's (eden src/core/loader/loader.h).
Diagnosis FromEden(const std::string& log) {
    const size_t at = log.rfind("Eden: ");
    if (at == std::string::npos) return {};
    const size_t end = log.find('\n', at);
    const std::string line = log.substr(at, end == std::string::npos ? end : end - at);
    const size_t open = line.rfind("(c");
    const size_t number = open == std::string::npos ? open : line.find_first_of("0123456789", open);
    if (number == std::string::npos) return {};
    const int status = std::atoi(line.c_str() + number);
    if (status == 5) return Make(true, "A placa de vídeo não conseguiu iniciar o Vulkan.", "Atualize o driver da placa de vídeo pelo site do fabricante e reinicie o PC.");
    const int loader = status - 7;
    if ((loader >= 13 && loader <= 15) || (loader >= 18 && loader <= 23))
        return Make(true, "As chaves do Switch não abrem este jogo: faltam chaves ou elas são mais antigas que ele.",
                    "Tire de novo as chaves (prod.keys e title.keys) e o firmware do seu console, atualizado.");
    if (loader == 24 || loader == 25 || loader == 28 || loader == 52 || loader == 60)
        return Make(true, "O arquivo é uma atualização ou um DLC, não o jogo.", "Abra o arquivo do jogo base; a atualização e o DLC entram nas opções do jogo.");
    if (status == 2 || loader == 4 || (loader >= 10 && loader <= 12) || loader == 27 || loader == 29 || loader == 30)
        return Make(true, "O arquivo do jogo está incompleto ou corrompido.", "Copie o jogo de novo do seu console.");
    if (status == 3 || status == 4) return Make(true, "O firmware do Switch está faltando ou incompleto.", "Tire de novo o firmware do seu console.");
    return {};
}

Diagnosis FromRpcs3(const std::string& log) {
    if (log.find("RPCS3:") == std::string::npos) return {};
    if (Has(log, {"Failed to decrypt", "Missing decryption key", "Wrong decryption key"}))
        return Make(true, "O jogo está criptografado e a chave dele não está no PC.", "Coloque a licença (.rap) ou a chave do disco tirada do seu próprio console.");
    if (Has(log, {"Firmware is missing", "Firmware is too old", "falta o firmware"}))
        return Make(true, "O firmware do PS3 está faltando ou é antigo demais para este jogo.", "Baixe o PS3UPDAT.PUP no site da Sony e use o botão Instalar firmware do PS3, na página do jogo.");
    if (Has(log, {"Invalid file or folder", "Invalid dev_bdvd", "Nothing to boot", "disc type is not supported"}))
        return Make(true, "O arquivo do jogo está incompleto ou corrompido.", "Copie o jogo de novo do seu console.");
    if (Has(log, {"Savestate"})) return Make(true, "O estado salvo não serve nesta versão do emulador.", "Abra o jogo sem carregar o estado salvo.");
    return {};
}

}

Diagnosis CheckGameFile(const std::wstring& rom, bool starting) {
    if (rom.empty()) return {};
    const std::wstring folder = ParentFolder(rom), name = FileName(rom);
    const DWORD attributes = GetFileAttributesW(rom.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES)
        return InFolder(Make(true, "O arquivo do jogo não está mais no lugar.", "Ponha o arquivo de volta ou escolha o novo lugar em \"Trocar\", nas opções do jogo."), folder, name);
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) return {};
    // Reading a cloud file would make Windows download it whole, so it is told by its attributes.
    if ((attributes & (FILE_ATTRIBUTE_OFFLINE | 0x00400000 /* RECALL_ON_DATA_ACCESS */)) != 0) {
        if (starting) return {};
        return InFolder(Make(true, "O jogo está só na nuvem (OneDrive ou parecido), não neste PC.", "Clique com o botão direito no arquivo e escolha manter sempre neste dispositivo, ou mova o jogo para fora do OneDrive."),
                        folder, name);
    }
    const long long size = FileSize(rom);
    if (size == 0) return InFolder(Make(true, "O arquivo do jogo está vazio (0 bytes).", "Copie o jogo de novo do seu console."), folder, name);
    const std::string head = Head(rom, 0x9000);
    if (head.empty()) return InFolder(Make(true, "O Windows não deixou ler o arquivo do jogo.", "Veja se outro programa está usando o arquivo e se o disco está com defeito."), folder, name);
    const std::wstring ext = Extension(rom);
    const std::string start = LowerAscii(head.substr(0, 512));
    if (ext != L".m3u" && ext != L".cue" && ext != L".gdi" && (start.find("<!doctype html") != std::string::npos || start.find("<html") == 0))
        return InFolder(Make(true, "O arquivo é uma página da internet salva no lugar do jogo.", "Copie o jogo de novo do seu console."), folder, name);
    if (ext == L".cue" || ext == L".gdi" || ext == L".m3u") {
        if (const std::wstring missing = MissingTrack(rom, ext); !missing.empty())
            return InFolder(Make(true, "Falta um arquivo do disco que este jogo pede.", "Ponha todos os arquivos do jogo na mesma pasta, com os nomes originais."), folder, missing);
        return {};
    }
    const auto at = [&](size_t offset, const char* magic, size_t length) { return head.size() >= offset + length && head.compare(offset, length, std::string(magic, length)) == 0; };
    const bool zip = at(0, "PK\x03\x04", 4), seven = at(0, "7z\xBC\xAF", 4), rar = at(0, "Rar!", 4);
    // Only kinds with one mark, always in the same place.
    struct Kind { const wchar_t* ext; size_t offset; const char* magic; size_t length; };
    static const Kind kinds[] = {{L".chd", 0, "MComprHD", 8}, {L".nsp", 0, "PFS0", 4},  {L".nsz", 0, "PFS0", 4},  {L".xci", 0x100, "HEAD", 4}, {L".rvz", 0, "RVZ\x01", 4},
                                 {L".wbfs", 0, "WBFS", 4},    {L".cso", 0, "CISO", 4},  {L".cci", 0x100, "NCSD", 4}, {L".wux", 0, "WUX0", 4}};
    for (const Kind& kind : kinds) {
        if (ext != kind.ext || at(kind.offset, kind.magic, kind.length)) continue;
        // A full card dump has its key area first.
        if (ext == L".xci" && at(0x1100, "HEAD", 4)) continue;
        if (zip || seven || rar)
            return InFolder(Make(true, "O jogo ainda está compactado (zip, rar ou 7z).", "Extraia o jogo e adicione o arquivo que sair."), folder, name);
        return InFolder(Make(true, "O arquivo do jogo está incompleto ou corrompido.", "Copie o jogo de novo do seu console."), folder, name);
    }
    if (rar || ((ext == L".iso" || ext == L".bin" || ext == L".3ds" || ext == L".gcm" || ext == L".xiso") && (zip || seven)))
        return InFolder(Make(true, "O jogo ainda está compactado (zip, rar ou 7z).", "Extraia o jogo e adicione o arquivo que sair."), folder, name);
    return {};
}

Diagnosis Diagnose(const std::wstring& root, const Game& game, const Engine& engine, const std::wstring& rom, const Options& options, unsigned long code,
                   unsigned long seconds) {
    // 0: closed by the player. 0xCFFFFFFF: closed by Windows as it shuts down. 1 in an external emulator, or after a
    // while, is how some end when the window is closed.
    if (code == 0 || code == 0xCFFFFFFFul) return {};
    const bool crashed = (code & 0xF0000000ul) == 0xC0000000ul || code == 0xE06D7363ul || code == 0x80000003ul;
    if (!crashed && seconds >= 30) return {};
    if (!crashed && !engine.native && code == 1 && seconds >= 8) return {};
    const std::string log = ReadTail(game.workspace + L"\\logs\\game.log", 512 * 1024);
    const std::string low = LowerAscii(log);

    // Windows could not even start the program: nothing in the log.
    if (code == 0xC0000135ul || code == 0xC0000139ul)
        return Make(false, "Falta no Windows uma peça que o emulador usa (Visual C++).", "Instale o Visual C++ Redistributable da Microsoft e tente de novo.", Diagnosis::Fix::VcRuntime);
    if (code == 0xC000001Dul)
        return Make(false, "O processador deste PC não tem uma instrução que este emulador usa (AVX).", "Este console precisa de um processador mais novo.");
    if (Diagnosis file = CheckGameFile(rom); !file.reason.empty()) return file;

    ULARGE_INTEGER free{};
    if (GetDiskFreeSpaceExW(root.c_str(), &free, nullptr, nullptr) && free.QuadPart < 300ull * 1024 * 1024)
        return Make(false, "O disco do Ranga está cheio.", "Libere espaço: o emulador precisa gravar saves e cache.");

    if (engine.native) {
        if (code == 3)
            return Make(true, "O núcleo deste console não carregou (faltando, corrompido ou bloqueado pelo antivírus).",
                        "Use Verificar agora para baixar o que faltar; se sumir de novo, libere a pasta do Ranga no antivírus.", Diagnosis::Fix::Repair);
        if (code == 4) {
            if (log.find("este core n\xC3\xA3o inicia sem jogo") != std::string::npos)
                return Make(true, "Este console não abre o sistema sem um jogo.", "Escolha um jogo e aperte Jogar.");
            if (game.system == L"SWITCH")
                if (Diagnosis eden = FromEden(log); !eden.reason.empty()) return eden;
            if (game.system == L"PS3")
                if (Diagnosis rpcs3 = FromRpcs3(log); !rpcs3.reason.empty()) return rpcs3;
            if (Has(low, {"encrypted", "decrypt"}))
                return Make(false, "O jogo parece estar criptografado.", "Tire do seu console uma cópia descriptografada.");
            if (Has(low, {"bios not found", "missing bios", "no bios", "firmware is missing", "firmware not found", "bios is missing"}))
                return InFolder(Make(false, "O emulador não achou a BIOS ou o firmware que este jogo pede.", "Ponha na pasta a BIOS da mesma região do jogo, tirada do seu console."), root + L"\\bios\\retroarch", L"");
            return Make(true, "O emulador não aceitou o arquivo do jogo.", "Veja se o jogo é deste console e se a cópia está completa.");
        }
        if (code == 5) return Make(true, "A placa de vídeo não conseguiu iniciar o gráfico deste console.", "Atualize o driver da placa de vídeo pelo site do fabricante e reinicie o PC.");
    }
    if (Has(low, {"device_lost", "device lost", "device removed", "dxgi_error_device"}))
        return Make(false, "O driver de vídeo parou no meio do jogo.", "Atualize o driver da placa de vídeo e tire o overclock dela, se tiver.");
    if (Has(low, {"out_of_device_memory", "out_of_host_memory", "bad_alloc", "out of memory"}) || code == 0xC0000017ul)
        return Make(false, "Faltou memória no PC ou na placa de vídeo.", "Feche outros programas e diminua a resolução nas opções do console.");
    // The layer sits inside the emulator's process, so it is the first thing to take out of the way.
    if (crashed && options.dlss5)
        return Make(false, "O emulador fechou com erro, e o DLSS 5 está ligado neste jogo.", "Desligue o DLSS 5 e tente de novo para saber se era ele.", Diagnosis::Fix::Dlss5Off);
    if (crashed && seconds < 60)
        return Make(false, "O emulador fechou com erro ao abrir o jogo.", "Feche outros programas, atualize o driver de vídeo e tente de novo. Se repetir, gere o relatório de erro.");
    if (crashed) return Make(false, "O emulador fechou sozinho no meio do jogo.", "Abra de novo e continue do último save. Se repetir no mesmo lugar, gere o relatório de erro.");
    return Make(false, "O emulador fechou antes de abrir o jogo.", "Tente de novo. Se repetir, gere o relatório de erro.");
}

}
