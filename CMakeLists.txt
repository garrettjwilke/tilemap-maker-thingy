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

if(EMSCRIPTEN)
    add_library(nfd STATIC
        src/wasm/nfd_wasm.cpp
        src/wasm/web_file_io.cpp
    )
    target_include_directories(nfd PUBLIC
        deps/nativefiledialog-extended/src/include
        src/wasm
    )
else()
    set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(NFD_INSTALL OFF CACHE BOOL "" FORCE)
    add_subdirectory(deps/nativefiledialog-extended EXCLUDE_FROM_ALL)
endif()

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
    src/core/zip.cpp
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

add_library(tileset_core STATIC
    deps/tileset-maker-thingy/src/core/md_color.cpp
    deps/tileset-maker-thingy/src/core/tileset_doc.cpp
    deps/tileset-maker-thingy/src/core/atlas_doc.cpp
    deps/tileset-maker-thingy/src/core/convert.cpp
    deps/tileset-maker-thingy/src/core/io.cpp
    deps/tileset-maker-thingy/src/core/palette_presets.cpp
    deps/tileset-maker-thingy/src/core/draw.cpp
    deps/tileset-maker-thingy/src/core/project.cpp
)
target_include_directories(tileset_core PUBLIC
    deps/tileset-maker-thingy/src
    deps/tileset-maker-thingy/src/core
    deps/gentileset
)
target_link_libraries(tileset_core PUBLIC gentileset)

if(NOT EMSCRIPTEN)
    add_executable(tilemap-maker-tests
        tests/test_main.cpp
        src/app/settings.cpp
        src/app/theme.cpp
        deps/tileset-maker-thingy/src/app/tileset_editor.cpp
        deps/tileset-maker-thingy/src/app/settings.cpp
    )
    target_include_directories(tilemap-maker-tests PRIVATE
        src
        src/core
        deps/tileset-maker-thingy/src
        deps/tileset-maker-thingy/src/app
    )
    target_link_libraries(tilemap-maker-tests PRIVATE tilemap_core tileset_core imgui nfd SDL3::SDL3)

    enable_testing()
    add_test(NAME tilemap-maker-self-test COMMAND tilemap-maker-tests)
endif()

add_executable(tilemap-maker
    src/app/main.cpp
    src/app/editor.cpp
    src/app/settings.cpp
    src/app/theme.cpp
    deps/tileset-maker-thingy/src/app/tileset_editor.cpp
    deps/tileset-maker-thingy/src/app/settings.cpp
)
target_include_directories(tilemap-maker PRIVATE
    src
    src/core
    deps/tileset-maker-thingy/src
    deps/tileset-maker-thingy/src/app
)
target_link_libraries(tilemap-maker PRIVATE tilemap_core tileset_core imgui nfd SDL3::SDL3)

if(EMSCRIPTEN)
    set_target_properties(tilemap-maker PROPERTIES
        SUFFIX ".js"
    )
    target_link_options(tilemap-maker PRIVATE
        "-sUSE_SDL=0"
        "-sALLOW_MEMORY_GROWTH=1"
        "-sMAX_WEBGL_VERSION=2"
        "-sEXPORTED_RUNTIME_METHODS=ccall,cwrap,FS,stringToNewUTF8,UTF8ToString"
        "-sEXPORTED_FUNCTIONS=_main,_wasm_on_file_uploaded,_wasm_c_open_file,_wasm_c_open_map,_wasm_c_import_tileset,_wasm_c_open_project,_wasm_on_pinch,_free"
    )

    add_custom_command(TARGET tilemap-maker POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/shell.html
            ${CMAKE_CURRENT_BINARY_DIR}/tilemap-maker.html
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/shell.html
            ${CMAKE_CURRENT_BINARY_DIR}/index.html
        COMMAND ${CMAKE_COMMAND} -E make_directory
            ${CMAKE_CURRENT_BINARY_DIR}/dist
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/shell.html
            ${CMAKE_CURRENT_BINARY_DIR}/dist/index.html
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_SOURCE_DIR}/src/wasm/shell.html
            ${CMAKE_CURRENT_BINARY_DIR}/dist/tilemap-maker.html
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/tilemap-maker.js
            ${CMAKE_CURRENT_BINARY_DIR}/dist/tilemap-maker.js
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${CMAKE_CURRENT_BINARY_DIR}/tilemap-maker.wasm
            ${CMAKE_CURRENT_BINARY_DIR}/dist/tilemap-maker.wasm
        COMMENT "Staging clean web distribution files in dist/"
    )
endif()
