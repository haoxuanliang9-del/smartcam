cmake_minimum_required(VERSION 3.16)
project(smartcam VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_PREFIX_PATH /usr)

option(ENABLE_DEBUG "Enable debug logging" OFF)

if(ENABLE_DEBUG)
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libavfilter
    libswresample
)
pkg_check_modules(ALSA REQUIRED IMPORTED_TARGET alsa)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)

# OpenCV — optional, for video enhancement
option(ENABLE_VIDEO_ENHANCE "Enable OpenCV-based video enhancement" ON)
if(ENABLE_VIDEO_ENHANCE)
    find_package(OpenCV QUIET COMPONENTS core imgproc photo)
    if(OpenCV_FOUND)
        add_compile_definitions(HAS_OPENCV)
        message(STATUS "OpenCV found: video enhancement enabled")
    else()
        message(STATUS "OpenCV NOT found: video enhancement will be a no-op stub")
    endif()
endif()

include_directories(
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/concurrentqueue
)

# RNNoise — neural noise suppression (vendored, always compiled)
add_library(rnnoise STATIC
    third_party/rnnoise/denoise.c
    third_party/rnnoise/rnn.c
    third_party/rnnoise/rnn_data.c
    third_party/rnnoise/pitch.c
    third_party/rnnoise/celt_lpc.c
    third_party/rnnoise/kiss_fft.c
)
target_include_directories(rnnoise PUBLIC third_party/rnnoise)
target_link_libraries(rnnoise PUBLIC m)

set(SMARTCAM_SOURCES
    src/main.cpp
    src/app/main_service.cpp
    src/common/config.cpp
    src/hal/v4l2_source.cpp
    src/hal/i2c_hal.cpp
    src/hal/spi_hal.cpp
    src/modules/camera/camera_capture.cpp
    src/modules/video/video_processor.cpp
    src/modules/audio/audio_capture.cpp
    src/modules/audio/audio_processor.cpp
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
    rnnoise
    PkgConfig::ALSA
)

target_link_options(smartcam PRIVATE -Wl,--no-as-needed)
target_link_libraries(smartcam
    avcodec
    avformat
    avutil
    swscale
    avfilter
    swresample
)

target_include_directories(smartcam PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
)

if(OpenCV_FOUND)
    target_link_libraries(smartcam ${OpenCV_LIBS})
endif()

install(TARGETS smartcam DESTINATION bin)
install(DIRECTORY config/ DESTINATION /etc/smartcam)
