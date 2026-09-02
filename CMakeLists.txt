cmake_minimum_required(VERSION 3.20)

project(CnaRts LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    # A benchmark built at -O0 measures the debugger, not the engine.
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# ---------------------------------------------------------------------------
# cna-rts drives ../cnanext (the CNA runtime) through the sibling
# ../sharp-runtimenext checkout, exactly like cna-samples and cna-killer do, so a
# single sibling checkout is fixed up by every consumer in the same session.
# ---------------------------------------------------------------------------

if(EMSCRIPTEN)
    set(_cna_rts_reference_renderer "WEBGL2")
else()
    set(_cna_rts_reference_renderer "OPENGLES3")
endif()
set(CNA_GRAPHICS_RENDERER "${_cna_rts_reference_renderer}" CACHE STRING
    "CNA RTS reference renderer")

set(CNA_SHARP_RUNTIME_ROOT
    "${CMAKE_CURRENT_SOURCE_DIR}/../sharp-runtimenext"
    CACHE PATH "sharp-runtimenext checkout used by cna-rts")

set(CNA_BUILD_TESTS    OFF CACHE BOOL "Build CNA tests")
set(CNA_BUILD_EXAMPLES OFF CACHE BOOL "Build CNA examples")
set(SHARP_RUNTIME_BUILD_TESTS OFF CACHE BOOL "Build sharp-runtime tests")

add_subdirectory(../cnanext CNA_BUILD)

add_executable(cna-rts
    src/Program.cpp
    src/CliOptions.cpp
    src/Terrain.cpp
    src/SpatialGrid.cpp
    src/FlowField.cpp
    src/Pathfinder.cpp
    src/Formation.cpp
    src/FogOfWar.cpp
    src/Simulation.cpp
    src/JobSystem.cpp
    src/UnitRenderer.cpp
    src/TerrainRenderer.cpp
    src/TextRenderer.cpp
    src/Hud.cpp
    src/Benchmark.cpp
    src/RtsGame.cpp
)

target_include_directories(cna-rts PRIVATE src)

target_link_libraries(cna-rts PRIVATE CNA)

find_package(Threads REQUIRED)
target_link_libraries(cna-rts PRIVATE Threads::Threads)

# CNA already links the sharp-runtime components it needs, but cna-rts uses
# System::Diagnostics::Stopwatch directly for its own instrumentation, so it names the component
# rather than relying on CNA's linkage staying PUBLIC.
if(TARGET SharpRuntime::Core.Base)
    target_link_libraries(cna-rts PRIVATE SharpRuntime::Core.Base)
endif()

if(EMSCRIPTEN)
    set_target_properties(cna-rts PROPERTIES SUFFIX ".html")
    # See cna-samples/CMakeLists.txt: Game::Run() is CNA's blocking XNA loop contract, so the
    # final web artifact needs the matching Asyncify-based browser-loop suspension policy.
    target_link_libraries(cna-rts PRIVATE CNA::EmscriptenAsyncify)
    target_link_options(cna-rts PRIVATE
        -sALLOW_MEMORY_GROWTH=1
        "-sMIN_WEBGL_VERSION=2"
        "-sMAX_WEBGL_VERSION=2"
    )
endif()

if(TARGET SDL3::SDL3main)
    target_link_libraries(cna-rts PRIVATE SDL3::SDL3main)
endif()

if(WIN32)
    set_target_properties(cna-rts PROPERTIES WIN32_EXECUTABLE TRUE)
    if(COMMAND cna_copy_sdl_runtime)
        cna_copy_sdl_runtime(cna-rts)
    endif()
endif()
