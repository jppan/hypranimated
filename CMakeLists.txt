cmake_minimum_required(VERSION 3.27)

project(
  hypranimated
  DESCRIPTION "Niri-style shader window animations for Hyprland"
  VERSION 1.0)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

file(GLOB_RECURSE SRC "src/*.cpp")

add_library(hypranimated SHARED ${SRC})

find_package(PkgConfig REQUIRED)
pkg_check_modules(
  deps
  REQUIRED
  IMPORTED_TARGET
  hyprland
  libdrm
  pangocairo
  pixman-1
  wayland-server)

target_link_libraries(hypranimated PRIVATE PkgConfig::deps EGL GLESv2 rt)
target_link_options(hypranimated PRIVATE -Wl,-Bsymbolic-functions)
install(TARGETS hypranimated)
