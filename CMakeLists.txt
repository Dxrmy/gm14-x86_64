cmake_minimum_required(VERSION 3.16)
project(gm14_x86_64 C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    add_compile_options(/W4 /EHsc /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wno-unused-parameter)
endif()

# Core library
add_library(gm14_core STATIC
    dw.cpp
    dw.hpp
    vm.cpp
    vm.hpp
    audio.cpp
    audio.hpp
    stb_vorbis_impl.c
    stb_vorbis.c
    stb_image.h
    stb_image_write.h
)

if(WIN32)
    target_link_libraries(gm14_core PUBLIC winmm)
elseif(UNIX AND NOT APPLE)
    find_package(ALSA)
    if(ALSA_FOUND)
        target_link_libraries(gm14_core PUBLIC ALSA::ALSA)
    else()
        target_compile_definitions(gm14_core PUBLIC GM14_NO_ALSA)
    endif()
    target_link_libraries(gm14_core PUBLIC pthread)
endif()

# Host utilities
add_executable(gm14_host main_host.cpp dw.cpp)
if(WIN32)
    target_link_libraries(gm14_host PRIVATE winmm)
endif()

add_executable(vm_host vm_host.cpp)
target_link_libraries(vm_host PRIVATE gm14_core)

# Test suite
add_executable(vm_tests vm_tests.cpp)
target_link_libraries(vm_tests PRIVATE gm14_core)

enable_testing()
add_test(NAME vm_unit_tests COMMAND vm_tests)
