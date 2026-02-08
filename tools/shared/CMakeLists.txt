add_library(sc_world_shared STATIC
  world_format.cpp
  asset_registry.cpp
  mesh_importer.cpp
  mesh_importer_glb.cpp
)

target_include_directories(sc_world_shared PUBLIC
  ${CMAKE_CURRENT_LIST_DIR}
  ${CMAKE_SOURCE_DIR}/third_party
)

target_compile_features(sc_world_shared PUBLIC cxx_std_17)
