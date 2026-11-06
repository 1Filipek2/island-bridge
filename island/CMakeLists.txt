cmake_minimum_required(VERSION 3.28)

project(IslandBridgeIsland VERSION 1.0.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "IslandBridge.Island builds with MSVC only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION_RELEASE ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

option(ISLAND_PCH "Use the precompiled header" ON)

set(ISLAND_VERSION "1.0.0-dev" CACHE STRING "Version string reported by the island")

add_library(island_options INTERFACE)

target_include_directories(island_options INTERFACE src)

target_compile_definitions(island_options INTERFACE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    ISLAND_VERSION=L"${ISLAND_VERSION}"
)

target_compile_options(island_options INTERFACE /utf-8 /EHsc /permissive- /Zc:__cplusplus /guard:cf /bigobj /MP)

add_library(island_modules OBJECT
    src/core/color.cpp
    src/core/localization.cpp
    src/core/settings.cpp
    src/core/state.cpp
    src/core/tabs.cpp
    src/core/util.cpp
    src/island.cpp
    src/media/audio.cpp
    src/media/lyrics.cpp
    src/media/lyrics_cache.cpp
    src/media/media.cpp
    src/media/media_source.cpp
    src/notifications/capture.cpp
    src/notifications/dnd.cpp
    src/notifications/notification_center.cpp
    src/pipe/pipe_protocol.cpp
    src/pipe/pipe_server.cpp
    src/services/geo.cpp
    src/services/github.cpp
    src/services/http.cpp
    src/services/location.cpp
    src/services/quick_lookup.cpp
    src/services/weather.cpp
    src/system/bluetooth.cpp
    src/system/input_hooks.cpp
    src/system/monitor.cpp
    src/system/privacy.cpp
    src/system/system_monitor.cpp
    src/ui/actions.cpp
    src/ui/backdrop.cpp
    src/ui/context_menu.cpp
    src/ui/file_tray.cpp
    src/ui/hit_test.cpp
    src/ui/icons.cpp
    src/ui/image.cpp
    src/ui/placement.cpp
    src/ui/render/alerts.cpp
    src/ui/render/artwork.cpp
    src/ui/render/cards.cpp
    src/ui/render/dashboards.cpp
    src/ui/render/frame.cpp
    src/ui/render/lookup.cpp
    src/ui/render/lyrics.cpp
    src/ui/render/media.cpp
    src/ui/render/thread.cpp
    src/ui/window.cpp
)

target_link_libraries(island_modules PRIVATE island_options)
target_compile_options(island_modules PRIVATE /W3)
if(ISLAND_PCH)
    target_precompile_headers(island_modules PRIVATE src/core/common.h)
endif()

add_executable(IslandBridge.Island WIN32
    src/main.cpp
    src/host/host.cpp
    src/host/defaults.cpp
    src/island.manifest
    $<TARGET_OBJECTS:island_modules>
)

target_link_libraries(IslandBridge.Island PRIVATE island_options)
target_compile_options(IslandBridge.Island PRIVATE /W4 /WX)

target_link_options(IslandBridge.Island PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT /CETCOMPAT)

include(CTest)

if(BUILD_TESTING)
    function(island_test name)
        add_executable(${name} tests/${name}.cpp ${ARGN})
        target_link_libraries(${name} PRIVATE island_options advapi32 ole32 oleaut32 runtimeobject)
        target_compile_options(${name} PRIVATE /W4 /WX)
        add_test(NAME ${name} COMMAND ${name})
    endfunction()

    island_test(notification_center_tests src/notifications/notification_center.cpp)
    island_test(island_pipe_tests src/pipe/pipe_protocol.cpp tests/fake_image.cpp)
    island_test(geo_tests src/services/geo.cpp)
endif()

target_link_libraries(IslandBridge.Island PRIVATE
    advapi32
    ole32
    oleaut32
    shcore
    d2d1
    dwrite
    dwmapi
    gdi32
    user32
    shell32
    runtimeobject
    windowscodecs
    setupapi
    winhttp
    pdh
    winmm
)
