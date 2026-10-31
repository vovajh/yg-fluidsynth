cmake_minimum_required(VERSION 3.22.1)
project(yg_fluidsynth C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# --- FluidSynth как статическая библиотека, без аудио-драйверов и лишних зависимостей ---
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
foreach(opt libsndfile alsa pulseaudio pipewire jack dbus readline network oss sdl3 ladspa midishare
            openmp aufile systemd oboe opensles portaudio dsound wasapi waveout winmidi coreaudio coremidi)
  set(enable-${opt} OFF CACHE BOOL "" FORCE)
endforeach()
# FluidSynth ищет сабмодули относительно CMAKE_SOURCE_DIR — при add_subdirectory подсказываем пути явно
set(SIGNALSMITH_AUDIO_BASICS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fluidsynth/signalsmith-audio-basics" CACHE PATH "" FORCE)
# FluidSynth ищет Signalsmith (лимитер + реверб) относительно CMAKE_SOURCE_DIR — при add_subdirectory подсказываем путь.
set(SIGNALSMITH_AUDIO_BASICS_PATH "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fluidsynth/signalsmith-audio-basics")
add_subdirectory(third_party/fluidsynth fluidsynth EXCLUDE_FROM_ALL)

# --- сам плагин ---
add_library(yg_fluidsynth SHARED
  src/plugin.cpp
  src/renderer.cpp
  src/default_sf2.c)
target_include_directories(yg_fluidsynth PRIVATE
  include src
  third_party/fluidsynth/include
  ${CMAKE_CURRENT_BINARY_DIR}/fluidsynth/include)
target_link_libraries(yg_fluidsynth PRIVATE libfluidsynth)
target_compile_options(yg_fluidsynth PRIVATE
  $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti>
  -fvisibility=hidden -ffunction-sections -fdata-sections)
target_link_options(yg_fluidsynth PRIVATE -Wl,--exclude-libs,ALL -Wl,--gc-sections)
if(ANDROID)
  target_link_options(yg_fluidsynth PRIVATE -Wl,-z,max-page-size=16384)
  if(NOT ANDROID_STL)  # не NDK-тулчейн (например Termux clang): libc++ должна быть внутри .so
    target_link_options(yg_fluidsynth PRIVATE -static-libstdc++)
  endif()
else()
  find_package(Threads REQUIRED)
  find_library(YG_ATOMIC atomic)
  target_link_libraries(yg_fluidsynth PRIVATE Threads::Threads m)
  if(YG_ATOMIC)
    target_link_libraries(yg_fluidsynth PRIVATE ${YG_ATOMIC})
  endif()
  target_link_options(yg_fluidsynth PRIVATE -static-libstdc++ -static-libgcc)
endif()

# --- тест на хосте (x86_64): заглушка Yougram, которая грузит плагин и гоняет настоящий рендер ---
option(YG_NATIVE_TEST "build mock host for desktop tests" OFF)
if(YG_NATIVE_TEST)
  add_executable(mock_host test/mock_host.cpp)
  target_include_directories(mock_host PRIVATE include)
  target_link_libraries(mock_host PRIVATE dl pthread)
endif()
