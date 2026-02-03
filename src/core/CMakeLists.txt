add_library(sc_core STATIC)

target_sources(sc_core
  PRIVATE
    src/sc_log.cpp
    src/sc_memtrack.cpp
    src/sc_memory.cpp
    src/sc_time.cpp
    src/sc_jobs.cpp
)

target_include_directories(sc_core
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

if (SC_ENABLE_WARNINGS)
  target_compile_options(sc_core PRIVATE /W4)
endif()

target_compile_definitions(sc_core PRIVATE
  $<$<CONFIG:Debug>:SC_DEBUG=1>
  $<$<CONFIG:Release>:SC_RELEASE=1>
)
