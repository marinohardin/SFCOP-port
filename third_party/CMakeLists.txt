# The native build's dependencies (CONVENTIONS §5), each pinned by a tagged release and the
# SHA-256 of its archive; INVENTORY.md ("Build dependencies") says what each is for. Only the
# integrator adds, removes or bumps one. Archives are fetched at configure time into the build
# directory (_deps/); nothing is vendored.
#
# Targets:
#   SDL3::SDL3-static   SDL3, static (window, GPU, input, events)
#   sfcop_sdl3          the SDL3 the client's own code links: the static library, except on macOS with
#                       SFCOP_LIFT, where it is the shared one (see below)
#   sfcop_miniaudio     miniaudio, static (audio engine)
#   sfcop_doctest       doctest headers (C++ tests)
#   sfcop_imgui         Dear ImGui with its SDL3 and SDL_Renderer backends, static (the launcher)
#   SDL2::SDL2          TEMPORARY, with SFCOP_LIFT only: sdl2-compat, the SDL2 API over SDL3, for
#                       the copied boot platform; leaves with Wave 1 B1/B3
include(FetchContent)

# SDL 3.4.18, release asset; tag release-3.4.18 is commit 829a65d769d935c4852f8159e964312c0957260a.
FetchContent_Declare(SDL3
    URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz
    URL_HASH SHA256=9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3
    DOWNLOAD_EXTRACT_TIMESTAMP ON)
# miniaudio 0.11.25: the tag's commit archive (the release has no asset).
FetchContent_Declare(miniaudio
    URL https://github.com/mackron/miniaudio/archive/9634bedb5b5a2ca38c1ee7108a9358a4e233f14d.tar.gz
    URL_HASH SHA256=1a3a79b80fc6f0b0cc155e28b954a598e0ddfa2db64e2afa8466be88c476fa55
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR do-not-add-its-cmake)
# doctest 2.5.3, release asset; tag v2.5.3 is commit 2d0a9359a60c51affe2a9bebb1be1dca47868151.
FetchContent_Declare(doctest
    URL https://github.com/doctest/doctest/releases/download/v2.5.3/doctest-v2.5.3.tar.gz
    URL_HASH SHA256=eb4a39729d091e43d4d593888bf4d145ee9a0f0e13f7404484b6f2fb60d5d50f
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR do-not-add-its-cmake)
# Dear ImGui 1.93.0: the tag's commit archive (the release has no source asset); tag v1.93.0 is
# commit c1aaac65e896a45f81f47a4d68f9959e0f86c859.
FetchContent_Declare(imgui
    URL https://github.com/ocornut/imgui/archive/c1aaac65e896a45f81f47a4d68f9959e0f86c859.tar.gz
    URL_HASH SHA256=b3d8f99838bb3ff05e14185562345f85e00c032da4d8fc277608d7898d847114
    DOWNLOAD_EXTRACT_TIMESTAMP ON
    SOURCE_SUBDIR do-not-add-its-cmake)

# TEMPORARY: leaves with Wave 1 B1/B3. sdl2-compat 2.32.74, release asset; the boot platform
# (src/app/boot/) and win32hle are written against the SDL2 API. It is a shared library that
# loads libSDL3.dylib from its own directory, so SDL3 is also built shared and both are written
# next to sfcop.
FetchContent_Declare(sdl2_compat
    URL https://github.com/libsdl-org/sdl2-compat/releases/download/release-2.32.74/sdl2-compat-2.32.74.tar.gz
    URL_HASH SHA256=ec68abde77e2e459c8abc4f9587b8976b5d2a657b2bfda2fb3a079b0f8924588
    DOWNLOAD_EXTRACT_TIMESTAMP ON)

set(SDL_SHARED ${SFCOP_LIFT})
set(SDL_STATIC ON)
set(SDL_TEST_LIBRARY OFF)
set(SDL_TESTS OFF)
set(SDL_EXAMPLES OFF)
set(SDL_INSTALL OFF)
set(SDL_DISABLE_INSTALL ON)
if(SFCOP_LIFT)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
endif()
FetchContent_MakeAvailable(SDL3 miniaudio doctest imgui)
if(SFCOP_LIFT)
    set(SDL2COMPAT_TESTS OFF)
    set(SDL2COMPAT_INSTALL OFF)
    FetchContent_MakeAvailable(sdl2_compat)
endif()

# One copy of SDL3 per process on macOS. sdl2-compat loads libSDL3.dylib, so the bundle ships it;
# linking the client's own code to a static SDL3 as well would put SDL's Objective-C classes and
# globals in the process twice (the runtime warns "implemented in both"). So there the client
# links the shared library too. Linux and Windows keep the static library (no shared Objective-C
# class registry to clash, and on Windows the link order in the root CMakeLists.txt matters).
add_library(sfcop_sdl3 INTERFACE)
if(APPLE AND SFCOP_LIFT)
    target_link_libraries(sfcop_sdl3 INTERFACE SDL3::SDL3-shared)
else()
    target_link_libraries(sfcop_sdl3 INTERFACE SDL3::SDL3-static)
endif()

add_library(sfcop_miniaudio STATIC "${miniaudio_SOURCE_DIR}/miniaudio.c")
target_include_directories(sfcop_miniaudio SYSTEM PUBLIC "${miniaudio_SOURCE_DIR}")
target_compile_options(sfcop_miniaudio PRIVATE -w)
if(APPLE)
    target_link_libraries(sfcop_miniaudio PUBLIC
        "-framework CoreFoundation" "-framework CoreAudio" "-framework AudioToolbox")
endif()

add_library(sfcop_doctest INTERFACE)
target_include_directories(sfcop_doctest SYSTEM INTERFACE "${doctest_SOURCE_DIR}")

# The launcher's window (src/app/launcher/): the core and the SDL3 platform and SDL_Renderer
# backends only, so it needs no shaders and stays clear of the game's SDL GPU device.
add_library(sfcop_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(sfcop_imgui SYSTEM PUBLIC
    "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(sfcop_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
target_compile_options(sfcop_imgui PRIVATE -w)
target_link_libraries(sfcop_imgui PUBLIC sfcop_sdl3)
