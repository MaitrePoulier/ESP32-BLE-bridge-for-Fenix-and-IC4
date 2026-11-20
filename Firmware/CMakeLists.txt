# ESP32 BaseBoard IC4 - firmware (ESP-IDF v6.1)
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Only the components actually used are pulled into the build
idf_build_set_property(MINIMAL_BUILD ON)

project(esp32-base-ic4)
