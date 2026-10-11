#ifndef LAUNCHER_DLSS5_HPP
#define LAUNCHER_DLSS5_HPP

#include "Common.hpp"

#include <string>
#include <vector>

// DLSS 5 the way DLSS 5 Swapper gives it to emulators: ReShade's Vulkan layer, the DLSS5-Feeder add-on and one neural
// add-on, all supplied by the user in addons\dlss5 beside this program. Nothing is installed in Windows: the layers are
// turned on only for the game's own process, through its environment, and the files put beside the game's executable are
// links listed in dlss5.placed in the workspace, so turning the option off takes away exactly those and nothing else.
namespace Ranga {

struct Dlss5Kit {
    bool ready = false;
    // Why the kit cannot be used yet; empty when it is ready.
    std::wstring problem;
    // Layer names for VK_INSTANCE_LAYERS, ReShade's last so the others sit above it.
    std::vector<std::wstring> layers;
};

std::wstring Dlss5Folder(const std::wstring& root);
Dlss5Kit InspectDlss5(const std::wstring& root);
// Makes the kit's folders and its LEIA-ME, so the user knows what goes where.
void CreateDlss5Folder(const std::wstring& root);
// Puts the kit's game files beside the executable in exeFolder and sets the layer variables; empty on success, otherwise
// why DLSS 5 could not be turned on.
std::wstring EnableDlss5(const std::wstring& root, const std::wstring& exeFolder, const std::wstring& workspace, const std::wstring& log);
// Takes away what EnableDlss5 put beside the executable and clears the layer variables.
void DisableDlss5(const std::wstring& exeFolder, const std::wstring& workspace);

}

#endif
