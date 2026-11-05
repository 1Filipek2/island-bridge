cmake_minimum_required(VERSION 3.28)

project(IslandBridgeIsland VERSION 1.0.0 LANGUAGES CXX)

if(NOT MSVC)
    message(FATAL_ERROR "IslandBridge.Island builds with MSVC only")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(ISLAND_VERSION "1.0.0-dev" CACHE STRING "Version string reported by the island")

add_executable(IslandBridge.Island WIN32
    src/main.cpp
    src/island.cpp
    src/host/host.cpp
    src/host/defaults.cpp
    src/island.manifest
)

target_include_directories(IslandBridge.Island PRIVATE src)

target_compile_definitions(IslandBridge.Island PRIVATE
    UNICODE
    _UNICODE
    WINVER=0x0A00
    _WIN32_WINNT=0x0A00
    ISLAND_VERSION=L"${ISLAND_VERSION}"
)

target_compile_options(IslandBridge.Island PRIVATE /utf-8 /EHsc /permissive- /Zc:__cplusplus /guard:cf /bigobj /MP)

set_source_files_properties(src/island.cpp PROPERTIES COMPILE_OPTIONS "/W3")
set_source_files_properties(src/main.cpp src/host/host.cpp src/host/defaults.cpp PROPERTIES COMPILE_OPTIONS "/W4;/WX")

target_link_options(IslandBridge.Island PRIVATE /guard:cf /DYNAMICBASE /NXCOMPAT /CETCOMPAT)

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
