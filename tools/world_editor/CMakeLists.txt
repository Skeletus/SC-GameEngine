add_executable(tools_world_editor
  main.cpp
  editor_core/editor_core.cpp
  editor_core/sc_asset_db.cpp
  ${CMAKE_SOURCE_DIR}/third_party/ImGuizmo/ImGuizmo.cpp
)

include(SCShaders)

target_include_directories(tools_world_editor PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/editor_core
  ${CMAKE_SOURCE_DIR}/third_party/ImGuizmo
)

target_link_libraries(tools_world_editor PRIVATE
  engine_render
  sc_world_shared
  imgui
  SDL2::SDL2
)

set_target_properties(tools_world_editor PROPERTIES OUTPUT_NAME "sc_world_editor")

if (SC_ENABLE_WARNINGS)
  target_compile_options(tools_world_editor PRIVATE /W4)
endif()

target_compile_definitions(tools_world_editor PRIVATE SDL_MAIN_HANDLED)

sc_compile_shaders(tools_world_editor)

add_custom_command(
  TARGET tools_world_editor
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:tools_world_editor>/assets"
  COMMENT "Copying editor assets folder"
)

add_custom_command(
  TARGET tools_world_editor
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:SDL2::SDL2>"
          "$<TARGET_FILE_DIR:tools_world_editor>"
  COMMENT "Copying SDL2 runtime"
)

add_custom_command(
  TARGET tools_world_editor
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:imgui>"
          "$<TARGET_FILE_DIR:tools_world_editor>"
  COMMENT "Copying ImGui runtime"
)
