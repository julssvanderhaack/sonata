cmake_minimum_required(VERSION 3.20)
project(sonata VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# Local builds are tuned for the machine they run on; release zips must run on
# any x86-64 PC from the last ~15 years (x86-64-v2: SSE4.2, POPCNT).
option(SONATA_NATIVE "Optimise for this machine's CPU" ON)
if(SONATA_NATIVE)
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=native")
else()
  set(CMAKE_CXX_FLAGS_RELEASE "-O2 -march=x86-64-v2 -mtune=generic")
endif()

find_package(Qt6 REQUIRED COMPONENTS Widgets Network DBus Concurrent)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET libavformat libavcodec libavutil libswresample)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
pkg_check_modules(TAGLIB REQUIRED IMPORTED_TARGET taglib>=2.0)

add_executable(sonata
  src/main.cpp
  src/ring.h
  src/scope.h
  src/eq.h src/eq.cpp
  src/spectrum.h src/spectrum.cpp
  src/engine.h src/engine.cpp
  src/meta.h src/meta.cpp
  src/lyrics.h src/lyrics.cpp
  src/tagedit.h src/tagedit.cpp
  src/lyricsview.h src/lyricsview.cpp
  src/playlist.h src/playlist.cpp
  src/eqpanel.h src/eqpanel.cpp
  src/mpris.h src/mpris.cpp
  src/theme.h src/theme.cpp
  src/widgets.h src/widgets.cpp
  src/mainwindow.h src/mainwindow.cpp
  data/sonata.qrc
)
target_link_libraries(sonata PRIVATE Qt6::Widgets Qt6::Network Qt6::DBus Qt6::Concurrent
  PkgConfig::FFMPEG PkgConfig::PIPEWIRE PkgConfig::TAGLIB)
install(TARGETS sonata RUNTIME DESTINATION bin)
install(FILES data/sonata.desktop DESTINATION share/applications)
install(FILES data/sonata.svg DESTINATION share/icons/hicolor/scalable/apps)
install(FILES data/sonata.svg DESTINATION share/icons/hicolor/scalable/apps)
