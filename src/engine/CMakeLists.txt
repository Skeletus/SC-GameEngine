add_library(sc_engine STATIC)

target_sources(sc_engine
  PRIVATE
    src/sc_app.cpp
    src/sc_assets.cpp
    src/sc_debug_draw.cpp
    src/sc_imgui.cpp
    src/sc_vk.cpp
    world/sc_world_partition.cpp
)

target_include_directories(sc_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/world
    ${CMAKE_SOURCE_DIR}/third_party
)

target_link_libraries(sc_engine
  PUBLIC
    sc_core
    SDL2::SDL2-static
    Vulkan::Vulkan
  PRIVATE
    imgui
)

if (SC_ENABLE_WARNINGS)
  target_compile_options(sc_engine PRIVATE /W4)
endif()
