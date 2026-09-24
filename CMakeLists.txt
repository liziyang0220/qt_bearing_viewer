cmake_minimum_required(VERSION 3.21)
project(qt_bearing_viewer VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Widgets Charts Sql)
find_package(PkgConfig REQUIRED)
pkg_check_modules(FFTW3 REQUIRED IMPORTED_TARGET fftw3)

add_executable(qt_bearing_viewer
  src/main.cpp
  src/mainwindow.cpp
  src/mainwindow.h
  src/dataworker.cpp
  src/dataworker.h
  src/analysisframe.h
)

target_link_libraries(qt_bearing_viewer PRIVATE
  Qt6::Widgets
  Qt6::Charts
  Qt6::Sql
  PkgConfig::FFTW3
)

target_compile_options(qt_bearing_viewer PRIVATE -Wall -Wextra -Wpedantic)

install(TARGETS qt_bearing_viewer RUNTIME DESTINATION bin)
