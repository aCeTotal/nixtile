cmake_minimum_required(VERSION 3.22)
project(NixTile VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Enable warnings
add_compile_options(-Wall -Wextra -Wpedantic)

# Use pkg-config for Wayland and Vulkan
find_package(PkgConfig REQUIRED)

pkg_check_modules(WAYLAND REQUIRED wayland-client wayland-server wayland-protocols)
pkg_check_modules(VULKAN REQUIRED vulkan)

# Bruk wlroots via miljøvariabler satt i flake.nix
if(NOT DEFINED ENV{WLROOTS_INCLUDE_DIR})
  message(FATAL_ERROR "WLROOTS_INCLUDE_DIR is not set. Run inside 'nix develop'.")
endif()

if(NOT DEFINED ENV{WLROOTS_LIB_DIR})
  message(FATAL_ERROR "WLROOTS_LIB_DIR is not set. Run inside 'nix develop'.")
endif()

# Sørg for at clangd får riktig -I flagg via compile_commands.json
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -I$ENV{WLROOTS_INCLUDE_DIR}")

# Library paths
link_directories(
    ${WAYLAND_LIBRARY_DIRS}
    ${VULKAN_LIBRARY_DIRS}
    $ENV{WLROOTS_LIB_DIR}
)

# Kildekode
add_executable(wm
    src/main.cpp
    src/renderer/Renderer.cpp
    src/renderer/VulkanRenderer.cpp
    src/renderer/MyCustomRenderer.cpp
    src/renderer/RendererFactory.cpp
)

# Include paths
target_include_directories(wm PRIVATE
    ${WAYLAND_INCLUDE_DIRS}
    ${VULKAN_INCLUDE_DIRS}
    /nix/store/l6mx7ifbcrlzmsl72ih41j8ij475kh7a-wlroots-0.18.2/include/wlroots-0.18/wlr/
    $ENV{WLROOTS_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/include/renderer
)

# Link libraries
target_link_libraries(wm
    ${WAYLAND_LIBRARIES}
    ${VULKAN_LIBRARIES}
    wlroots
)

