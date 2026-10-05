# Note-by-Note mod: native (C++) part.
# Rocksmith2014.exe is a 32-bit program, so everything here MUST be built for Win32 (x86).
# Build with: mod\build.ps1

cmake_minimum_required(VERSION 3.20)
project(NoteByNote LANGUAGES C CXX)

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
  message(FATAL_ERROR "Build for Win32 (x86): cmake -A Win32. The game is a 32-bit process.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT, so the DLL doesn't depend on the Visual C++ runtime being installed.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

# MinHook: intercepts ("hooks") functions inside the game.
add_subdirectory(../external/minhook minhook EXCLUDE_FROM_ALL)

# The probe: a DLL loaded into the game that logs state and runs speed experiments.
add_library(NoteByNoteProbe SHARED probe/probe.cpp)
target_link_libraries(NoteByNoteProbe PRIVATE minhook imagehlp winmm)
target_compile_definitions(NoteByNoteProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)

# Dear ImGui (the in-game banner and menu), with its Direct3D 9 + Win32 backends.
set(IMGUI_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../external/imgui)
add_library(imgui STATIC
  ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp ${IMGUI_DIR}/imgui_widgets.cpp
  ${IMGUI_DIR}/imgui_demo.cpp
  ${IMGUI_DIR}/backends/imgui_impl_dx9.cpp ${IMGUI_DIR}/backends/imgui_impl_win32.cpp)
target_include_directories(imgui PUBLIC ${IMGUI_DIR} ${IMGUI_DIR}/backends)
# No demo window, no gamepad polling (the game owns the controllers).
target_compile_definitions(imgui PUBLIC IMGUI_DISABLE_DEMO_WINDOWS IMGUI_IMPL_WIN32_DISABLE_GAMEPAD)

# THE MOD: NoteByNote.dll (loaded by our RS_ASIO build from the game folder).
add_library(NoteByNote SHARED nbn/main.cpp nbn/game.cpp nbn/detector.cpp nbn/tap.cpp nbn/chart.cpp nbn/log.cpp nbn/overlay.cpp nbn/theme.cpp nbn/music.cpp nbn/technique.cpp nbn/hint.cpp nbn/stringid.cpp nbn/startup.cpp nbn/fastintro.cpp nbn/crashfix.cpp nbn/crashlog.cpp nbn/report.cpp nbn/stats.cpp nbn/tuning.cpp nbn/picking.cpp)
target_link_libraries(NoteByNote PRIVATE imgui minhook imagehlp winmm)
target_compile_definitions(NoteByNote PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

# Detector test: C++ NoteTracker on a WAV, to compare with tools/detector/wait_sim.py analyze.
add_executable(nbn_detector_test nbn/test_detector.cpp nbn/detector.cpp)
add_executable(nbn_music_test nbn/test_music.cpp nbn/music.cpp nbn/technique.cpp)
add_executable(nbn_hint_test nbn/test_hint.cpp nbn/hint.cpp nbn/music.cpp)
# String identification (which string a note was played on): synthetic test, or a take of tools/detector/string_id.py.
add_executable(nbn_stringid_test nbn/test_stringid.cpp nbn/stringid.cpp nbn/detector.cpp)
# Trouble spots: the trouble of a try, the heat per phrase, saving and loading a record.
add_executable(nbn_stats_test nbn/test_stats.cpp nbn/stats.cpp)
# Tuning check: steady pitch, when a string or the guitar is out of tune, the words (--wav: measure recordings).
add_executable(nbn_tuning_test nbn/test_tuning.cpp nbn/tuning.cpp nbn/music.cpp nbn/detector.cpp)
# Pick strokes: suggested from the rhythm, or the song's own when they make sense.
add_executable(nbn_picking_test nbn/test_picking.cpp nbn/picking.cpp nbn/chart.cpp nbn/music.cpp nbn/technique.cpp)

# Dev injector: loads a DLL into the running game.
add_executable(nbn_inject injector/inject.cpp)
target_compile_definitions(nbn_inject PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
