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

link_directories(/opt/nvidia/deepstream/deepstream/lib)

add_executable(edge-room-monitor src/main.cpp)
target_include_directories(edge-room-monitor PRIVATE 
    ${GSTREAMER_INCLUDE_DIRS}
    /opt/nvidia/deepstream/deepstream/sources/includes
)
target_link_libraries(edge-room-monitor PRIVATE 
    ${GSTREAMER_LIBRARIES} 
    Threads::Threads
    nvdsgst_meta
    nvds_meta
)
target_compile_options(edge-room-monitor PRIVATE ${GSTREAMER_CFLAGS_OTHER})

# YOLOv8 custom parser library
add_library(nvdsinfer_yolov8 SHARED src/yolov8_parser.cpp)
target_include_directories(nvdsinfer_yolov8 PRIVATE 
    /opt/nvidia/deepstream/deepstream/sources/includes
    /usr/local/cuda/include
)
set_target_properties(nvdsinfer_yolov8 PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
    OUTPUT_NAME "nvdsinfer_custom_impl_yolov8"
)
