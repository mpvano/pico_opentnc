cmake_minimum_required(VERSION 3.13)

add_library(z80emu STATIC
    z80emu.c
)

target_include_directories(z80emu PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
