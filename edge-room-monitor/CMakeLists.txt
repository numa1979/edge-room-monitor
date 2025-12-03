cmake_minimum_required(VERSION 3.10)
project(edge-room-monitor LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GSTREAMER REQUIRED
  gstreamer-1.0>=1.14
  gstreamer-app-1.0>=1.14
)
find_package(Threads REQUIRED)

add_executable(edge-room-monitor src/main.cpp)
target_include_directories(edge-room-monitor PRIVATE ${GSTREAMER_INCLUDE_DIRS})
target_link_libraries(edge-room-monitor PRIVATE ${GSTREAMER_LIBRARIES} Threads::Threads)
target_compile_options(edge-room-monitor PRIVATE ${GSTREAMER_CFLAGS_OTHER})
