add_library(sc_engine STATIC)

target_sources(sc_engine
  PRIVATE
    src/sc_app.cpp
)

target_include_directories(sc_engine
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(sc_engine
  PUBLIC
    sc_core
    SDL2::SDL2-static
)

if (SC_ENABLE_WARNINGS)
  target_compile_options(sc_engine PRIVATE /W4)
endif()
