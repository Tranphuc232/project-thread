# Project ESP-IDF cho ESP32-H2: nhan chuoi qua UART tu ESP32-C6
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(h2_uart_receiver)
