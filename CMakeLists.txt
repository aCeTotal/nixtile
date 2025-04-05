cmake_minimum_required(VERSION 3.26)
project(nixtile LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(nixtile
  src/main.cpp
  src/compositor/Compositor.cpp
  src/compositor/WaylandServer.cpp
)

# Find Wayland and Vulkan
find_package(PkgConfig REQUIRED)
pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-server wayland-egl)
pkg_check_modules(VULKAN REQUIRED vulkan)

target_include_directories(nixtile PRIVATE
  ${WAYLAND_INCLUDE_DIRS}
  ${VULKAN_INCLUDE_DIRS}
)

target_link_libraries(nixtile
  ${WAYLAND_LIBRARIES}
  ${VULKAN_LIBRARIES}
)

