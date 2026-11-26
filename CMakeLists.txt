cmake_minimum_required(VERSION 3.20)
project(Relinker CXX)

set(ANYPS5_TEST_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

function(add_test_executable target)
    add_executable(${target} ${ARGN})
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "$<1:${ANYPS5_TEST_OUTPUT_DIRECTORY}>")
endfunction()

option(BUILD_TESTING "Build and register relinker tests" OFF)
if(BUILD_TESTING)
    enable_testing()
endif()

option(ANYPS5_ENABLE_SPIRV_TOOLS "Enable SPIRV-Tools validation and optimization" OFF)
option(APS5_ENABLE_TIMING_LOG "Enable frame timing logging" OFF)
add_compile_definitions(APS5_ENABLE_TIMING_LOG=$<BOOL:${APS5_ENABLE_TIMING_LOG}>)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static -static-libgcc -static-libstdc++")

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_VIDEO ON CACHE BOOL "" FORCE)
set(SDL_RENDER OFF CACHE BOOL "" FORCE)
set(SDL_X11 ON CACHE BOOL "" FORCE)
set(SDL_WAYLAND OFF CACHE BOOL "" FORCE)
set(SDL_OPENGL OFF CACHE BOOL "" FORCE)
set(SDL_OPENGLES OFF CACHE BOOL "" FORCE)
set(SDL_VULKAN ON CACHE BOOL "" FORCE)
set(SDL_JOYSTICK ON CACHE BOOL "" FORCE)
set(SDL_HAPTIC OFF CACHE BOOL "" FORCE)
set(SDL_HIDAPI ON CACHE BOOL "" FORCE)
set(SDL_SENSOR OFF CACHE BOOL "" FORCE)
set(SDL_STATIC_PIC ON CACHE BOOL "" FORCE)
set(ANYPS5_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS}")
set(CMAKE_EXE_LINKER_FLAGS "")
add_subdirectory(3rdparty/SDL2)
set(CMAKE_EXE_LINKER_FLAGS "${ANYPS5_EXE_LINKER_FLAGS}")
set(ARCHITECTURE x86_64)
add_subdirectory(3rdparty/ffmpeg-core)
if(MINGW)
    target_link_libraries(ffmpeg INTERFACE bcrypt ole32 user32 ws2_32)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(ffmpeg INTERFACE LINKER:--exclude-libs,ALL)
endif()

set(FT_DISABLE_ZLIB ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BZIP2 ON CACHE BOOL "" FORCE)
set(FT_DISABLE_PNG ON CACHE BOOL "" FORCE)
set(FT_DISABLE_HARFBUZZ ON CACHE BOOL "" FORCE)
set(FT_DISABLE_BROTLI ON CACHE BOOL "" FORCE)
set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
add_subdirectory(3rdparty/freetype EXCLUDE_FROM_ALL)
set_target_properties(freetype PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_subdirectory(core/shader/recompiler)
add_subdirectory(core/Decoder/Jpeg)
add_subdirectory(core/Decoder/Png)
add_subdirectory(core/libs)
add_subdirectory(core/relinker)

if(WIN32)
    set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/3rdparty/imgui)
    add_executable(RangaEmu WIN32
        launcher/Client.cpp launcher/Common.cpp launcher/Covers.cpp launcher/Dlss5.cpp launcher/Library.cpp launcher/Runner.cpp launcher/Systems.cpp launcher/Embed.cpp launcher/Rooms.cpp launcher/ConsoleGraphics.cpp launcher/Update.cpp launcher/Http.cpp launcher/PlayerProfile.cpp launcher/Lobby.cpp launcher/Media.cpp launcher/Backup.cpp launcher/Cloud.cpp launcher/Store.cpp launcher/Idioma.cpp launcher/Install.cpp launcher/Diagnosis.cpp launcher/CrashReport.cpp launcher/launcher.rc
        ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
        ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx11.cpp ${IMGUI_DIR}/misc/freetype/imgui_freetype.cpp)
    # The translations (launcher/idiomas.txt) go into the .rc: a change to them rebuilds it.
    set_source_files_properties(launcher/launcher.rc PROPERTIES OBJECT_DEPENDS ${CMAKE_SOURCE_DIR}/launcher/idiomas.txt)
    target_include_directories(RangaEmu PRIVATE ${IMGUI_DIR} ${IMGUI_DIR}/backends ${CMAKE_SOURCE_DIR}/3rdparty/stb)
    # IMGUI_USE_WCHAR32: characters past 0xFFFF, the chat's emoji among them.
    # IMGUI_ENABLE_FREETYPE: the fonts drawn by FreeType (3rdparty/freetype), which draws the emoji in their colors.
    target_compile_definitions(RangaEmu PRIVATE UNICODE _UNICODE IMGUI_USE_WCHAR32 IMGUI_ENABLE_FREETYPE)
    target_link_options(RangaEmu PRIVATE -municode -static)
    target_link_libraries(RangaEmu PRIVATE SDL2-static freetype d3d11 d3dcompiler dxgi dwmapi ole32 shell32 shlwapi uuid gdi32 comctl32 winhttp bcrypt ws2_32 iphlpapi mfplat mfreadwrite mfuuid)
    # Ranga Alpha: Ranga with the consoles still being worked on and nothing else, for this PC only. Built by name
    # (--target RangaAlpha), never part of a normal build, and it has no network, updates or publishing.
    get_target_property(RANGA_SOURCES RangaEmu SOURCES)
    add_executable(RangaAlpha WIN32 EXCLUDE_FROM_ALL ${RANGA_SOURCES})
    foreach(PROPERTY INCLUDE_DIRECTORIES COMPILE_DEFINITIONS LINK_OPTIONS LINK_LIBRARIES)
        get_target_property(VALUE RangaEmu ${PROPERTY})
        set_target_properties(RangaAlpha PROPERTIES ${PROPERTY} "${VALUE}")
    endforeach()
    target_compile_definitions(RangaAlpha PRIVATE RANGA_ALPHA)
    # The Alpha's private consoles (launcher/alpha, kept on the owner's PC and never published), only when it is there.
    if(EXISTS ${CMAKE_SOURCE_DIR}/launcher/alpha/Extra.cpp)
        target_sources(RangaAlpha PRIVATE launcher/alpha/Extra.cpp)
        target_compile_definitions(RangaAlpha PRIVATE RANGA_ALPHA_EXTRA)
    endif()
    # Ranga Retro: libretro cores inside Ranga's own window (launcher/retro).
    # RetroAchievements (3rdparty/rcheevos, MIT): the achievements Ranga Retro checks in the game's memory.
    enable_language(C)
    file(GLOB RCHEEVOS_SOURCES ${CMAKE_SOURCE_DIR}/3rdparty/rcheevos/src/*.c ${CMAKE_SOURCE_DIR}/3rdparty/rcheevos/src/rapi/*.c
         ${CMAKE_SOURCE_DIR}/3rdparty/rcheevos/src/rcheevos/*.c ${CMAKE_SOURCE_DIR}/3rdparty/rcheevos/src/rhash/*.c)
    add_library(rcheevos STATIC ${RCHEEVOS_SOURCES})
    target_include_directories(rcheevos PUBLIC ${CMAKE_SOURCE_DIR}/3rdparty/rcheevos/include ${CMAKE_SOURCE_DIR}/3rdparty/rcheevos/src ${CMAKE_SOURCE_DIR}/3rdparty/libretro)
    target_compile_definitions(rcheevos PUBLIC RC_CLIENT_SUPPORTS_HASH RC_DISABLE_LUA)
    # libchdr (3rdparty/libchdr, BSD): .chd discs for the RetroAchievements hash (launcher/retro/ChdReader.cpp).
    set(CHDR_WANT_TESTS OFF CACHE BOOL "" FORCE)
    add_subdirectory(${CMAKE_SOURCE_DIR}/3rdparty/libchdr ${CMAKE_BINARY_DIR}/libchdr EXCLUDE_FROM_ALL)
    add_executable(RangaRetro WIN32 launcher/retro/RetroMain.cpp launcher/retro/VulkanVideo.cpp launcher/retro/Netplay.cpp launcher/retro/Achievements.cpp launcher/retro/ChdReader.cpp launcher/retro/Social.cpp launcher/Http.cpp launcher/retro/retro.rc)
    set_target_properties(RangaRetro PROPERTIES OUTPUT_NAME ranga_retro)
    target_include_directories(RangaRetro PRIVATE ${CMAKE_SOURCE_DIR}/3rdparty/libretro ${CMAKE_SOURCE_DIR}/3rdparty/Vulkan-Headers/include ${CMAKE_SOURCE_DIR}/3rdparty/stb)
    target_compile_definitions(RangaRetro PRIVATE UNICODE _UNICODE)
    target_link_options(RangaRetro PRIVATE -municode -static)
    target_link_libraries(RangaRetro PRIVATE SDL2-static rcheevos chdr-static d3d11 d3dcompiler dxgi dwmapi opengl32 gdi32 ws2_32 winhttp)
    # The table server of online play for the consoles whose cores have none (launcher/net), shipped as toolsanga-sala.exe.
    add_executable(RangaSala launcher/net/RangaSala.cpp)
    set_target_properties(RangaSala PROPERTIES OUTPUT_NAME ranga-sala)
    target_link_options(RangaSala PRIVATE -static)
    target_link_libraries(RangaSala PRIVATE ws2_32)
endif()
