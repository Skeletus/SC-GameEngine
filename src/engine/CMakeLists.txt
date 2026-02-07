add_library(engine_render SHARED)

target_sources(engine_render
  PRIVATE
    src/sc_assets.cpp
    src/sc_debug_draw.cpp
    src/sc_imgui.cpp
    src/sc_vk.cpp
    src/sc_engine_render.cpp
)

target_include_directories(engine_render
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/physics
    ${CMAKE_CURRENT_SOURCE_DIR}/traffic
    ${CMAKE_CURRENT_SOURCE_DIR}/world
    ${CMAKE_SOURCE_DIR}/tools/shared
)

target_link_libraries(engine_render
  PUBLIC
    sc_core
    SDL2::SDL2
    Vulkan::Vulkan
  PRIVATE
    imgui
)

target_compile_definitions(engine_render PRIVATE SC_RENDER_EXPORTS)

set_target_properties(engine_render PROPERTIES
  OUTPUT_NAME "engine_render"
  WINDOWS_EXPORT_ALL_SYMBOLS ON
)

if (SC_ENABLE_WARNINGS)
  target_compile_options(engine_render PRIVATE /W4)
endif()

add_library(sc_engine STATIC)

target_sources(sc_engine
  PRIVATE
    src/sc_app.cpp
    src/sc_debug_draw_system.cpp
    physics/sc_physics.cpp
    physics/sc_vehicle.cpp
    traffic/sc_traffic_ai.cpp
    traffic/sc_traffic_lanes.cpp
    traffic/sc_traffic_lod.cpp
    traffic/sc_traffic_spawner.cpp
    world/sc_world_partition.cpp
)

target_include_directories(sc_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/physics
    ${CMAKE_CURRENT_SOURCE_DIR}/traffic
    ${CMAKE_CURRENT_SOURCE_DIR}/world
    ${CMAKE_SOURCE_DIR}/tools/shared
    ${CMAKE_SOURCE_DIR}/third_party
)

target_include_directories(sc_engine
  PRIVATE
    ${bullet3_SOURCE_DIR}/src
)

target_link_libraries(sc_engine
  PUBLIC
    sc_core
    SDL2::SDL2
    engine_render
    sc_world_shared
  PRIVATE
    BulletDynamics
    BulletCollision
    LinearMath
)

if (SC_ENABLE_WARNINGS)
  target_compile_options(sc_engine PRIVATE /W4)
endif()
