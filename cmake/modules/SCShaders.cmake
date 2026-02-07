include_guard()

function(sc_compile_shaders target)
  if (NOT TARGET ${target})
    message(FATAL_ERROR "sc_compile_shaders: target '${target}' does not exist.")
  endif()

  # Locate glslangValidator from the Vulkan SDK.
  find_program(GLSLANG_VALIDATOR
    NAMES glslangValidator glslangValidator.exe
    HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/Bin32"
    REQUIRED
  )

  set(SC_SHADERS_DIR "${CMAKE_SOURCE_DIR}/assets/shaders")
  if (NOT EXISTS "${SC_SHADERS_DIR}")
    message(FATAL_ERROR "sc_compile_shaders: shaders directory not found: ${SC_SHADERS_DIR}")
  endif()

  # Collect shader sources (supports subfolders).
  file(GLOB_RECURSE SC_SHADER_SOURCES
    CONFIGURE_DEPENDS
    "${SC_SHADERS_DIR}/*.vert"
    "${SC_SHADERS_DIR}/*.frag"
    "${SC_SHADERS_DIR}/*.comp"
  )

  if (SC_SHADER_SOURCES STREQUAL "")
    message(STATUS "sc_compile_shaders: no shaders found in ${SC_SHADERS_DIR}")
    return()
  endif()

  # Intermediate output per-config to avoid collisions in multi-config generators.
  set(SC_SHADER_BIN_DIR "${CMAKE_BINARY_DIR}/shaders/$<CONFIG>")

  set(SC_SHADER_SPV_OUTPUTS "")

  foreach (SC_SHADER_FILE IN LISTS SC_SHADER_SOURCES)
    file(RELATIVE_PATH SC_SHADER_REL "${SC_SHADERS_DIR}" "${SC_SHADER_FILE}")
    set(SC_SHADER_OUT_REL "${SC_SHADER_REL}.spv")
    set(SC_SHADER_OUT "${SC_SHADER_BIN_DIR}/${SC_SHADER_OUT_REL}")

    get_filename_component(SC_SHADER_OUT_DIR "${SC_SHADER_OUT}" DIRECTORY)

    add_custom_command(
      OUTPUT "${SC_SHADER_OUT}"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${SC_SHADER_OUT_DIR}"
      COMMAND ${GLSLANG_VALIDATOR} -V "${SC_SHADER_FILE}" -o "${SC_SHADER_OUT}"
      DEPENDS "${SC_SHADER_FILE}"
      COMMENT "Compiling shader ${SC_SHADER_REL}"
      VERBATIM
    )

    list(APPEND SC_SHADER_SPV_OUTPUTS "${SC_SHADER_OUT}")
  endforeach()

  # Build target that compiles all shaders.
  set(SC_SHADERS_TARGET "${target}_shaders")
  add_custom_target(${SC_SHADERS_TARGET} DEPENDS ${SC_SHADER_SPV_OUTPUTS})

  # Ensure the main target builds shaders first.
  add_dependencies(${target} ${SC_SHADERS_TARGET})

  # Copy compiled shaders into assets/shaders so resolveAssetPath finds them.
  set(SC_RUNTIME_SHADER_DIR "$<TARGET_FILE_DIR:${target}>/assets/shaders")
  add_custom_command(
    TARGET ${SC_SHADERS_TARGET}
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "${SC_RUNTIME_SHADER_DIR}"
    COMMENT "Copying shaders to ${SC_RUNTIME_SHADER_DIR}"
    VERBATIM
  )

  foreach (SC_SHADER_SPV IN LISTS SC_SHADER_SPV_OUTPUTS)
    add_custom_command(
      TARGET ${SC_SHADERS_TARGET}
      POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
              "${SC_SHADER_SPV}"
              "${SC_RUNTIME_SHADER_DIR}"
      VERBATIM
    )
  endforeach()
endfunction()
