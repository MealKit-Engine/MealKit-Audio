# mkcontract: the contract between MealKit tools (MealKit-Contract). Built standalone it includes tests; added by another project with
# add_subdirectory, only the libraries.
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
# Targets:
#   mkcontract          text files, user folder, tool manifests, live connection info, module list (standard library only)
#   mkcontract_client   live connection client (sockets). Tools link it; the engine doesn't
cmake_minimum_required(VERSION 3.25)
project(mkcontract VERSION 0.1.0 LANGUAGES CXX)

option(MKCONTRACT_BUILD_TESTS "Build mkcontract tests" ${PROJECT_IS_TOP_LEVEL})

add_library(mkcontract STATIC
    include/mkcontract/Text.h
    include/mkcontract/Files.h
    include/mkcontract/User.h
    include/mkcontract/Tool.h
    include/mkcontract/Json.h
    include/mkcontract/Live.h
    include/mkcontract/Modules.h
    src/Text.cpp
    src/Files.cpp
    src/User.cpp
    src/Tool.cpp
    src/Json.cpp
    src/Live.cpp
    src/Modules.cpp)
target_include_directories(mkcontract PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(mkcontract PUBLIC cxx_std_20)
target_compile_definitions(mkcontract PUBLIC MKCONTRACT_VERSION="${PROJECT_VERSION}")

add_library(mkcontract_client STATIC include/mkcontract/Client.h src/Client.cpp)
target_link_libraries(mkcontract_client PUBLIC mkcontract)
if(WIN32)
    target_link_libraries(mkcontract_client PRIVATE ws2_32)
endif()

foreach(target mkcontract mkcontract_client)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
        target_compile_definitions(${target} PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra)
    endif()
endforeach()

if(MKCONTRACT_BUILD_TESTS)
    enable_testing()
    add_executable(mkcontract_tests tests/ContractTests.cpp)
    target_link_libraries(mkcontract_tests PRIVATE mkcontract mkcontract_client)
    if(MSVC)
        target_compile_options(mkcontract_tests PRIVATE /W4 /permissive- /utf-8)
        target_compile_definitions(mkcontract_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(mkcontract_tests PRIVATE -Wall -Wextra)
    endif()
    add_test(NAME mkcontract_tests COMMAND mkcontract_tests WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
endif()
