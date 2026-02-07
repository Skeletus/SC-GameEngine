add_library(sc_engine STATIC)

target_sources(sc_engine
  PRIVATE
    src/sc_app.cpp
    src/sc_assets.cpp
    src/sc_debug_draw.cpp
    src/sc_imgui.cpp
    src/sc_vk.cpp
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
    ${CMAKE_SOURCE_DIR}/third_party
)

target_include_directories(sc_engine
  PRIVATE
    ${bullet3_SOURCE_DIR}/src
)

target_link_libraries(sc_engine
  PUBLIC
    sc_core
    SDL2::SDL2-static
    Vulkan::Vulkan
  PRIVATE
    imgui
    BulletDynamics
    BulletCollision
    LinearMath
)

if (SC_ENABLE_WARNINGS)
  target_compile_options(sc_engine PRIVATE /W4)
endif()
