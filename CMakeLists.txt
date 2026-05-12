cmake_minimum_required(VERSION 3.16)
project(smartcam VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTS "Build unit tests" ON)
option(ENABLE_DEBUG "Enable debug logging" OFF)

if(ENABLE_DEBUG)
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_DEBUG)
else()
    add_compile_definitions(SPDLOG_ACTIVE_LEVEL=SPDLOG_LEVEL_INFO)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")

find_package(PkgConfig REQUIRED)
find_package(OpenCV REQUIRED)
pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
    libavcodec
    libavformat
    libavutil
    libswscale
    libavfilter
)
find_package(CURL REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(spdlog REQUIRED)
find_package(asio CONFIG REQUIRED)

include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party/concurrentqueue
    ${CMAKE_SOURCE_DIR}/third_party/BS_thread_pool
)

add_executable(smartcam
    src/main.cpp
    src/app/main_service.cpp
    src/common/config.cpp
    src/hal/camera_hal.cpp
    src/hal/i2c_hal.cpp
    src/modules/camera/camera_capture.cpp
    src/modules/sensor/sensor_module.cpp
    src/modules/analysis/intelligent_analysis.cpp
    src/modules/streaming/rtsp_server.cpp
    src/modules/ota/ota_manager.cpp
)

target_link_libraries(smartcam
    ${OpenCV_LIBS}
    PkgConfig::FFMPEG
    CURL::libcurl
    OpenSSL::SSL
    OpenSSL::Crypto
    yaml-cpp
    spdlog::spdlog
    asio::asio
    pthread
)

target_include_directories(smartcam PRIVATE
    ${FFMPEG_INCLUDE_DIRS}
)

install(TARGETS smartcam DESTINATION bin)
install(DIRECTORY config/ DESTINATION /etc/smartcam)

if(BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
