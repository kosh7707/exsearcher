cmake_minimum_required(VERSION 3.21)
project(exsearcher LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/utf-8 /W4 /permissive-)
endif()

# --- core static library (Qt-independent) ---
add_library(exsearcher-core STATIC
    core/src/Index.cpp
    core/src/CrawlIndexer.cpp
    core/src/SearchEngine.cpp
    core/src/WinText.cpp
)
target_include_directories(exsearcher-core PUBLIC core/include)

# --- CLI test tool ---
add_executable(exsearcher-cli cli/main.cpp)
target_link_libraries(exsearcher-cli PRIVATE exsearcher-core)

# Static CRT so the CLI exe is dependency-free (no VC redist).
set_target_properties(exsearcher-core exsearcher-cli PROPERTIES
    MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
)
