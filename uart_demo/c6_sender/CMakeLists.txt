# Project ESP-IDF cho ESP32-C6: gui chuoi qua UART sang ESP32-H2
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(MINIMAL_BUILD ON)
project(c6_uart_sender)
