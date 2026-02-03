cmake_minimum_required(VERSION 3.24)

project(SandboxCityEngine LANGUAGES CXX)

# -------------------------
# Global settings
# -------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(SC_ENABLE_WARNINGS "Enable high warning levels" ON)

# Output folders (nice for VS)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)

# -------------------------
# Dependencies
# -------------------------
include(FetchContent)

# SDL2 (window + input)
FetchContent_Declare(
  SDL2
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-2.32.10
)

# Queremos estático (más simple para el engine al inicio)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON  CACHE BOOL "" FORCE)

# Clave: evita rutas del CMake de SDL que esperan targets "SDL2"
set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
set(SDL_TEST            OFF CACHE BOOL "" FORCE)

# Opcional: si NO quieres SDL2main (tú ya usas SDL_MAIN_HANDLED)
set(SDL2_DISABLE_SDL2MAIN ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(SDL2)

find_package(Vulkan REQUIRED)

# Dear ImGui (debug UI)
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.91.5
)

FetchContent_MakeAvailable(imgui)

add_library(imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl2.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

target_include_directories(imgui PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)

target_link_libraries(imgui PUBLIC SDL2::SDL2-static Vulkan::Vulkan)

# -------------------------
# Engine modules
# -------------------------
add_subdirectory(src/core)
add_subdirectory(src/engine)
add_subdirectory(src/sandbox)
