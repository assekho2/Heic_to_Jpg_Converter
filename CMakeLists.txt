# Build for the graphical HEIC -> JPG converter (heic_converter_gui).
#
# The command-line tool still builds via the Makefile / scripts/build_static.sh.
# This CMake build is only for the Dear ImGui desktop app, which pulls GLFW,
# Dear ImGui and the native file dialog via FetchContent and links the same
# libheif / libde265 / libjpeg engine the CLI uses.
#
# Prerequisite: run scripts/build_static.sh once first (`make static`) so the
# static libheif (and on macOS libde265/libjpeg) live in build/deps/prefix.
#
#   cmake -S . -B build/gui -G Ninja -DCMAKE_BUILD_TYPE=Release
#   cmake --build build/gui

cmake_minimum_required(VERSION 3.16)
project(heic_converter_gui CXX C)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)

# ---------------------------------------------------------------------------
# Windowing + UI dependencies (downloaded and built statically)
# ---------------------------------------------------------------------------
set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)

FetchContent_Declare(imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG v1.91.5
    GIT_SHALLOW TRUE)

set(NFD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(nfd
    GIT_REPOSITORY https://github.com/btzy/nativefiledialog-extended.git
    GIT_TAG v1.2.1
    GIT_SHALLOW TRUE)

FetchContent_MakeAvailable(glfw nfd)
FetchContent_MakeAvailable(imgui)   # imgui has no CMake of its own

# Dear ImGui as a static lib (core + GLFW/OpenGL3 backends)
add_library(imgui STATIC
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp)
target_include_directories(imgui PUBLIC
    ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_link_libraries(imgui PUBLIC glfw)

find_package(OpenGL REQUIRED)

# ---------------------------------------------------------------------------
# Converter engine dependencies (reuse what scripts/build_static.sh produced)
# ---------------------------------------------------------------------------
set(DEPS_PREFIX "${CMAKE_SOURCE_DIR}/build/deps/prefix")
if(NOT EXISTS "${DEPS_PREFIX}/include/libheif/heif.h")
    message(FATAL_ERROR
        "libheif not found in ${DEPS_PREFIX}. Run scripts/build_static.sh "
        "(make static) once before configuring the GUI.")
endif()

add_executable(heic_converter_gui WIN32
    gui/main.cpp
    converter_core.cpp)

target_include_directories(heic_converter_gui PRIVATE
    "${CMAKE_SOURCE_DIR}" "${DEPS_PREFIX}/include")
target_link_libraries(heic_converter_gui PRIVATE imgui nfd OpenGL::GL)
target_compile_definitions(heic_converter_gui PRIVATE LIBHEIF_STATIC_BUILD=1)

if(WIN32)
    # MinGW/UCRT64: de265 + jpeg + zlib live on the toolchain's default paths.
    # Resolve them as -l flags so -static prefers the .a archives, yielding a
    # single self-contained .exe (matches the CLI's static build).
    target_link_directories(heic_converter_gui PRIVATE "${DEPS_PREFIX}/lib")
    target_link_libraries(heic_converter_gui PRIVATE heif de265 jpeg z)
    target_link_options(heic_converter_gui PRIVATE -static -mwindows)
    target_compile_definitions(heic_converter_gui PRIVATE LIBDE265_STATIC_BUILD)
elseif(APPLE)
    # build_static.sh installs libheif + libde265 into the prefix; libjpeg comes
    # from Homebrew's keg-only jpeg-turbo.
    execute_process(COMMAND brew --prefix jpeg-turbo
        OUTPUT_VARIABLE JT_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET)
    if(NOT JT_PREFIX OR NOT EXISTS "${JT_PREFIX}/lib/libjpeg.a")
        execute_process(COMMAND brew --prefix
            OUTPUT_VARIABLE BREW_PREFIX OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        set(JT_PREFIX "${BREW_PREFIX}/opt/jpeg-turbo")
    endif()
    target_include_directories(heic_converter_gui PRIVATE "${JT_PREFIX}/include")
    target_link_libraries(heic_converter_gui PRIVATE
        "${DEPS_PREFIX}/lib/libheif.a"
        "${DEPS_PREFIX}/lib/libde265.a"
        "${JT_PREFIX}/lib/libjpeg.a"
        z)
endif()
