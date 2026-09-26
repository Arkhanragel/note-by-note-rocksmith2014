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

# THE MOD: NoteByNote.dll (loaded by our RS_ASIO build from the game folder).
add_library(NoteByNote SHARED nbn/main.cpp nbn/game.cpp nbn/detector.cpp nbn/tap.cpp nbn/chart.cpp nbn/log.cpp)
target_link_libraries(NoteByNote PRIVATE imagehlp winmm)
target_compile_definitions(NoteByNote PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _CRT_SECURE_NO_WARNINGS)

# Detector test: C++ NoteTracker on a WAV, to compare with tools/detector/wait_sim.py analyze.
add_executable(nbn_detector_test nbn/test_detector.cpp nbn/detector.cpp)

# Dev injector: loads a DLL into the running game.
add_executable(nbn_inject injector/inject.cpp)
target_compile_definitions(nbn_inject PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
