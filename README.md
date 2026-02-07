# SandboxCityEngine

GTA-like open-world engine (Windows + MSVC).  
Monorepo: Core + Engine + Game + Tools.

## Build (Visual Studio CMake)
Open folder in Visual Studio (CMake project) and select preset `msvc-debug` or `msvc-release`.

Full configure + build (Visual Studio 2022)
cmake -S . -B build -G "Visual Studio 17 2022"
cmake --build build --config Debug

Release
cmake --build build --config Release

Build just the editor
cmake --build build --config Debug --target sc_world_editor

Build just the runtime
cmake --build build --config Debug --target sc_sandbox
