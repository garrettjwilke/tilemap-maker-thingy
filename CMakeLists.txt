cmake_minimum_required(VERSION 3.16)
project(tilemap-maker-thingy C CXX)
if(APPLE)
    enable_language(OBJC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 99)

set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/SDL EXCLUDE_FROM_ALL)

set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/nativefiledialog-extended EXCLUDE_FROM_ALL)

add_library(gentileset STATIC
    deps/gentileset/gentileset.c
    deps/gentileset/lodepng.c
)
target_include_directories(gentileset PUBLIC deps/gentileset)
target_compile_definitions(gentileset PUBLIC GENTILESET_NO_MAIN)

add_library(tilemap_core STATIC
    src/core/collision.cpp
    src/core/io.cpp
    src/core/tilemap_doc.cpp
    src/core/tileset.cpp
)
target_include_directories(tilemap_core PUBLIC src)
target_link_libraries(tilemap_core PUBLIC gentileset)

add_library(imgui STATIC
    deps/imgui/imgui.cpp
    deps/imgui/imgui_draw.cpp
    deps/imgui/imgui_tables.cpp
    deps/imgui/imgui_widgets.cpp
    deps/imgui/backends/imgui_impl_sdl3.cpp
    deps/imgui/backends/imgui_impl_sdlrenderer3.cpp
)
target_include_directories(imgui PUBLIC deps/imgui deps/imgui/backends)
target_link_libraries(imgui PUBLIC SDL3::SDL3)

add_executable(tilemap-maker-tests
    tests/test_main.cpp
    src/app/settings.cpp
)
target_link_libraries(tilemap-maker-tests PRIVATE tilemap_core)

add_executable(tilemap-maker
    src/app/main.cpp
    src/app/editor.cpp
    src/app/settings.cpp
)
target_link_libraries(tilemap-maker PRIVATE tilemap_core imgui nfd SDL3::SDL3)

enable_testing()
add_test(NAME tilemap-maker-self-test COMMAND tilemap-maker-tests)
