cmake_minimum_required(VERSION 3.20)
project(Project6MeanCoroutine LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

add_library(mean3_core STATIC
    src/mean_coroutine.cpp)
target_include_directories(mean3_core PUBLIC src)

add_executable(mean3_demo
    src/main.cpp)
target_link_libraries(mean3_demo PRIVATE mean3_core)

add_executable(mean3_tests tests/tests.cpp)
target_link_libraries(mean3_tests PRIVATE mean3_core)

foreach(target mean3_core mean3_demo mean3_tests)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endforeach()

enable_testing()
add_test(NAME mean3_tests COMMAND mean3_tests)
