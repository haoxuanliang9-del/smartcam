find_package(GTest REQUIRED)

add_executable(smartcam_tests
    test_config.cpp
    test_sensor.cpp
    test_analysis.cpp
)

target_link_libraries(smartcam_tests
    GTest::GTest
    GTest::Main
    yaml-cpp
    spdlog::spdlog
)

add_test(NAME smartcam_tests COMMAND smartcam_tests)
