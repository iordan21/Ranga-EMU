#ifndef RANGA_RETRO_VULKAN_VIDEO_HPP
#define RANGA_RETRO_VULKAN_VIDEO_HPP

#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>

#include "libretro.h"

struct retro_hw_render_interface_vulkan;
struct retro_hw_render_context_negotiation_interface_vulkan;

// Ranga Retro's Vulkan path: the frontend side of libretro's Vulkan interface. It makes the instance, device and
// swapchain on the game window (letting the core choose them through the negotiation interface when it offers one),
// hands the core the queue it draws with, and presents each image the core sets by blitting it into the swapchain.
// Presenting through Vulkan is also what lets ReShade's Vulkan layer, and so DLSS 5, reach native games.
namespace RetroVulkan {

using LogFunction = void (*)(const char* format, ...);

bool Create(HWND window, const retro_hw_render_context_negotiation_interface_vulkan* negotiation, LogFunction log);
const retro_hw_render_interface_vulkan* Interface();
// Before retro_run: moves to the next sync index and waits until the GPU is done with it.
void BeginFrame();
// After retro_run: shows the image the core set, frameWidth x frameHeight of it, with the aspect ratio.
void Present(HWND window, unsigned frameWidth, unsigned frameHeight, float aspect);
void Resize();
// RANGA_RETRO_STATS: images the core set since the last call, and how many of them were a different image.
void TakeStats(int& sets, int& changes);
void Destroy(const retro_hw_render_context_negotiation_interface_vulkan* negotiation);

}

#endif
