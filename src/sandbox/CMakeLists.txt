add_executable(sc_sandbox)

include(SCShaders)

target_sources(sc_sandbox
  PRIVATE
    src/main.cpp
)

target_link_libraries(sc_sandbox
  PRIVATE
    sc_engine
)

if (SC_ENABLE_WARNINGS)
  target_compile_options(sc_sandbox PRIVATE /W4)
endif()

# On Windows, SDL2 sometimes needs this for a console app.
target_compile_definitions(sc_sandbox PRIVATE SDL_MAIN_HANDLED)

sc_compile_shaders(sc_sandbox)

add_custom_command(
  TARGET sc_sandbox
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
          "${CMAKE_SOURCE_DIR}/assets"
          "$<TARGET_FILE_DIR:sc_sandbox>/assets"
  COMMENT "Copying runtime assets folder"
)

add_custom_command(
  TARGET sc_sandbox
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:SDL2::SDL2>"
          "$<TARGET_FILE_DIR:sc_sandbox>"
  COMMENT "Copying SDL2 runtime"
)

add_custom_command(
  TARGET sc_sandbox
  POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          "$<TARGET_FILE:imgui>"
          "$<TARGET_FILE_DIR:sc_sandbox>"
  COMMENT "Copying ImGui runtime"
)
