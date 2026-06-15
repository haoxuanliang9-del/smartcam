cmake_minimum_required(VERSION 3.16)
project(smartcam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(ENABLE_DEBUG "Enable debug logging" OFF)

if(ENABLE_DEBUG)
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libavfilter
)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/concurrentqueue
)

set(SMARTCAM_SOURCES
    src/main.cpp
    src/app/main_service.cpp
    src/common/config.cpp
    src/hal/video_file_source.cpp
    src/hal/i2c_hal.cpp
    src/hal/spi_hal.cpp
    src/modules/camera/camera_capture.cpp
    src/modules/audio/audio_capture.cpp
    src/modules/sensor/sensor_module.cpp
    src/modules/display/oled_display.cpp
    src/modules/streaming/rtsp_server.cpp
)

add_executable(smartcam ${SMARTCAM_SOURCES})

target_link_libraries(smartcam
    yaml-cpp
    spdlog::spdlog
    fmt
    pthread
    PkgConfig::ALSA
)

target_link_options(smartcam PRIVATE -Wl,--no-as-needed)
target_link_libraries(smartcam
    avcodec
    avformat
    avutil
    swscale
    avfilter
)

target_include_directories(smartcam PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
)

install(TARGETS smartcam DESTINATION bin)
install(DIRECTORY config/ DESTINATION /etc/smartcam)
