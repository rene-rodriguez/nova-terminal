# Fangs — native anti-Warp terminal (Phase 1: terminal core).
# Mirrors the proven ghostling build: FetchContent pulls raylib + libghostty-vt
# (pinned ghostty commit, built via `zig build`), embeds the font as a C header,
# and links src/*.c into the `fangs` binary.
#
# Build:
#   Linux/CachyOS:  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
#   macOS:          scripts/macos-build.sh        (handles the Zig/macOS-26 SDK workaround)
# Requires: CMake 3.19+, Ninja, a C compiler, and Zig 0.15.2 on PATH.
cmake_minimum_required(VERSION 3.19)
project(fangs C)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include(FetchContent)

# Resolve Zig from the active environment (the macOS build script puts a pinned
# 0.15.2 + shim first on PATH).
find_program(FANGS_ZIG_EXECUTABLE zig REQUIRED NO_CACHE)
set(ZIG_EXECUTABLE "${FANGS_ZIG_EXECUTABLE}" CACHE FILEPATH "Path to the Zig compiler" FORCE)

# --- Raylib 5.5 (system, else fetched) ---------------------------------------
set(RAYLIB_VERSION 5.5)
find_package(raylib ${RAYLIB_VERSION} QUIET)
if (NOT raylib_FOUND)
  FetchContent_Declare(raylib
    DOWNLOAD_EXTRACT_TIMESTAMP OFF
    URL https://github.com/raysan5/raylib/archive/refs/tags/${RAYLIB_VERSION}.tar.gz)
  FetchContent_GetProperties(raylib)
  if (NOT raylib_POPULATED)
    set(FETCHCONTENT_QUIET NO)
    FetchContent_MakeAvailable(raylib)
  endif()
endif()

# --- libghostty-vt (pinned ghostty commit; `zig build lib-vt`) ---------------
FetchContent_Declare(ghostty
  GIT_REPOSITORY https://github.com/ghostty-org/ghostty.git
  GIT_TAG ae52f97dcac558735cfa916ea3965f247e5c6e9e)
FetchContent_MakeAvailable(ghostty)

# --- AI transport deps: libcurl (HTTP/SSE) + pthreads (worker thread) --------
find_package(CURL REQUIRED)
set(THREADS_PREFER_PTHREAD_FLAG ON)
find_package(Threads REQUIRED)

# --- Embed fonts as C headers (avoids C23 #embed) ---------------------------
set(FONT_INPUT  "${CMAKE_SOURCE_DIR}/assets/JetBrainsMono-Regular.ttf")
set(FONT_HEADER "${CMAKE_BINARY_DIR}/font_jetbrains_mono.h")
add_custom_command(
  OUTPUT "${FONT_HEADER}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${FONT_INPUT}
    -DOUTPUT=${FONT_HEADER}
    -DARRAY_NAME=font_jetbrains_mono
    -P ${CMAKE_SOURCE_DIR}/cmake/bin2header.cmake
  DEPENDS "${FONT_INPUT}" "${CMAKE_SOURCE_DIR}/cmake/bin2header.cmake"
  COMMENT "Generating font_jetbrains_mono.h")

set(FONT_BOLD_INPUT  "${CMAKE_SOURCE_DIR}/assets/JetBrainsMono-Bold.ttf")
set(FONT_BOLD_HEADER "${CMAKE_BINARY_DIR}/font_jetbrains_mono_bold.h")
add_custom_command(
  OUTPUT "${FONT_BOLD_HEADER}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT=${FONT_BOLD_INPUT}
    -DOUTPUT=${FONT_BOLD_HEADER}
    -DARRAY_NAME=font_jetbrains_mono_bold
    -P ${CMAKE_SOURCE_DIR}/cmake/bin2header.cmake
  DEPENDS "${FONT_BOLD_INPUT}" "${CMAKE_SOURCE_DIR}/cmake/bin2header.cmake"
  COMMENT "Generating font_jetbrains_mono_bold.h")

# --- fangs ----------------------------------------------------------------
add_executable(fangs
  src/main.c
  src/pty.c
  src/config.c
  src/layout.c
  src/ui_sidebar.c
  src/ui_sidebar_model.c
  src/ui_settings.c
  src/term_engine.c
  src/ai_http.c
  src/sse.c
  src/context.c
  src/redact.c
  src/cmdextract.c
  src/ui_inline.c
  src/inline_cmd.c
  src/cmdblocks.c
  src/cmdblocks_osc.c
  src/theme.c
  src/ui_theme.c
  src/cJSON.c
  src/ai_block.c
  src/session.c
  src/pane.c
  src/ui_toast.c
  src/ui_effects.c
  "${FONT_HEADER}"
  "${FONT_BOLD_HEADER}")
target_compile_features(fangs PRIVATE c_std_11)
target_include_directories(fangs PRIVATE
  "${CMAKE_BINARY_DIR}"          # generated font header
  "${CMAKE_SOURCE_DIR}/src")
target_link_libraries(fangs raylib ghostty-vt CURL::libcurl Threads::Threads)

# macOS: raylib windowing/input/rendering frameworks.
if (APPLE)
  target_link_libraries(fangs "-framework IOKit" "-framework Cocoa" "-framework OpenGL")
endif()
# Linux: libutil provides forkpty().
if (UNIX AND NOT APPLE)
  target_link_libraries(fangs util)
endif()

# --- tests -------------------------------------------------------------------
enable_testing()

add_executable(config_tests
  tests/config_tests.c
  src/config.c)
target_compile_features(config_tests PRIVATE c_std_11)
target_include_directories(config_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME config_tests COMMAND config_tests)

add_executable(layout_tests
  tests/layout_tests.c
  src/layout.c)
target_compile_features(layout_tests PRIVATE c_std_11)
target_include_directories(layout_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME layout_tests COMMAND layout_tests)

add_executable(ui_sidebar_model_tests
  tests/ui_sidebar_model_tests.c
  src/ui_sidebar_model.c)
target_compile_features(ui_sidebar_model_tests PRIVATE c_std_11)
target_include_directories(ui_sidebar_model_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME ui_sidebar_model_tests COMMAND ui_sidebar_model_tests)

add_executable(sse_tests
  tests/sse_tests.c
  src/sse.c
  src/cJSON.c)
target_compile_features(sse_tests PRIVATE c_std_11)
target_include_directories(sse_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME sse_tests COMMAND sse_tests)

add_executable(redact_tests
  tests/redact_tests.c
  src/redact.c)
target_compile_features(redact_tests PRIVATE c_std_11)
target_include_directories(redact_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME redact_tests COMMAND redact_tests)

add_executable(cmdextract_tests
  tests/cmdextract_tests.c
  src/cmdextract.c)
target_compile_features(cmdextract_tests PRIVATE c_std_11)
target_include_directories(cmdextract_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME cmdextract_tests COMMAND cmdextract_tests)

add_executable(inline_cmd_tests
  tests/inline_cmd_tests.c
  src/inline_cmd.c)
target_compile_features(inline_cmd_tests PRIVATE c_std_11)
target_include_directories(inline_cmd_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME inline_cmd_tests COMMAND inline_cmd_tests)

add_executable(theme_tests
  tests/theme_tests.c
  src/theme.c)
target_compile_features(theme_tests PRIVATE c_std_11)
target_include_directories(theme_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME theme_tests COMMAND theme_tests)

add_executable(cmdblocks_osc_tests
  tests/cmdblocks_osc_tests.c
  src/cmdblocks_osc.c)
target_compile_features(cmdblocks_osc_tests PRIVATE c_std_11)
target_include_directories(cmdblocks_osc_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME cmdblocks_osc_tests COMMAND cmdblocks_osc_tests)

# --- E1 (§15): ai_block ------------------------------------------------------
add_executable(ai_block_tests
  tests/ai_block_tests.c
  src/ai_block.c)
target_compile_features(ai_block_tests PRIVATE c_std_11)
target_include_directories(ai_block_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME ai_block_tests COMMAND ai_block_tests)

# --- E2 (§16): pane tree tests (pure, no session link needed) ---------------
add_executable(pane_tests
  tests/pane_tests.c
  src/pane.c)
target_compile_features(pane_tests PRIVATE c_std_11)
target_include_directories(pane_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME pane_tests COMMAND pane_tests)

# --- E5 (§18): Toast notifications (pure logic) -------------------------------
add_executable(toast_tests
  tests/toast_tests.c
  src/ui_toast.c)
target_compile_features(toast_tests PRIVATE c_std_11)
target_include_directories(toast_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
add_test(NAME toast_tests COMMAND toast_tests)

# --- E3 (§17): UI theme derive -----------------------------------------------
add_executable(ui_theme_tests
  tests/ui_theme_tests.c
  src/ui_theme.c
  src/theme.c)
target_compile_features(ui_theme_tests PRIVATE c_std_11)
target_include_directories(ui_theme_tests PRIVATE "${CMAKE_SOURCE_DIR}/src")
if (UNIX AND NOT APPLE)
  target_link_libraries(ui_theme_tests m)
endif()
add_test(NAME ui_theme_tests COMMAND ui_theme_tests)

add_test(
  NAME cmake_linux_link_tests
  COMMAND bash "${CMAKE_SOURCE_DIR}/tests/cmake_linux_link_tests.sh")

add_test(
  NAME install_script_tests
  COMMAND bash "${CMAKE_SOURCE_DIR}/tests/install_script_tests.sh")

add_test(
  NAME macos_app_install_docs_tests
  COMMAND bash "${CMAKE_SOURCE_DIR}/tests/macos_app_install_docs_tests.sh")

add_test(
  NAME macos_release_lib_tests
  COMMAND bash "${CMAKE_SOURCE_DIR}/tests/macos_release_lib_tests.sh")

add_test(
  NAME release_workflow_tests
  COMMAND bash "${CMAKE_SOURCE_DIR}/tests/release_workflow_tests.sh")
