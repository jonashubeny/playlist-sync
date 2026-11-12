# Sestaveni:  cmake -B build && cmake --build build
# Vysledek:   ./playlist-sync vedle tohoto souboru
cmake_minimum_required(VERSION 3.20)
project(playlist-sync LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(CURL REQUIRED IMPORTED_TARGET libcurl)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib)
pkg_check_modules(UTF8PROC REQUIRED IMPORTED_TARGET libutf8proc)
find_package(nlohmann_json 3.10 REQUIRED)

add_executable(playlist-sync playlist-sync.cpp)
target_compile_options(playlist-sync PRIVATE -Wall -Wextra)
target_link_libraries(playlist-sync PRIVATE
    PkgConfig::CURL
    PkgConfig::TAGLIB
    PkgConfig::UTF8PROC
    nlohmann_json::nlohmann_json
)

# binarka vedle zdrojaku, protoze config.json, playlist.json a preskocit.txt
# hleda ve sve slozce
set_target_properties(playlist-sync PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR})
