# rv3032-driver — a small, MCU-independent C driver for the Micro Crystal RV-3032-C7 RTC.
cmake_minimum_required(VERSION 3.13)
project(rv3032_driver C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The library: pure C, no platform dependencies. Add it to your firmware and implement the
# rv3032_bus_t I2C callback for your MCU; see README.md and examples/.
add_library(rv3032 STATIC src/rv3032.c)
target_include_directories(rv3032 PUBLIC include)

# Host unit test (time, alarm, flags, EEPROM procedure) against a fake chip. Opt out
# with -DRV3032_BUILD_TESTS=OFF.
option(RV3032_BUILD_TESTS "Build the host unit test" ON)
if(RV3032_BUILD_TESTS)
    enable_testing()
    add_executable(test_rv3032 test/test_rv3032.c)
    target_link_libraries(test_rv3032 PRIVATE rv3032)
    target_include_directories(test_rv3032 PRIVATE test)
    if(NOT MSVC)
        target_compile_options(test_rv3032 PRIVATE -Wall -Wextra -Wpedantic)
    endif()
    add_test(NAME test_rv3032 COMMAND test_rv3032)
endif()
